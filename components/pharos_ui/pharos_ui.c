/* Pharos - the UI runtime (M1 scaffold; LVGL widgets are M2)
 *
 * Two tasks, split by core exactly as the architecture promises:
 *
 *   analytics (core 1): drains the active lens' ingest ring and calls its
 *   on_event per frame. This is the hot side; it never touches the display.
 *
 *   ui (core 0): ticks the active lens ~20 Hz, reads touch, and - once M2
 *   lands - draws the round HUD. Today it drives the dial model and logs the
 *   active verdict so the firmware is fully exercisable on hardware over
 *   serial before any pixel is lit.
 *
 * The dial is built from the lens registry, sorted so the tools a defender
 * reaches for first sit at the top. main.c never names a lens; adding one .c
 * file to the build adds it to this dial automatically.
 */
#include "pharos_ui.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pharos_bus.h"
#include "pharos_dial.h"
#include "pharos_lens.h"

static const char *TAG = "ui";

static bool s_fence_ok;
static volatile bool s_analytics_run;

unsigned pharos_ui_pump(void)
{
    const pharos_lens_t *lens = pharos_lens_active();
    if (!lens || !lens->ingest || !lens->on_event) {
        return 0;
    }
    struct pharos_bus *bus = lens->ingest();
    if (!bus) {
        return 0;
    }
    unsigned n = 0;
    pharos_event_t ev;
    /* Bounded per call so one very busy lens cannot starve the tick loop. */
    while (n < 256 && pharos_bus_pop((pharos_bus_t *)bus, &ev)) {
        lens->on_event(&ev);
        n++;
    }
    return n;
}

static void analytics_task(void *arg)
{
    (void)arg;
    s_analytics_run = true;
    while (s_analytics_run) {
        const unsigned n = pharos_ui_pump();
        /* Sleep a little longer when idle, stay hot under load. */
        vTaskDelay(pdMS_TO_TICKS(n ? 2 : 10));
    }
    vTaskDelete(NULL);
}

/* Is a lens safe to auto-launch given the fence state? A lens that holds any
 * radio capability is gated behind a clean fence. */
static bool lens_launchable(const pharos_lens_t *l)
{
    if (s_fence_ok) {
        return true;
    }
    const pharos_caps_t radio =
        PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_BLE_SCAN;
    return (l->caps & radio) == 0;
}

/* Order the dial: observe lenses first (the working tools), then train, then
 * system. Within a kind, registry order. This is presentation only; the
 * registry itself is untouched. */
static void build_dial(pd_dial_t *dial, const pharos_lens_t **order, unsigned *count)
{
    unsigned n = 0;
    const pharos_lens_kind_t kinds[] = {
        PHAROS_LENS_OBSERVE, PHAROS_LENS_ANALYSE, PHAROS_LENS_TRAIN, PHAROS_LENS_SYSTEM
    };
    for (unsigned k = 0; k < 4; k++) {
        for (unsigned i = 0; i < pharos_lens_count(); i++) {
            const pharos_lens_t *l = pharos_lens_at(i);
            if (l->kind == kinds[k]) {
                order[n++] = l;
            }
        }
    }
    *count = n;
    pd_dial_layout(n, 0.0f, PR_RING_R, PR_SAFE_R, dial);
    if (!dial->hittable) {
        /* Too many lenses for one ring of thumb-sized wedges. M2 pages the
         * dial; for now log it so it is never a silent usability failure. */
        ESP_LOGW(TAG, "%u lenses exceed one dial page (%u hittable); M2 will page",
                 n, dial->max_hittable);
    }
}

void pharos_ui_run(const pharos_bsp_status_t *bsp, bool fence_ok)
{
    (void)bsp;
    s_fence_ok = fence_ok;

    const pharos_lens_t *order[PHAROS_MAX_LENSES];
    pd_dial_t dial;
    unsigned count = 0;
    build_dial(&dial, order, &count);

    ESP_LOGI(TAG, "Lamp Room: %u lenses on the dial%s", count,
             s_fence_ok ? "" : " (radio lenses locked: fence not clean)");

    xTaskCreatePinnedToCore(analytics_task, "pharos_rx", 4096, NULL, 6, NULL, 1);

    /* Default landing lens: Spectrum if the fence is clean (you look before
     * you judge), otherwise the System panel so the operator sees why radio
     * is locked. */
    const char *landing = s_fence_ok ? "wifi.spectrum" : "sys.audit";
    if (!pharos_lens_activate(landing)) {
        ESP_LOGE(TAG, "could not activate landing lens %s", landing);
    }

    uint64_t last_us = (uint64_t)esp_timer_get_time();
    uint32_t heartbeat = 0;
    for (;;) {
        const uint64_t now = (uint64_t)esp_timer_get_time();
        const uint32_t dt_ms = (uint32_t)((now - last_us) / 1000);
        last_us = now;

        const pharos_lens_t *active = pharos_lens_active();
        if (active && active->on_tick && lens_launchable(active)) {
            active->on_tick(dt_ms);
        }

        /* M2: read touch, hit-test the dial, draw the HUD. The geometry is in
         * pharos_dial/pharos_round and is already tested; this loop is where
         * it gets wired to LVGL and the CST9217 touch events. */

        if ((++heartbeat % 100) == 0 && active) {
            ESP_LOGI(TAG, "active: %s  (dt=%ums)", active->id, dt_ms);
        }
        vTaskDelay(pdMS_TO_TICKS(50)); /* ~20 Hz */
    }
}
