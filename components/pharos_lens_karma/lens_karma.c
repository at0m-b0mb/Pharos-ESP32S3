/* Pharos lens: Karma - the impersonation watch
 *
 * The natural companion to Probe. Probe shows you that phones shout the names
 * of networks they remember; Karma watches for the radio that answers "yes,
 * that's me" to every one of those shouts.
 *
 * It needs probe requests, probe responses and beacons, so it asks the radio
 * for management frames and camps a little longer than the survey default -
 * the whole detection rests on having genuinely heard what a radio does and
 * does not announce, and that is precisely the claim a fast hop cannot make.
 *
 * Judgement is in pharos_karma.c, pure and host-tested. This file translates
 * frames into engine calls and hands the UI a snapshot.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_dot11.h"
#include "pharos_karma.h"
#include "pharos_pulse.h"
#include "pharos_lens.h"
#include "pharos_radio.h"
#include "pharos_report.h"

static const char *TAG = "lens.karma";

#define KARMA_RING 512

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[KARMA_RING];
static pharos_bus_t s_bus;
EXT_RAM_BSS_ATTR static pk_engine_t s_engine;
static pk_verdict_t s_verdict;
static SemaphoreHandle_t s_lock;

static bool karma_mount(void)
{
    pk_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock && pharos_bus_init(&s_bus, s_slots, KARMA_RING);
}

static bool karma_start(void)
{
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    /* Longer dwell than the survey default: "this radio never announced that
     * name" is only worth saying if we listened long enough to have heard it. */
    plan.dwell_ms = 600;
    plan.want_mgmt = true;
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void karma_stop(void) { pharos_radio_rx_stop(); }

/* The shared activity ribbon: one call per event in, one call per repaint
 * out. Before this, every lens but Watch drew an empty timeline. */
static pharos_pulse_t s_pulse;

static void karma_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }

    pharos_pulse_note(&s_pulse, ev->t_us);
    const pharos_ev_dot11_t *f = &ev->u.dot11;
    if (f->type != PHAROS_FT_MGMT) {
        return;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    /* Karma is ENTIRELY a question about names: does this radio answer to a
     * name it never advertises? Feeding it NULL, as this did until v1.13.0,
     * left it structurally unable to detect anything - it reported NORMAL
     * forever, honestly but uselessly. */
    const char *ssid = f->ssid_len ? f->ssid : NULL;
    switch (f->subtype) {
    case PHAROS_ST_PROBE_REQ:
        pk_observe_probe(&s_engine, ssid, f->ssid_len, ev->t_us);
        break;
    case PHAROS_ST_PROBE_RESP:
        pk_observe_response(&s_engine, f->a2, ssid, f->ssid_len, f->rssi, f->channel, ev->t_us);
        break;
    case PHAROS_ST_BEACON:
        pk_observe_beacon(&s_engine, f->a2, ssid, f->ssid_len, f->rssi, f->channel, ev->t_us);
        break;
    default:
        break;
    }
    xSemaphoreGive(s_lock);
}

static void karma_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    pk_context_t ctx = {
        .dwell_permil = pharos_radio_dwell_permil(pharos_radio_channel()),
        .bus_yield_permil = pharos_bus_yield_permil(&s_bus),
    };
    pk_verdict_t v;
    pk_evaluate(&s_engine, &ctx, &v);
    if (v.band != s_verdict.band) {
        ESP_LOGI(TAG, "%s score=%u/%u answered=%u unannounced=%u",
                 pk_band_name(v.band), v.score, v.ceiling, v.answered_ssids,
                 v.unannounced);
    }
    s_verdict = v;
    xSemaphoreGive(s_lock);
}

bool pharos_lens_karma_snapshot(pk_verdict_t *out)
{
    if (!out || !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    *out = s_verdict;
    xSemaphoreGive(s_lock);
    return true;
}

bool pharos_lens_karma_report(char *buf, size_t cap, prt_redact_t redact, uint32_t salt)
{
    pk_verdict_t v;
    if (!pharos_lens_karma_snapshot(&v)) {
        return false;
    }
    prt_t w;
    prt_init(&w, buf, cap, redact, salt);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_str(&w, "lens", "wifi.karma");
    prt_u32(&w, "uptime_ms", (uint32_t)(esp_timer_get_time() / 1000));
    prt_obj_begin(&w, "verdict");
    prt_str(&w, "band", pk_band_name(v.band));
    prt_u32(&w, "score", v.score);
    prt_u32(&w, "raw_score", v.raw_score);
    prt_u32(&w, "ceiling", v.ceiling);
    prt_u32(&w, "families", v.families);
    prt_u32(&w, "notes", v.notes);
    prt_mac(&w, "suspect", v.suspect);
    prt_u32(&w, "answered_ssids", v.answered_ssids);
    prt_u32(&w, "unannounced", v.unannounced);
    prt_u32(&w, "echoed", v.echoed);
    prt_str(&w, "headline", v.headline);
    prt_str(&w, "advice", pk_band_advice(v.band));
    prt_obj_end(&w);
    prt_obj_end(&w);
    return prt_finish(&w);
}

static struct pharos_bus *karma_ingest(void) { return &s_bus; }


/* Aegis: this lens speaks for the IMPERSONATE stage. See pharos_lens_t::stage_report
 * - the UI loop asks about once a second, so the finding survives the operator
 * moving on to another lens. */
static bool k_karma_stage(uint8_t *stage, uint8_t *score, uint8_t *ceiling)
{
    *stage = 1; /* PA_STAGE_IMPERSONATE */
    *score = s_verdict.score;
    *ceiling = s_verdict.ceiling;
    return true;
}

static bool k_karma_display(struct pharos_lens_display *o)
{
    pk_verdict_t v = s_verdict;
    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", pk_band_name(v.band));
    snprintf(o->detail, sizeof(o->detail), "%u answered  ceil %u",
             (unsigned)v.answered_ssids, v.ceiling);
    snprintf(o->advice, sizeof(o->advice), "%s", pk_band_advice(v.band));
    o->score = v.score; o->ceiling = v.ceiling; o->has_score = true;
    /* WHY it thinks so, on the glass. The engine has computed these families
     * all along; nothing was carrying them to the face, so this lens lit no
     * chips at all and its score had to be taken on trust. */
    o->families = v.families;
    o->fam_label[0] = "BREADTH";
    o->fam_label[1] = "ABSENT";
    o->fam_label[2] = "ECHO";
    o->fam_label[3] = NULL;
    o->has_history = pharos_pulse_fill(&s_pulse, (uint64_t)esp_timer_get_time(), o->history);
    return true;
}

/* A KARMA responder answers to names nobody advertised. These rows are that
 * sentence as numbers: how many names it answered for, and how many of those
 * it never announced itself - the gap IS the attack. */
static bool k_karma_row(unsigned index, struct pharos_lens_row *out)
{
    pk_verdict_t v;
    if (!pharos_lens_karma_snapshot(&v)) return false;
    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "names answered");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.answered_ssids);
        out->tone = PHAROS_TONE_NEUTRAL; return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "never announced");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.unannounced);
        /* The gap is the finding: a legitimate multi-SSID access point
         * beacons every name it answers for. */
        out->tone = v.unannounced ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD; return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "answered promptly");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.echoed);
        out->tone = v.echoed ? PHAROS_TONE_WARN : PHAROS_TONE_DIM; return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "responder");
        snprintf(out->right, sizeof(out->right), "%02x:%02x:%02x",
                 v.suspect[3], v.suspect[4], v.suspect[5]);
        out->tone = v.unannounced ? PHAROS_TONE_BAD : PHAROS_TONE_DIM; return true;
    default: return false;
    }
}

static const pharos_lens_t k_karma = {
    .id = "wifi.karma",
    .purpose = "APs answering any name",
    .name = "Karma",
    .summary = "Finds the radio that agrees to be whatever a phone asks for",
    .glyph = "mask",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_STORAGE_W,
    .budget_ma = 132,
    .on_mount = karma_mount,
    .on_start = karma_start,
    .on_stop = karma_stop,
    .on_tick = karma_tick,
    .on_event = karma_event,
    .ingest = karma_ingest,
    .stage_report = k_karma_stage,
    .display = k_karma_display,
    .row = k_karma_row,
    .row_head_left = "RESPONDER",
    .row_head_right = "COUNT",
};

PHAROS_LENS_REGISTER(&k_karma);
