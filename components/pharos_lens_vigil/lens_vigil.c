/* Pharos lens: Vigil - is a tracker travelling with you?
 *
 * The first lens to use the Bluetooth radio, which has sat idle since v1.0.
 * It listens for item trackers (AirTag, Tile, SmartTag) and answers the only
 * question that matters about them: not "is one nearby" - one always is - but
 * "is one still with me now that I have MOVED?"
 *
 * Movement without a GPS: the lens periodically hashes the set of access
 * points the Wi-Fi radio can hear into a signature, and hands it to the engine
 * as a "locale". When that landscape turns over, you are somewhere else. A
 * tracker present across several locales is travelling with you.
 *
 * That means this lens uses BOTH radios at once - BLE to hear the tags, Wi-Fi
 * to know whether you have moved - which is why it declares both capabilities.
 * Neither transmits: the BLE scan is passive by construction (see
 * pharos_radio.c) and the Wi-Fi side is promiscuous receive.
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
#include "pharos_pulse.h"
#include "pharos_lens.h"
#include "pharos_sense.h"
#include "pharos_ui.h"
#include "pharos_radio.h"
#include "pharos_report.h"
#include "pharos_vigil.h"

static const char *TAG = "lens.vigil";

#define VIGIL_RING 512
#define VIGIL_LOCALE_MS 20000 /* re-read the landscape this often */

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[VIGIL_RING];
static pharos_bus_t s_bus;

EXT_RAM_BSS_ATTR static pv_state_t s_engine;
static pv_verdict_t s_verdict;

/* Steps counted when this lens started looking. Everything Vigil concludes
 * about "travelling with you" is measured from here. */
static uint32_t s_step_mark;
static bool s_motion_gated; /* the last verdict was held back by stillness */
static SemaphoreHandle_t s_lock;
static pv_band_t s_last_band;

/* The access-point landscape, accumulated between locale samples. Each BSSID
 * sets one bit, so the signature is a cheap set summary the engine can compare
 * without needing the addresses themselves - which also means no MAC ever
 * leaves this lens for the locale path. */
static uint32_t s_sig_accum;
static uint32_t s_locale_ms;

static bool vigil_mount(void)
{
    pv_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    s_last_band = PV_BAND_CLEAR;
    s_step_mark = pharos_sense_steps();
    s_motion_gated = false;
    s_sig_accum = 0;
    s_locale_ms = 0;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL && pharos_bus_init(&s_bus, s_slots, VIGIL_RING);
}

static bool vigil_start(void)
{
    /* Wi-Fi first, for the locale signal, then the BLE observer. */
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    plan.dwell_ms = 250;
    plan.want_mgmt = true;
    if (!pharos_radio_rx_start(&plan, &s_bus)) {
        return false;
    }
    if (!pharos_radio_ble_scan_start(&s_bus)) {
        ESP_LOGE(TAG, "BLE observer would not start - Vigil cannot see tags");
        pharos_radio_rx_stop();
        return false;
    }
    return true;
}

static void vigil_stop(void)
{
    pharos_radio_ble_scan_stop();
    pharos_radio_rx_stop();
}

/* The shared activity ribbon: one call per event in, one call per repaint
 * out. Before this, every lens but Watch drew an empty timeline. */
static pharos_pulse_t s_pulse;

static void vigil_event(const pharos_event_t *ev)
{
    if (!ev) {
        return;
    }

    pharos_pulse_note(&s_pulse, ev->t_us);
    if (ev->type == PHAROS_EV_BLE_ADV) {
        pv_observe_adv(&s_engine, ev->u.ble.addr, ev->u.ble.addr_type,
                       ev->u.ble.rssi, ev->u.ble.data, ev->u.ble.data_len,
                       ev->t_us);
        return;
    }
    if (ev->type == PHAROS_EV_DOT11) {
        const pharos_ev_dot11_t *f = &ev->u.dot11;
        if (f->type == PHAROS_FT_MGMT && f->subtype == PHAROS_ST_BEACON) {
            /* One bit per access point, folded from the BSSID. Cheap, order
             * independent, and it keeps no addresses. */
            uint32_t h = 2166136261u;
            for (int i = 0; i < 6; i++) {
                h ^= f->a2[i];
                h *= 16777619u;
            }
            s_sig_accum |= (1u << (h & 31u));
        }
    }
}

static void vigil_tick(uint32_t dt_ms)
{
    /* Tell the engine whether anybody actually went anywhere.
     *
     * Locale turnover is evidence of movement, not proof: a rebooting router
     * or turning round in a big building looks the same. The accelerometer is
     * wrong in different circumstances, so when it says confidently that the
     * device has not moved, Vigil withholds FOLLOWING - see pv_set_moved. */
    {
        uint8_t mstate = 0;
        if (pharos_ui_motion(&mstate, NULL, NULL)) {
            pv_set_moved(&s_engine, pharos_ui_has_travelled(0));
        }
    }

    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    const uint64_t now = (uint64_t)esp_timer_get_time();

    s_locale_ms += dt_ms;
    if (s_locale_ms >= VIGIL_LOCALE_MS) {
        s_locale_ms = 0;
        if (s_sig_accum) {
            pv_observe_locale(&s_engine, s_sig_accum, now);
        }
        s_sig_accum = 0; /* start a fresh picture of where we are */
    }

    pv_evaluate(&s_engine, now, &s_verdict);

    /* THE MOTION GATE.
     *
     * "It is travelling with you" is only a finding if YOU travelled. Vigil
     * infers that from Wi-Fi locale turnover, which is a good signal and a
     * fallible one in both directions: access points switch off at night and
     * the locale turns over while you sat still, and a car park has no Wi-Fi
     * to turn over at all.
     *
     * The accelerometer measures the same thing directly and fails in
     * completely different circumstances, which is the whole reason to have
     * it. So a FOLLOWING verdict now needs corroboration: if the person
     * holding this has not actually gone anywhere, the conclusion is refused
     * rather than scored down. A tracker that has been beside you the whole
     * time you sat at one table has not followed you anywhere, however many
     * neighbouring routers rebooted.
     *
     * pharos_sense_travelled() returns true when there is no IMU, so a board
     * without one behaves exactly as it did before this existed. */
    s_motion_gated = false;
    if (s_verdict.band >= PV_BAND_FOLLOWING &&
        !pharos_sense_travelled(s_step_mark)) {
        s_verdict.band = PV_BAND_PERSISTENT;
        /* The ceiling, not the score, because this is an observation-quality
         * limit of exactly the kind the rest of the project already models:
         * we cannot see far enough to say the stronger thing. */
        if (s_verdict.ceiling > 55u) {
            s_verdict.ceiling = 55u;
        }
        if (s_verdict.score > s_verdict.ceiling) {
            s_verdict.score = s_verdict.ceiling;
        }
        s_verdict.headline = "seen repeatedly - but you have not moved";
        s_motion_gated = true;
    }

    if (s_verdict.band != s_last_band) {
        ESP_LOGI(TAG, "%s score=%u/%u tags=%u following=%u locales=%u \"%s\"",
                 pv_band_name(s_verdict.band), s_verdict.score, s_verdict.ceiling,
                 s_verdict.n_tags, s_verdict.n_following, s_verdict.n_locales,
                 s_verdict.headline);
        s_last_band = s_verdict.band;
    }
    xSemaphoreGive(s_lock);
}

/* One device from the table, under the lock, so the UI never reads the engine
 * while the analytics core is writing it. */
bool pharos_lens_vigil_tag(unsigned index, pv_tag_t *out, uint32_t *minutes)
{
    if (!out || !s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    const bool ok = pv_tag_at(&s_engine, index, (uint64_t)esp_timer_get_time(),
                              out, minutes);
    xSemaphoreGive(s_lock);
    return ok;
}

bool pharos_lens_vigil_snapshot(pv_verdict_t *out)
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

/* Mark the currently most-suspicious tag as the operator's own. Deliberate,
 * like adopting a baseline: your earbuds are supposed to follow you, and the
 * device should not pretend otherwise or quietly fudge the score. */
bool pharos_lens_vigil_mark_known(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    bool ok = false;
    if (s_verdict.n_tags && s_verdict.worst_kind != PV_KIND_UNKNOWN) {
        ok = pv_mark_known(&s_engine, s_verdict.worst_addr);
    }
    xSemaphoreGive(s_lock);
    if (ok) {
        ESP_LOGI(TAG, "marked the current tag as yours; it is now excluded");
    }
    return ok;
}

bool pharos_lens_vigil_report(char *buf, size_t cap, prt_redact_t redact, uint32_t salt)
{
    prt_t w;
    prt_init(&w, buf, cap, redact, salt);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_str(&w, "lens", "ble.vigil");
    prt_u32(&w, "uptime_ms", (uint32_t)(esp_timer_get_time() / 1000));
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        prt_str(&w, "band", pv_band_name(s_verdict.band));
        prt_u32(&w, "score", s_verdict.score);
        prt_u32(&w, "ceiling", s_verdict.ceiling);
        prt_u32(&w, "notes", s_verdict.notes);
        prt_u32(&w, "trackers_seen", s_verdict.n_tags);
        prt_u32(&w, "following", s_verdict.n_following);
        prt_u32(&w, "locales", s_verdict.n_locales);
        if (s_verdict.n_tags) {
            prt_mac(&w, "worst_addr", s_verdict.worst_addr);
            prt_str(&w, "worst_kind", pv_kind_name(s_verdict.worst_kind));
            prt_u32(&w, "worst_locales", s_verdict.worst_locales);
            prt_u32(&w, "worst_minutes", s_verdict.worst_minutes);
        }
        prt_str(&w, "advice", pv_band_advice(s_verdict.band));
        xSemaphoreGive(s_lock);
    }
    prt_obj_end(&w);
    return prt_finish(&w);
}

/* Aegis: a tracker travelling with you is surveillance of a person rather than
 * of a network, so it reports as RECON - somebody is gathering information. */
static bool k_vigil_stage(uint8_t *stage, uint8_t *score, uint8_t *ceiling)
{
    *stage = 0; /* PA_STAGE_RECON */
    *score = s_verdict.score;
    *ceiling = s_verdict.ceiling;
    return true;
}

static struct pharos_bus *vigil_ingest(void) { return &s_bus; }

static bool k_vigil_display(struct pharos_lens_display *o)
{
    pv_verdict_t v = s_verdict;
    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", pv_band_name(v.band));
    snprintf(o->detail, sizeof(o->detail), "%u tags  %u following  %u places",
             v.n_tags, v.n_following, v.n_locales);
    snprintf(o->advice, sizeof(o->advice), "%s", v.headline ? v.headline : "");
    o->score = v.score; o->ceiling = v.ceiling; o->has_score = true;

    /* TRACKERS NEARBY ARE ORDINARY. BEING FOLLOWED IS NOT.
     *
     * This lens' own advice says as much - "trackers nearby, which is ordinary
     * in a public place" - and then it handed the ring a score of 24 that got
     * read as a finding anyway. The distinction the engine actually draws is
     * between something being present and something MOVING WITH YOU, so that
     * is the distinction the ring gets. */
    o->has_alert = true;
    o->alert = (v.band >= PV_BAND_FOLLOWING)  ? 3u
             : (v.band >= PV_BAND_PERSISTENT) ? 2u
             : (v.band >= PV_BAND_SEEN)       ? 1u
                                              : 0u;
    o->has_history = pharos_pulse_fill(&s_pulse, (uint64_t)esp_timer_get_time(), o->history);
    return true;
}

/* The tags themselves. A score saying "something is following you" that
 * cannot name WHICH device is a fright without a remedy; the address is what
 * lets somebody search a bag. */
static bool k_vigil_row(unsigned index, struct pharos_lens_row *out)
{
    /* Motion first: it is the thing that decides whether anything below it
     * can be called following. Showing the verdict without showing this would
     * leave somebody unable to tell a quiet room from a refused conclusion. */
    if (index == 0) {
        pm_verdict_t mv;
        pharos_sense_motion(&mv);
        snprintf(out->left, sizeof(out->left), "you are");
        if (!mv.present) {
            snprintf(out->right, sizeof(out->right), "unknown");
            out->tone = PHAROS_TONE_DIM;
        } else {
            snprintf(out->right, sizeof(out->right), "%.11s",
                     pm_state_name(mv.state));
            out->tone = (mv.state == PM_STILL) ? PHAROS_TONE_WARN
                                               : PHAROS_TONE_GOOD;
        }
        return true;
    }
    if (index == 1) {
        snprintf(out->left, sizeof(out->left), "steps since opened");
        const uint32_t now_steps = pharos_sense_steps();
        snprintf(out->right, sizeof(out->right), "%u",
                 (unsigned)(now_steps > s_step_mark ? now_steps - s_step_mark : 0u));
        out->tone = s_motion_gated ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    }
    if (index == 2 && s_motion_gated) {
        /* Say the refusal out loud. A capped verdict that does not explain
         * itself is indistinguishable from a broken one. */
        snprintf(out->left, sizeof(out->left), "held back: you have not");
        snprintf(out->right, sizeof(out->right), "moved");
        out->tone = PHAROS_TONE_WARN;
        return true;
    }
    index -= s_motion_gated ? 3u : 2u;
    pv_verdict_t v;
    if (!pharos_lens_vigil_snapshot(&v)) {
        return false;
    }
    /* Four summary rows, then EVERY DEVICE, named.
     *
     * The summary answers "should I care"; the list answers "what do I look
     * for". A verdict saying something is following you that cannot say which
     * device is a fright with no remedy - you cannot search a bag for a
     * score. */
    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "trackers seen");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_tags);
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "following you");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_following);
        out->tone = v.n_following ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "places visited");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_locales);
        /* Following means nothing until you have MOVED, so the number that
         * makes the verdict meaningful sits next to it. */
        out->tone = (v.n_locales < 2) ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    case 3:
        /* Said out loud on the page, because a quiet screen here is the most
         * dangerous thing this device can show. See PV_BREDR_BLIND. */
        snprintf(out->left, sizeof(out->left), "classic BT");
        snprintf(out->right, sizeof(out->right), "deaf");
        out->tone = PHAROS_TONE_WARN;
        return true;
    default:
        break;
    }

    /* The devices themselves, worst first. */
    pv_tag_t t;
    uint32_t mins = 0;
    if (!pharos_lens_vigil_tag(index - 4u, &t, &mins)) {
        return false;
    }
    snprintf(out->left, sizeof(out->left), "%.13s %02x:%02x:%02x",
             pv_kind_name(t.kind), t.addr[3], t.addr[4], t.addr[5]);
    if (t.n_locales >= 2) {
        /* The one fact that turns a neighbour into a follower. */
        snprintf(out->right, sizeof(out->right), "%up %um",
                 (unsigned)t.n_locales, (unsigned)mins);
        out->tone = PHAROS_TONE_BAD;
    } else {
        snprintf(out->right, sizeof(out->right), "%u min", (unsigned)mins);
        out->tone = (t.kind == PV_KIND_FLIPPER || t.kind == PV_KIND_SERIAL)
                        ? PHAROS_TONE_WARN
                        : PHAROS_TONE_NEUTRAL;
    }
    return true;
}


static const pharos_lens_t k_vigil = {
    .id = "ble.vigil",
    .purpose = "trackers following you",
    .name = "Vigil",
    .summary = "Is an item tracker travelling with you?",
    .glyph = "eye",
    .kind = PHAROS_LENS_OBSERVE,
    /* Both radios: BLE to hear the tags, Wi-Fi to know whether you moved. */
    .caps = PHAROS_CAP_BLE_SCAN | PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN |
            PHAROS_CAP_STORAGE_W,
    .budget_ma = 150,
    .on_mount = vigil_mount,
    .on_start = vigil_start,
    .on_stop = vigil_stop,
    .on_tick = vigil_tick,
    .on_event = vigil_event,
    .ingest = vigil_ingest,
    .stage_report = k_vigil_stage,
    .display = k_vigil_display,
    .row = k_vigil_row,
    .row_head_left = "DEVICE",
    .row_head_right = "SEEN",
};

PHAROS_LENS_REGISTER(&k_vigil);
