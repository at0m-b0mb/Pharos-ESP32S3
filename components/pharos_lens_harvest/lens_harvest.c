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
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_harvest.h"
#include "pharos_pulse.h"
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

/* CAMPING IS NOT A LUXURY FOR THIS LENS, IT IS THE DIFFERENCE BETWEEN
 * CATCHING THE ATTACK AND NOT.
 *
 * A four-way handshake completes in about a hundred milliseconds. Hopping
 * thirteen channels at 900 ms means this lens is on any given channel for
 * roughly eight per cent of the time, so it would miss the very thing it
 * exists to see about twelve times out of thirteen - and the FORCED family
 * needs the ORDER of a disconnect and the handshake that follows it, so a hop
 * landing between the two destroys the evidence even when both are in range.
 *
 * Watch has had an operator camp since v2. Harvest, which needs it more, did
 * not. */
static bool s_camp_requested;
static uint8_t s_camp_channel;

static bool harvest_start(void)
{
    pharos_scan_plan_t plan;
    if (s_camp_requested) {
        plan = pharos_scan_plan_camp(s_camp_channel);
    } else {
        plan = pharos_scan_plan_survey();
        /* Handshakes are brief and the ordering is the evidence, so this lens
         * camps longer than the survey lenses do: a hop mid-handshake loses
         * the very sequence it is looking for. */
        plan.dwell_ms = 900;
    }
    plan.want_mgmt = true;
    plan.want_data = true; /* the handshake itself rides in data frames */
    return pharos_radio_rx_start(&plan, &s_bus);
}

void pharos_lens_harvest_camp(uint8_t channel)
{
    s_camp_requested = true;
    s_camp_channel = channel;
    if (pharos_lens_active() &&
        strcmp(pharos_lens_active()->id, "wifi.harvest") == 0) {
        pharos_radio_rx_stop();
        harvest_start();
        ESP_LOGI(TAG, "camping on channel %u (operator)", channel);
    }
}

void pharos_lens_harvest_survey(void)
{
    s_camp_requested = false;
    if (pharos_lens_active() &&
        strcmp(pharos_lens_active()->id, "wifi.harvest") == 0) {
        pharos_radio_rx_stop();
        harvest_start();
        ESP_LOGI(TAG, "surveying all channels (operator)");
    }
}

static void harvest_stop(void)
{
    pharos_radio_rx_stop();
}

/* The shared activity ribbon: one call per event in, one call per repaint
 * out. Before this, every lens but Watch drew an empty timeline. */
static pharos_pulse_t s_pulse;

static void harvest_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }

    pharos_pulse_note(&s_pulse, ev->t_us);
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

static bool k_harvest_display(struct pharos_lens_display *o)
{
    ph_verdict_t v = s_verdict;
    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", ph_band_name(v.band));
    /* THE NUMBERS THAT FIRED, NOT A FIXED THREE.
     *
     * This read "forced 0  pmkid 0  ceil 96" during a live attack that had
     * been caught by the association family - three zeros under a SUSPECTED
     * headline, which reads as a device that has broken rather than one that
     * has found something. */
    snprintf(o->detail, sizeof(o->detail), "joined %u/%u  pmkid %u  ceil %u",
             (unsigned)v.touch_and_go, (unsigned)v.assoc_reqs,
             (unsigned)v.pmkid_orphans, v.ceiling);
    snprintf(o->advice, sizeof(o->advice), "%s", v.headline ? v.headline : "");
    o->score = v.score; o->ceiling = v.ceiling; o->has_score = true;
    /* WHY it thinks so, on the glass. The engine has computed these families
     * all along; nothing was carrying them to the face, so this lens lit no
     * chips at all and its score had to be taken on trust. */
    /* FIVE FAMILIES, FOUR CHIPS.
     *
     * PH_FAM_TOUCH_GO is bit 4 and the face carries PHAROS_DISP_FAMILIES = 4,
     * so passing the engine's bitmap straight through did two wrong things at
     * once: the family that had actually fired could not be drawn at all, and
     * REPEAT (bit 2) lit whichever chip sat in slot 2 regardless of what that
     * slot was labelled.
     *
     * So the display bitmap is BUILT rather than forwarded. REPEAT and BREADTH
     * both mean "more than one victim" and share the last chip; the
     * association family gets one of its own, because it is now the evidence
     * most likely to be the reason the score moved. */
    uint8_t fam = 0;
    if (v.families & PH_FAM_FORCED)   fam |= 1u << 0;
    if (v.families & PH_FAM_PMKID)    fam |= 1u << 1;
    if (v.families & PH_FAM_TOUCH_GO) fam |= 1u << 2;
    if (v.families & (PH_FAM_REPEAT | PH_FAM_BREADTH)) fam |= 1u << 3;
    o->families = fam;
    o->fam_label[0] = "FORCED";
    o->fam_label[1] = "PMKID";
    o->fam_label[2] = "JOINED";
    o->fam_label[3] = "SPREAD";
    o->has_history = pharos_pulse_fill(&s_pulse, (uint64_t)esp_timer_get_time(), o->history);
    return true;
}

/* Which client is being forced off, and by whom. The score says somebody is
 * farming handshakes; these rows say whose. */
static bool k_harvest_row(unsigned index, struct pharos_lens_row *out)
{
    ph_verdict_t v;
    if (!pharos_lens_harvest_snapshot(&v)) {
        return false;
    }
    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "forced cycles");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.forced_cycles);
        out->tone = v.forced_cycles ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "handshakes seen");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.handshakes);
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "PMKID requests");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.pmkid_orphans);
        out->tone = v.pmkid_orphans ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD;
        return true;
    case 3:
        /* THE FRAME WE CAN ACTUALLY RELY ON SEEING. A live PMKID attack ran
         * for minutes against this device and every EAPOL counter stayed at
         * zero, because message 1 is one brief data frame. The association
         * request that precedes it is management: unencrypted, always
         * delivered, and unskippable. */
        snprintf(out->left, sizeof(out->left), "associations seen");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.assoc_reqs);
        out->tone = v.assoc_reqs ? PHAROS_TONE_NEUTRAL : PHAROS_TONE_DIM;
        return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "joined, never used it");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.touch_and_go);
        out->tone = (v.touch_and_go >= 2) ? PHAROS_TONE_BAD
                  : v.touch_and_go ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD;
        return true;
    case 5:
        snprintf(out->left, sizeof(out->left), "clients affected");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.victims);
        out->tone = v.victims ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
        return true;
    case 6:
        snprintf(out->left, sizeof(out->left), "worst client");
        snprintf(out->right, sizeof(out->right), "%02x:%02x:%02x",
                 v.worst_client[3], v.worst_client[4], v.worst_client[5]);
        out->tone = v.victims ? PHAROS_TONE_BAD : PHAROS_TONE_DIM;
        return true;
    case 7:
        snprintf(out->left, sizeof(out->left), "on network");
        snprintf(out->right, sizeof(out->right), "%02x:%02x:%02x",
                 v.worst_bssid[3], v.worst_bssid[4], v.worst_bssid[5]);
        out->tone = PHAROS_TONE_DIM;
        return true;
    default:
        return false;
    }
}

static const pharos_lens_t k_harvest = {
    .id = "wifi.harvest",
    .purpose = "handshake capture",
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
    .display = k_harvest_display,
    .row = k_harvest_row,
    .row_head_left = "HANDSHAKE FARMING",
    .row_head_right = "COUNT",
};

PHAROS_LENS_REGISTER(&k_harvest);
