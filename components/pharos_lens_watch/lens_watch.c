/* Pharos lens: Watch - the deauthentication sentinel
 *
 * The blue-team headliner. It listens for somebody knocking clients off a
 * network and grades what it hears, honestly, with a confidence ceiling tied
 * to how much of the channel it is actually hearing.
 *
 * All the judgement is in pharos_watch.c, which is pure and host-tested. This
 * file is plumbing: bring the radio up, feed frames to the engine on the
 * analytics core, and hand the UI a snapshot. If you find yourself writing an
 * `if` here that decides whether something is an attack, it belongs in the
 * engine with a test, not in the lens.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_lens.h"
#include "pharos_radio.h"
#include "pharos_report.h"
#include "pharos_watch.h"

static const char *TAG = "lens.watch";

#define WATCH_RING 1024 /* a flood is exactly when the ring matters */

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[WATCH_RING];
static pharos_bus_t s_bus;
EXT_RAM_BSS_ATTR static pw_engine_t s_engine;
static pw_verdict_t s_verdict;
static SemaphoreHandle_t s_lock;
static bool s_camp_requested;
static uint8_t s_camp_channel;

static bool watch_mount(void)
{
    pw_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock && pharos_bus_init(&s_bus, s_slots, WATCH_RING);
}

static bool watch_start(void)
{
    pharos_scan_plan_t plan = s_camp_requested
                                  ? pharos_scan_plan_camp(s_camp_channel)
                                  : pharos_scan_plan_survey();
    plan.want_mgmt = true;
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void watch_stop(void)
{
    pharos_radio_rx_stop();
}

/* Analytics core. */
static void watch_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }
    pw_observe(&s_engine, &ev->u.dot11, ev->t_us);
}

static void watch_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    const uint8_t chan = pharos_radio_channel();
    pw_context_t ctx = {
        .dwell_permil = pharos_radio_dwell_permil(chan),
        .bus_yield_permil = pharos_bus_yield_permil(&s_bus),
        .window_ms = 10000,
    };
    pw_verdict_t v;
    pw_evaluate(&s_engine, (uint64_t)esp_timer_get_time(), &ctx, &v);

    if (v.band != s_verdict.band) {
        ESP_LOGI(TAG, "%s score=%u/%u fam=0x%02x est=%u.%02u/s dwell=%u%%",
                 pw_band_name(v.band), v.score, v.ceiling, v.families,
                 v.est_per_s_x100 / 100, v.est_per_s_x100 % 100, ctx.dwell_permil / 10);
    }
    s_verdict = v;
    xSemaphoreGive(s_lock);
}

bool pharos_lens_watch_snapshot(pw_verdict_t *out)
{
    if (!out || !s_lock) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    *out = s_verdict;
    xSemaphoreGive(s_lock);
    return true;
}

/* The UI's "camp here" button: stop hopping and hold the channel the current
 * verdict is about, which is what actually raises the confidence ceiling. */
void pharos_lens_watch_camp(uint8_t channel)
{
    s_camp_requested = true;
    s_camp_channel = channel;
    if (pharos_lens_active() && strcmp(pharos_lens_active()->id, "wifi.watch") == 0) {
        pharos_radio_rx_stop();
        watch_start();
        ESP_LOGI(TAG, "camping on channel %u", channel);
    }
}

void pharos_lens_watch_survey(void)
{
    s_camp_requested = false;
    if (pharos_lens_active() && strcmp(pharos_lens_active()->id, "wifi.watch") == 0) {
        pharos_radio_rx_stop();
        watch_start();
    }
}

bool pharos_lens_watch_report(char *buf, size_t cap, prt_redact_t redact, uint32_t salt)
{
    pw_verdict_t v;
    if (!pharos_lens_watch_snapshot(&v)) {
        return false;
    }
    prt_t w;
    prt_init(&w, buf, cap, redact, salt);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_str(&w, "lens", "wifi.watch");
    prt_u32(&w, "uptime_ms", (uint32_t)(esp_timer_get_time() / 1000));
    prt_obj_begin(&w, "verdict");
    prt_str(&w, "band", pw_band_name(v.band));
    prt_u32(&w, "score", v.score);
    prt_u32(&w, "raw_score", v.raw_score);
    prt_u32(&w, "ceiling", v.ceiling);
    prt_u32(&w, "families", v.families);
    prt_u32(&w, "notes", v.notes);
    prt_u32(&w, "observed", v.observed);
    prt_u32(&w, "est_per_s_x100", v.est_per_s_x100);
    prt_u32(&w, "broadcast_permil", v.broadcast_permil);
    prt_u32(&w, "distinct_victims", v.distinct_victims);
    prt_u32(&w, "distinct_sources", v.distinct_sources);
    prt_mac(&w, "dominant_source", v.src);
    prt_i32(&w, "rssi_delta", v.rssi_delta);
    prt_u32(&w, "dominant_reason", v.dominant_reason);
    prt_str(&w, "advice", pw_band_advice(v.band));
    prt_obj_end(&w);
    prt_obj_end(&w);
    return prt_finish(&w);
}

static struct pharos_bus *watch_ingest(void) { return &s_bus; }

static const pharos_lens_t k_watch = {
    .id = "wifi.watch",
    .name = "Watch",
    .summary = "Listens for deauthentication floods and grades them honestly",
    .glyph = "shield",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_STORAGE_W,
    .budget_ma = 135,
    .on_mount = watch_mount,
    .on_start = watch_start,
    .on_stop = watch_stop,
    .on_tick = watch_tick,
    .on_event = watch_event,
    .ingest = watch_ingest,
};

PHAROS_LENS_REGISTER(&k_watch);
