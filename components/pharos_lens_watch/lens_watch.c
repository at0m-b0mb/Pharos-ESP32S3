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
#include <stdio.h>
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

/* ---- the lock-on -----------------------------------------------------
 *
 * The confidence ceiling is a function of how much of the channel this
 * receiver actually hears, so a hopping Watch tops out around 60 - SUSPICIOUS,
 * never an alarm - on any evidence that is a measurement rather than a
 * contradiction. The way out is to stop hopping. Until now the only way to do
 * that was a console command over USB, which is not something anybody does
 * while holding the device up in a corridor.
 *
 * So the lens does it itself: survey until disconnect traffic appears, then
 * camp on the channel carrying it for long enough to earn a real reading, then
 * release and carry on surveying. That is the same survey-then-hunt shape a
 * person would use, and it is the difference between a detector that reports
 * and one that only ever hints.
 *
 * The centre tap overrides it either way - see watch_select. */
#define WATCH_LOCK_MS   20000u /* long enough for the ceiling to be worth it */
#define WATCH_RELOCK_MS 10000u /* survey at least this long before re-arming */

static uint32_t s_lock_ms;    /* time left camped, 0 = surveying   */
static uint32_t s_survey_ms;  /* time spent surveying since a lock */
static bool s_manual;         /* the operator took the wheel       */
static uint32_t s_log_accum_ms;

static bool watch_mount(void)
{
    pw_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    s_lock_ms = 0;
    s_survey_ms = 0;
    s_manual = false;
    s_camp_requested = false;
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

/* Stop hopping and hold `channel`. Called only from the UI task. */
static void watch_camp_on(uint8_t channel)
{
    s_camp_requested = true;
    s_camp_channel = channel;
    pharos_radio_rx_stop();
    watch_start();
}

static void watch_survey_now(void)
{
    s_camp_requested = false;
    pharos_radio_rx_stop();
    watch_start();
}

static void watch_tick(uint32_t dt_ms)
{
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    const uint64_t now = (uint64_t)esp_timer_get_time();
    const uint8_t chan = pharos_radio_channel();
    /* Fifteen seconds, not ten.
     *
     * Real deauthentication tools fire in bursts with gaps between them -
     * aireplay-ng's default is 64 frames per round - and a ten-second window
     * can land entirely inside a gap. On hardware, against a live flood, the
     * verdict blinked FLOOD LIKELY -> QUIET -> FLOOD LIKELY as the window
     * rolled across those gaps. An alarm that switches itself off between
     * bursts is an alarm the operator misses.
     *
     * The engine keeps PW_WINDOW_SLOTS (16) seconds of per-second counts
     * anyway, so widening the window to fifteen costs nothing and spans the
     * gap. It is also the honest fix rather than latching a stale verdict on
     * the glass: the number still describes a window that actually happened. */
    pw_context_t ctx = {
        .dwell_permil = pharos_radio_dwell_permil(chan),
        .bus_yield_permil = pharos_bus_yield_permil(&s_bus),
        .window_ms = 15000,
    };
    pw_verdict_t v;
    pw_evaluate(&s_engine, now, &ctx, &v);

    /* Log on a band change, and also once a second while anything above
     * BACKGROUND is in view - a band that sits still is exactly when the
     * operator most wants the supporting numbers, and "it said SUSPICIOUS
     * once and then went quiet" is not a usable field report. */
    s_log_accum_ms += dt_ms;
    const bool interesting = (v.band >= PW_BAND_ELEVATED);
    if (v.band != s_verdict.band || (interesting && s_log_accum_ms >= 2000u)) {
        if (v.band != s_verdict.band) {
            s_log_accum_ms = 0;
        } else {
            s_log_accum_ms = 0;
        }
        const char *why = pw_forgery_name(v.forgery);
        ESP_LOGI(TAG,
                 "%s %u/%u ch%u \"%s\" fam=0x%02x est=%u.%02u/s peak=%u obs=%u "
                 "bcast=%u%% victims=%u burst=%u src=%02x:%02x:%02x:%02x:%02x:%02x "
                 "rssi d=%d/s=%d seqv=%u rejoin=%u/%u dwell=%u%% %s",
                 pw_band_name(v.band), v.score, v.ceiling,
                 (unsigned)(v.channel ? v.channel : chan),
                 v.ssid[0] ? v.ssid : "?",
                 v.families,
                 v.est_per_s_x100 / 100, v.est_per_s_x100 % 100,
                 (unsigned)v.peak_second, (unsigned)v.observed,
                 (unsigned)(v.broadcast_permil / 10), (unsigned)v.distinct_victims,
                 (unsigned)v.max_burst,
                 v.src[0], v.src[1], v.src[2], v.src[3], v.src[4], v.src[5],
                 (int)v.rssi_delta, (int)v.rssi_spread,
                 (unsigned)v.seq_violations,
                 (unsigned)v.rejoins_after, (unsigned)v.rejoins,
                 ctx.dwell_permil / 10, why ? why : "");
    }
    s_verdict = v;
    const uint8_t pressure = v.channel;
    xSemaphoreGive(s_lock);

    /* The lock-on state machine. Deliberately outside the verdict mutex: it
     * restarts the radio, which is slow, and nothing else needs the lock held
     * while it happens. */
    if (s_manual) {
        return;
    }
    if (s_lock_ms) {
        s_lock_ms = (s_lock_ms > dt_ms) ? (s_lock_ms - dt_ms) : 0u;
        if (s_lock_ms == 0u) {
            ESP_LOGI(TAG, "lock expired; back to survey");
            watch_survey_now();
            s_survey_ms = 0;
        } else if (v.band >= PW_BAND_ELEVATED) {
            /* Still worth watching: hold the channel rather than letting the
             * timer drop us back into a survey mid-event. */
            s_lock_ms = WATCH_LOCK_MS;
        }
        return;
    }

    s_survey_ms += dt_ms;
    if (s_survey_ms < WATCH_RELOCK_MS) {
        return; /* do not thrash the radio between two busy channels */
    }
    /* Something is happening and we are only hearing a fourteenth of it. Go
     * and stand on it. Requiring the RATE family rather than merely a nonzero
     * count keeps the device from camping on one roaming client's timeout. */
    if (pressure && (v.families & PW_FAM_RATE) && !pharos_radio_is_camped()) {
        ESP_LOGI(TAG, "locking on to channel %u to raise the ceiling", pressure);
        watch_camp_on(pressure);
        s_lock_ms = WATCH_LOCK_MS;
    }
}

/* The centre tap while Watch is running: take the wheel.
 *
 * First tap camps here and stays camped; second tap goes back to surveying and
 * hands the lock-on back to the lens. An operator who has decided where to
 * stand should not have the device wander off after twenty seconds. */
static void watch_select(void)
{
    if (!s_manual || !pharos_radio_is_camped()) {
        uint8_t ch = s_verdict.channel;
        if (!ch) {
            ch = pharos_radio_channel();
        }
        if (!ch) {
            return;
        }
        s_manual = true;
        s_lock_ms = 0;
        watch_camp_on(ch);
        ESP_LOGI(TAG, "operator camped on channel %u", ch);
    } else {
        s_manual = false;
        s_lock_ms = 0;
        s_survey_ms = 0;
        watch_survey_now();
        ESP_LOGI(TAG, "operator released the camp; surveying");
    }
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

/* The console's "camp here": stop hopping and hold one channel, which is what
 * actually raises the confidence ceiling.
 *
 * Both of these set s_manual, because an operator who has said where to stand
 * should not have the lens' own lock-on wander off twenty seconds later. The
 * centre tap on the glass clears it again - see watch_select. */
void pharos_lens_watch_camp(uint8_t channel)
{
    s_manual = true;
    s_lock_ms = 0;
    s_camp_requested = true;
    s_camp_channel = channel;
    if (pharos_lens_active() && strcmp(pharos_lens_active()->id, "wifi.watch") == 0) {
        pharos_radio_rx_stop();
        watch_start();
        ESP_LOGI(TAG, "camping on channel %u (operator)", channel);
    }
}

void pharos_lens_watch_survey(void)
{
    s_manual = false;
    s_lock_ms = 0;
    s_survey_ms = 0;
    s_camp_requested = false;
    if (pharos_lens_active() && strcmp(pharos_lens_active()->id, "wifi.watch") == 0) {
        pharos_radio_rx_stop();
        watch_start();
        ESP_LOGI(TAG, "surveying (operator); lock-on re-armed");
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
    prt_u32(&w, "forgery", v.forgery);
    prt_u32(&w, "observed", v.observed);
    prt_u32(&w, "peak_second", v.peak_second);
    prt_u32(&w, "est_per_s_x100", v.est_per_s_x100);
    prt_u32(&w, "broadcast_permil", v.broadcast_permil);
    prt_u32(&w, "distinct_victims", v.distinct_victims);
    prt_u32(&w, "distinct_sources", v.distinct_sources);
    prt_u32(&w, "max_burst", v.max_burst);
    prt_u32(&w, "channel", v.channel);
    prt_mac(&w, "dominant_source", v.src);
    prt_i32(&w, "rssi_delta", v.rssi_delta);
    prt_i32(&w, "rssi_spread", v.rssi_spread);
    prt_u32(&w, "seq_violations", v.seq_violations);
    prt_u32(&w, "seq_distinct", v.seq_distinct);
    prt_u32(&w, "rejoins", v.rejoins);
    prt_u32(&w, "rejoins_after", v.rejoins_after);
    prt_u32(&w, "dominant_reason", v.dominant_reason);
    {
        const char *why = pw_forgery_name(v.forgery);
        if (why) {
            prt_str(&w, "forgery_finding", why);
        }
    }
    prt_str(&w, "advice", pw_band_advice(v.band));
    prt_obj_end(&w);
    prt_obj_end(&w);
    return prt_finish(&w);
}

static struct pharos_bus *watch_ingest(void) { return &s_bus; }


/* Aegis: this lens speaks for the DISRUPT stage. See pharos_lens_t::stage_report
 * - the UI loop asks about once a second, so the finding survives the operator
 * moving on to another lens. */
static bool k_watch_stage(uint8_t *stage, uint8_t *score, uint8_t *ceiling)
{
    *stage = 2; /* PA_STAGE_DISRUPT */
    *score = s_verdict.score;
    *ceiling = s_verdict.ceiling;
    return true;
}

static bool k_watch_display(struct pharos_lens_display *o)
{
    pw_verdict_t v;
    if (!pharos_lens_watch_snapshot(&v)) {
        return false;
    }

    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", pw_band_name(v.band));
    snprintf(o->advice, sizeof(o->advice), "%s", pw_band_advice(v.band));
    o->score = v.score;
    o->raw_score = v.raw_score;
    o->ceiling = v.ceiling;
    o->has_score = true;

    /* The context line: what is under pressure, and how hard we are looking.
     * "LOCKED" is not decoration - it is the operator's answer to a
     * SUSPICIOUS reading, and they need to see whether it is in effect. */
    {
        const char *posture = s_manual         ? "HELD"
                              : s_lock_ms      ? "LOCKED"
                              : pharos_radio_is_camped() ? "CAMPED"
                                                         : "HOPPING";
        if (v.ssid[0]) {
            snprintf(o->detail, sizeof(o->detail), "%.14s  %s", v.ssid, posture);
        } else {
            snprintf(o->detail, sizeof(o->detail), "CH %u  %s",
                     (unsigned)(v.channel ? v.channel : pharos_radio_channel()),
                     posture);
        }
    }

    /* The evidence, named. Four pips beat three anonymous dots because the
     * operator can act on "FORGE" and cannot act on "the third dot". */
    o->families = v.families;
    o->fam_label[0] = "RATE";
    o->fam_label[1] = "SHAPE";
    o->fam_label[2] = "FORGE";
    o->fam_label[3] = "AFTER";

    /* The specific finding, if there is one. "sequence counter went backwards"
     * is worth a line of glass in a way that "the shape looks wrong" is not. */
    {
        const char *why = pw_forgery_name(v.forgery);
        if (why) {
            snprintf(o->why, sizeof(o->why), "%s", why);
        }
    }

    /* The activity ribbon, normalised against its own peak. The engine keeps
     * these seconds anyway to do its arithmetic; this only shows them. A
     * steady trickle and one violent burst have the same ten-second mean and
     * are not the same event. */
    {
        uint16_t hist[PW_WINDOW_SLOTS];
        pw_history(&s_engine, (uint64_t)esp_timer_get_time(), hist);
        uint16_t peak = 0;
        for (unsigned i = 0; i < PW_WINDOW_SLOTS; i++) {
            if (hist[i] > peak) peak = hist[i];
        }
        const unsigned n = (PW_WINDOW_SLOTS < PHAROS_DISP_HISTORY)
                               ? PW_WINDOW_SLOTS : PHAROS_DISP_HISTORY;
        for (unsigned i = 0; i < n; i++) {
            o->history[i] = peak ? (uint8_t)(((uint32_t)hist[i] * 255u) / peak) : 0u;
        }
        o->has_history = true;
    }
    return true;
}

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
    .stage_report = k_watch_stage,
    .display = k_watch_display,
    .on_select = watch_select,
};

PHAROS_LENS_REGISTER(&k_watch);
