/* Pharos lens: Harvest - somebody is collecting your handshakes
 *
 * Watches for the attack that ends in a cracked Wi-Fi password: knock a client
 * off, catch the first two messages of the 4-way handshake as it comes back,
 * and attack the passphrase offline at leisure. Or skip the client entirely and
 * solicit a PMKID from the access point itself.
 *
 * This is the one lens that needs DATA frames as well as management ones - the
 * handshake rides in data frames - so its scan plan asks for both. All of the
 * judgement is in the pure, host-tested engine (pharos_harvest); this file only
 * feeds it frames and hands the verdict out.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_harvest.h"
#include "pharos_lens.h"
#include "pharos_radio.h"
#include "pharos_report.h"

static const char *TAG = "lens.harvest";

#define HARVEST_RING 512

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[HARVEST_RING];
static pharos_bus_t s_bus;

EXT_RAM_BSS_ATTR static ph_state_t s_engine;
static ph_verdict_t s_verdict;
static SemaphoreHandle_t s_lock;
static ph_band_t s_last_band;

static bool harvest_mount(void)
{
    ph_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    s_last_band = PH_BAND_QUIET;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL && pharos_bus_init(&s_bus, s_slots, HARVEST_RING);
}

static bool harvest_start(void)
{
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    /* Handshakes are brief and the ordering is the evidence, so this lens
     * camps longer than the survey lenses do: a hop mid-handshake loses the
     * very sequence it is looking for. */
    plan.dwell_ms = 900;
    plan.want_mgmt = true;
    plan.want_data = true; /* the handshake itself rides in data frames */
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void harvest_stop(void)
{
    pharos_radio_rx_stop();
}

static void harvest_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }
    ph_observe(&s_engine, &ev->u.dot11, ev->t_us);
}

static void harvest_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    ph_context_t ctx = {
        .dwell_permil = (uint16_t)pharos_radio_dwell_permil(pharos_radio_channel()),
        .yield_permil = pharos_bus_yield_permil(&s_bus),
        .mfp_required = false, /* the engine also learns this from the air */
    };
    ph_settle(&s_engine);
    ph_evaluate(&s_engine, &ctx, &s_verdict);

    if (s_verdict.band != s_last_band) {
        ESP_LOGI(TAG, "%s score=%u/%u forced=%u pmkid=%u victims=%u \"%s\"",
                 ph_band_name(s_verdict.band), s_verdict.score, s_verdict.ceiling,
                 (unsigned)s_verdict.forced_cycles, (unsigned)s_verdict.pmkid_orphans,
                 (unsigned)s_verdict.victims, s_verdict.headline);
        s_last_band = s_verdict.band;
    }
    xSemaphoreGive(s_lock);
}

bool pharos_lens_harvest_snapshot(ph_verdict_t *out)
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

bool pharos_lens_harvest_report(char *buf, size_t cap, prt_redact_t redact, uint32_t salt)
{
    prt_t w;
    prt_init(&w, buf, cap, redact, salt);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_str(&w, "lens", "wifi.harvest");
    prt_u32(&w, "uptime_ms", (uint32_t)(esp_timer_get_time() / 1000));
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        prt_str(&w, "band", ph_band_name(s_verdict.band));
        prt_u32(&w, "score", s_verdict.score);
        prt_u32(&w, "ceiling", s_verdict.ceiling);
        prt_u32(&w, "families", s_verdict.families);
        prt_u32(&w, "forced_cycles", s_verdict.forced_cycles);
        prt_u32(&w, "pmkid_orphans", s_verdict.pmkid_orphans);
        prt_u32(&w, "victims", s_verdict.victims);
        prt_u32(&w, "handshakes", s_verdict.handshakes);
        if (s_verdict.victims) {
            prt_mac(&w, "worst_client", s_verdict.worst_client);
            prt_mac(&w, "worst_bssid", s_verdict.worst_bssid);
        }
        prt_str(&w, "advice", ph_band_advice(s_verdict.band));
        xSemaphoreGive(s_lock);
    }
    prt_obj_end(&w);
    return prt_finish(&w);
}

/* Aegis: this lens speaks for the HARVEST stage. */
static bool k_harvest_stage(uint8_t *stage, uint8_t *score, uint8_t *ceiling)
{
    *stage = 3; /* PA_STAGE_HARVEST */
    *score = s_verdict.score;
    *ceiling = s_verdict.ceiling;
    return true;
}

static struct pharos_bus *harvest_ingest(void) { return &s_bus; }

static const pharos_lens_t k_harvest = {
    .id = "wifi.harvest",
    .name = "Harvest",
    .summary = "Catches somebody collecting handshakes to crack offline",
    .glyph = "key",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_STORAGE_W,
    .budget_ma = 140,
    .on_mount = harvest_mount,
    .on_start = harvest_start,
    .on_stop = harvest_stop,
    .on_tick = harvest_tick,
    .on_event = harvest_event,
    .ingest = harvest_ingest,
    .stage_report = k_harvest_stage,
};

PHAROS_LENS_REGISTER(&k_harvest);
