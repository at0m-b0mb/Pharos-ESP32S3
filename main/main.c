/* Pharos - application entry
 *
 * The boot story is deliberately short and its order is deliberate:
 *
 *   1. Bring the board up (PMU, display, touch) through the vendor BSP.
 *   2. Verify the transmit fence before anything else runs. A device that
 *      cannot prove it is receive-only has no business scanning; if the fence
 *      is not clean, we say so on screen and refuse to launch a radio lens.
 *   3. Hand control to the UI, which reads the self-registered lens registry
 *      and builds the Lamp Room dial from it. main.c does not know the names
 *      of any lenses - that is the whole point of the registry.
 *
 * The two cores are split by role: the analytics task drains the ingest bus
 * and calls the active lens' on_event on core 1; the UI/tick loop runs on
 * core 0. Neither ever calls a transmit primitive, because there is not one
 * to call.
 */
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "pharos_bsp.h"
#include "pharos_lens.h"
#include "pharos_region.h"
#include "pharos_audio.h"
#include "pharos_theme.h"
#include "pharos_ui.h"

static const char *TAG = "pharos";

/* Forward decls from the system lens and the console glue. */
bool pharos_lens_system_fence_ok(void);
void pharos_console_start(void);

static void banner(const pharos_bsp_status_t *bsp)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  Pharos - defensive RF observatory (receive-only)");
    ESP_LOGI(TAG, "  lenses registered: %u", pharos_lens_count());
    ESP_LOGI(TAG, "  display=%d touch=%d pmu=%d psram=%uKB",
             bsp->display_ok, bsp->touch_ok, bsp->pmu_ok, bsp->psram_free / 1024);
    for (unsigned i = 0; i < pharos_lens_count(); i++) {
        const pharos_lens_t *l = pharos_lens_at(i);
        char caps[96];
        pharos_caps_describe(l->caps, caps, sizeof(caps));
        ESP_LOGI(TAG, "    %-14s %-10s [%s]", l->id, l->name, caps);
    }
}

void app_main(void)
{
    /* NVS: settings, region, saved site profiles. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* The default event loop MUST exist before esp_wifi_init(), even for a
     * receive-only, NULL-mode driver: the Wi-Fi stack posts internal events
     * (start, channel change, scan-done) to it unconditionally. Without it
     * every post fails with ESP_ERR_INVALID_STATE and the console fills with
     *     E wifi:failed to post WiFi event=43 ret=259
     * several times a second - which is exactly what a channel-hopping lens
     * produces. esp_netif_init() is its companion and is equally required.
     * Both are idempotent, so calling them once here at boot is the whole fix. */
    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    pharos_region_set((pharos_region_t)CONFIG_PHAROS_REGION_DEFAULT);

    pharos_bsp_status_t bsp;
    if (!pharos_bsp_init(&bsp)) {
        ESP_LOGE(TAG, "board bring-up failed; halting");
        /* Do not proceed on a half-initialised board. */
        return;
    }

    banner(&bsp);

    /* The self-audit lens verifies the transmit fence on mount. Mount it once
     * at boot so the check runs and the result is available to the UI before
     * the operator can launch anything that touches a radio. */
    pharos_lens_activate("sys.audit");
    const bool fence_ok = pharos_lens_system_fence_ok();
    pharos_lens_deactivate();

    if (!fence_ok) {
        ESP_LOGE(TAG, "TRANSMIT FENCE NOT CLEAN - refusing to start radio lenses");
    } else {
        ESP_LOGI(TAG, "transmit fence clean - receive-only confirmed");
    }

    /* The serial command console: a receive-only REPL on the USB port, so the
     * device is usable before the touch dial lands. Started before the UI takes
     * the main loop. */
    pharos_console_start();

    /* The UI owns the main loop from here: it builds the dial from the
     * registry, dispatches touch, ticks the active lens, and drains the
     * analytics bus. It is told whether the fence is clean so it can gate the
     * radio lenses behind a warning if it is not. */
    /* The alarm. A detector you have to watch is half a detector - see
     * pharos_audio.h. Failure here is not fatal: the device runs silent. */
    pharos_audio_init();

    /* The look the operator last chose, before the first frame is drawn -
     * booting in Beacon and snapping to Nightwatch a second later is the
     * device forgetting in public. */
    pharos_theme_load();

    pharos_ui_run(&bsp, fence_ok);
}
