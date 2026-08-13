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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "pharos_bsp.h"
#include "pharos_lens.h"
#include "pharos_region.h"
#include "pharos_ui.h"

static const char *TAG = "pharos";

/* Forward decls from the system lens. */
bool pharos_lens_system_fence_ok(void);

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

    /* The UI owns the main loop from here: it builds the dial from the
     * registry, dispatches touch, ticks the active lens, and drains the
     * analytics bus. It is told whether the fence is clean so it can gate the
     * radio lenses behind a warning if it is not. */
    pharos_ui_run(&bsp, fence_ok);
}
