/* Pharos - the UI runtime
 *
 * Two tasks, split by core exactly as the architecture promises:
 *
 *   analytics (core 1): drains the active lens' ingest ring and calls its
 *   on_event per frame. This is the hot side; it never touches the display.
 *
 *   ui (core 0): ticks the active lens ~20 Hz and repaints the round HUD at
 *   ~5 Hz. LVGL runs on the vendor BSP's own task, so painting here is only
 *   pushing fresh values in under its lock.
 *
 * The dial is built from the lens registry, sorted so the tools a defender
 * reaches for first sit at the top. main.c never names a lens; adding one .c
 * file to the build adds it to this dial automatically.
 */
#include "pharos_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "freertos/semphr.h"

#include "pharos_aegis.h"
#include "pharos_bus.h"
#include "pharos_dial.h"
#include "pharos_hud.h"
#include "pharos_radio.h"
#include "pharos_lens.h"

static const char *TAG = "ui";

static bool s_fence_ok;
static volatile bool s_analytics_run;

/* ---- the Aegis latch ------------------------------------------------
 *
 * Lives here because the UI loop is the one thing that runs continuously and
 * always knows which lens is active. Every second it asks the active lens for
 * its finding and folds it in, so the picture survives lens changes - and the
 * operator not looking. */
static pa_state_t s_aegis;
static SemaphoreHandle_t s_aegis_lock;
static uint32_t s_aegis_accum_ms;

static void aegis_pump(const pharos_lens_t *active, uint32_t dt_ms)
{
    s_aegis_accum_ms += dt_ms;
    if (s_aegis_accum_ms < 1000u) {
        return;
    }
    s_aegis_accum_ms = 0;
    if (!active || !active->stage_report || !s_aegis_lock) {
        return;
    }
    uint8_t stage = 0, score = 0, ceiling = 0;
    if (!active->stage_report(&stage, &score, &ceiling)) {
        return;
    }
    if (xSemaphoreTake(s_aegis_lock, 0) != pdTRUE) {
        return;
    }
    pa_observe(&s_aegis, (pa_stage_t)stage, score, ceiling,
               (uint64_t)esp_timer_get_time());
    xSemaphoreGive(s_aegis_lock);
}

bool pharos_ui_aegis_snapshot(pa_verdict_t *out)
{
    if (!out || !s_aegis_lock) {
        return false;
    }
    if (xSemaphoreTake(s_aegis_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    pa_evaluate(&s_aegis, (uint64_t)esp_timer_get_time(), out);
    xSemaphoreGive(s_aegis_lock);
    return true;
}

void pharos_ui_aegis_ack(void)
{
    if (!s_aegis_lock || xSemaphoreTake(s_aegis_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    pa_acknowledge(&s_aegis);
    xSemaphoreGive(s_aegis_lock);
    ESP_LOGI(TAG, "aegis: latch acknowledged; watch restarts clean");
}

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

/* Push the active lens' state onto the panel.
 *
 * The HUD is deliberately generic - lens name, one big value, a band word, a
 * detail line, a 0..100 gauge - so a new lens gets a screen for free. Where a
 * lens exposes a verdict snapshot we show it; otherwise we show that it is
 * running, which is still the truth and still better than a black panel. */
static void paint(const pharos_lens_t *active)
{
    if (!pharos_bsp_display_lock(30)) {
        return; /* display busy; try again next tick rather than block */
    }

    /* Idempotent: builds the widgets on the first frame under a good lock, so
     * the HUD exists even if the one-shot splash lock happened to time out.
     * Without this, a single missed lock at boot would leave the panel blank
     * for the whole session. */
    pharos_hud_create();

    if (!active) {
        pharos_hud_update("PHAROS", "--", s_fence_ok ? "idle" : "FENCE UNVERIFIED",
                          s_fence_ok ? "no lens running" : "radio locked",
                          0, s_fence_ok ? 0x7FA6B5 : 0xE8503F);
        pharos_bsp_display_unlock();
        return;
    }

    /* Uppercase short name for the header. */
    char name[16];
    unsigned n = 0;
    for (; active->name[n] && n < sizeof(name) - 1; n++) {
        const char c = active->name[n];
        name[n] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    name[n] = '\0';

    pharos_radio_stats_t st;
    pharos_radio_stats(&st);

    char big[16], detail[48];
    snprintf(big, sizeof(big), "%u", (unsigned)(st.frames_seen > 99999 ? 99999
                                                : st.frames_seen));
    snprintf(detail, sizeof(detail), "ch %u  %s",
             (unsigned)st.current_channel, st.camped ? "camped" : "hopping");

    /* Gauge shows airtime activity until a lens publishes a graded verdict:
     * frames seen, log-ish compressed into 0..100 so it moves visibly in a
     * quiet room and does not peg in a busy one. */
    int score = 0;
    uint32_t f = st.frames_seen;
    while (f && score < 100) { f >>= 1; score += 7; }

    pharos_hud_update(name, big, "listening", detail, score, 0x1FB6C9);
    pharos_bsp_display_unlock();
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

    pa_reset(&s_aegis);
    if (!s_aegis_lock) {
        s_aegis_lock = xSemaphoreCreateMutex();
    }

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

    /* Put the identity on the panel immediately, so the operator sees the
     * device is alive long before a lens has anything to say. */
    if (pharos_bsp_display_lock(200)) {
        pharos_hud_splash("v1.6.0", s_fence_ok);
        pharos_bsp_display_unlock();
    }

    uint64_t last_us = (uint64_t)esp_timer_get_time();
    uint32_t heartbeat = 0;
    uint32_t since_paint = 0;
    for (;;) {
        const uint64_t now = (uint64_t)esp_timer_get_time();
        const uint32_t dt_ms = (uint32_t)((now - last_us) / 1000);
        last_us = now;

        const pharos_lens_t *active = pharos_lens_active();
        if (active && active->on_tick && lens_launchable(active)) {
            active->on_tick(dt_ms);
        }

        /* Fold whatever the active lens is seeing into the correlator. */
        aegis_pump(active, dt_ms);

        /* Repaint at ~5 Hz. LVGL runs on the BSP's own task, so all we do here
         * is push fresh text/values in under its lock; a short timeout means a
         * busy display never stalls the analytics tick. */
        since_paint += dt_ms;
        if (since_paint >= 200) {
            since_paint = 0;
            paint(active);
        }

        /* M2 (remaining): touch hit-testing against the Lamp Room dial, so the
         * operator can switch lenses on the glass rather than over serial. The
         * geometry for it is done and tested in pharos_dial/pharos_round. */

        if ((++heartbeat % 100) == 0 && active) {
            ESP_LOGI(TAG, "active: %s  (dt=%ums)", active->id, dt_ms);
        }
        vTaskDelay(pdMS_TO_TICKS(50)); /* ~20 Hz */
    }
}
