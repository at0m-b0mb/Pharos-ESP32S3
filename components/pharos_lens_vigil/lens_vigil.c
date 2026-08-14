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
#include "pharos_lens.h"
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

static void vigil_event(const pharos_event_t *ev)
{
    if (!ev) {
        return;
    }
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
    if (s_verdict.band != s_last_band) {
        ESP_LOGI(TAG, "%s score=%u/%u tags=%u following=%u locales=%u \"%s\"",
                 pv_band_name(s_verdict.band), s_verdict.score, s_verdict.ceiling,
                 s_verdict.n_tags, s_verdict.n_following, s_verdict.n_locales,
                 s_verdict.headline);
        s_last_band = s_verdict.band;
    }
    xSemaphoreGive(s_lock);
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
    return true;
}

static const pharos_lens_t k_vigil = {
    .id = "ble.vigil",
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
};

PHAROS_LENS_REGISTER(&k_vigil);
