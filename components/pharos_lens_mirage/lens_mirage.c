/* Pharos lens: Mirage - the beacon-flood watch
 *
 * Detects the SSID-spam attack the ESP32 world is best known for - a radio
 * inventing hundreds of fake network names to fill every phone's Wi-Fi list.
 * Pharos is the inverse of the tools that ship that attack: it finds it,
 * passively, and never transmits a frame doing so.
 *
 * Judgement is in pharos_flood.c, pure and host-tested. This lens surveys
 * beacons and feeds the engine. Fast beacons on the current channel are the
 * signal, so it uses a brisk survey sweep and keeps the mgmt filter on.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_dot11.h"
#include "pharos_flood.h"
#include "pharos_lens.h"
#include "pharos_radio.h"
#include "pharos_report.h"

static const char *TAG = "lens.mirage";

#define MIRAGE_RING 1024 /* a flood is exactly when the ring matters */

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[MIRAGE_RING];
static pharos_bus_t s_bus;
EXT_RAM_BSS_ATTR static pf_engine_t s_engine;
static pf_verdict_t s_verdict;
static SemaphoreHandle_t s_lock;

static bool mirage_mount(void)
{
    pf_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock && pharos_bus_init(&s_bus, s_slots, MIRAGE_RING);
}

static bool mirage_start(void)
{
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    plan.dwell_ms = 150;
    plan.want_mgmt = true;
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void mirage_stop(void) { pharos_radio_rx_stop(); }

static void mirage_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }
    const pharos_ev_dot11_t *f = &ev->u.dot11;
    if (f->type != PHAROS_FT_MGMT || f->subtype != PHAROS_ST_BEACON) {
        return;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    /* The SSID comes from the beacon body via the capture ring (M5). Until
     * that path lands the engine sees the BSSID and timing but not the name,
     * so pf_observe is fed the transmitter address as a stand-in key and the
     * name-based families stay conservative - the same honest degradation as
     * Census and Karma, never a fabricated verdict. */
    pf_observe(&s_engine, f->a2, (const char *)f->a2, 6, ev->t_us);
    xSemaphoreGive(s_lock);
}

static void mirage_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    pf_context_t ctx = {
        .dwell_permil = pharos_radio_dwell_permil(pharos_radio_channel()),
        .bus_yield_permil = pharos_bus_yield_permil(&s_bus),
        .window_ms = 12000,
    };
    pf_verdict_t v;
    pf_evaluate(&s_engine, &ctx, &v);
    if (v.band != s_verdict.band) {
        ESP_LOGI(TAG, "%s score=%u/%u ssids=%u new/min=%u.%u synth=%u%%",
                 pf_band_name(v.band), v.score, v.ceiling, v.distinct_ssids,
                 v.new_per_min_x10 / 10, v.new_per_min_x10 % 10,
                 v.synthetic_permil / 10);
    }
    s_verdict = v;
    xSemaphoreGive(s_lock);
}

bool pharos_lens_mirage_snapshot(pf_verdict_t *out)
{
    if (!out || !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    *out = s_verdict;
    xSemaphoreGive(s_lock);
    return true;
}

bool pharos_lens_mirage_report(char *buf, size_t cap, prt_redact_t redact, uint32_t salt)
{
    pf_verdict_t v;
    if (!pharos_lens_mirage_snapshot(&v)) {
        return false;
    }
    prt_t w;
    prt_init(&w, buf, cap, redact, salt);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_str(&w, "lens", "wifi.mirage");
    prt_u32(&w, "uptime_ms", (uint32_t)(esp_timer_get_time() / 1000));
    prt_obj_begin(&w, "verdict");
    prt_str(&w, "band", pf_band_name(v.band));
    prt_u32(&w, "score", v.score);
    prt_u32(&w, "ceiling", v.ceiling);
    prt_u32(&w, "families", v.families);
    prt_u32(&w, "notes", v.notes);
    prt_u32(&w, "distinct_ssids", v.distinct_ssids);
    prt_u32(&w, "new_per_min_x10", v.new_per_min_x10);
    prt_u32(&w, "ephemeral_permil", v.ephemeral_permil);
    prt_u32(&w, "synthetic_permil", v.synthetic_permil);
    prt_str(&w, "advice", pf_band_advice(v.band));
    prt_obj_end(&w);
    prt_obj_end(&w);
    return prt_finish(&w);
}

static struct pharos_bus *mirage_ingest(void) { return &s_bus; }


/* Aegis: this lens speaks for the DISRUPT stage. See pharos_lens_t::stage_report
 * - the UI loop asks about once a second, so the finding survives the operator
 * moving on to another lens. */
static bool k_mirage_stage(uint8_t *stage, uint8_t *score, uint8_t *ceiling)
{
    *stage = 2; /* PA_STAGE_DISRUPT */
    *score = s_verdict.score;
    *ceiling = s_verdict.ceiling;
    return true;
}

static const pharos_lens_t k_mirage = {
    .id = "wifi.mirage",
    .name = "Mirage",
    .summary = "Detects the beacon flood that fills a phone with fake networks",
    .glyph = "sparkles",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_STORAGE_W,
    .budget_ma = 132,
    .on_mount = mirage_mount,
    .on_start = mirage_start,
    .on_stop = mirage_stop,
    .on_tick = mirage_tick,
    .on_event = mirage_event,
    .ingest = mirage_ingest,
    .stage_report = k_mirage_stage,
};

PHAROS_LENS_REGISTER(&k_mirage);
