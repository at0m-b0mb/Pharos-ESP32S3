/* Pharos lenses: Census and Twin
 *
 * Two lenses in one component because they share one thing: the table of
 * access points heard so far. Census grades each one on its own; Twin grades
 * the groups that share an SSID against each other. Splitting the table
 * between two components would mean surveying twice for the same answer.
 *
 * As everywhere in Pharos, no judgement happens in this file. Census calls
 * pc_grade(), Twin calls pt_evaluate(), and both of those are pure functions
 * with host tests. What lives here is the beacon-to-record translation and
 * the snapshot the UI reads.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_census.h"
#include "pharos_dot11.h"
#include "pharos_lens.h"
#include "pharos_radio.h"
#include "pharos_report.h"
#include "pharos_twin.h"

static const char *TAG = "lens.census";

#define CENSUS_RING 512
#define CENSUS_MAX_AP 64

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[CENSUS_RING];
static pharos_bus_t s_bus;

EXT_RAM_BSS_ATTR static pc_ap_t s_aps[CENSUS_MAX_AP];
EXT_RAM_BSS_ATTR static pc_verdict_t s_grades[CENSUS_MAX_AP];
static unsigned s_n_aps;
static SemaphoreHandle_t s_lock;

static pt_profile_t s_profile;
static pt_verdict_t s_twin_worst;
static char s_twin_ssid[PC_SSID_MAX + 1];

static pc_ap_t *table_find(const uint8_t bssid[6])
{
    for (unsigned i = 0; i < s_n_aps; i++) {
        if (memcmp(s_aps[i].bssid, bssid, 6) == 0) {
            return &s_aps[i];
        }
    }
    return NULL;
}

static pc_ap_t *table_admit(const uint8_t bssid[6])
{
    pc_ap_t *ap = table_find(bssid);
    if (ap) {
        return ap;
    }
    if (s_n_aps >= CENSUS_MAX_AP) {
        return NULL; /* full: the UI shows the count so this is never silent */
    }
    ap = &s_aps[s_n_aps++];
    memset(ap, 0, sizeof(*ap));
    memcpy(ap->bssid, bssid, 6);
    return ap;
}

static bool census_mount(void)
{
    memset(s_aps, 0, sizeof(s_aps));
    memset(s_grades, 0, sizeof(s_grades));
    s_n_aps = 0;
    memset(&s_twin_worst, 0, sizeof(s_twin_worst));
    s_twin_ssid[0] = '\0';
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL && pharos_bus_init(&s_bus, s_slots, CENSUS_RING);
}

static bool census_start(void)
{
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    plan.dwell_ms = 350; /* beacons arrive every ~100 ms; three per visit */
    plan.want_mgmt = true;
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void census_stop(void)
{
    pharos_radio_rx_stop();
}

/* Analytics core. The fixed header arrived on the bus; the element chain is
 * walked here, off the radio's hot path, exactly as the architecture says. */
static void census_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }
    const pharos_ev_dot11_t *f = &ev->u.dot11;
    if (f->type != PHAROS_FT_MGMT) {
        return;
    }
    if (f->subtype != PHAROS_ST_BEACON && f->subtype != PHAROS_ST_PROBE_RESP) {
        return;
    }

    pc_ap_t *ap = table_admit(f->a2);
    if (!ap) {
        return;
    }
    ap->beacons++;
    ap->rssi = f->rssi;
    ap->channel = f->channel;
    ap->privacy = (f->flags & PHAROS_DOT11_F_PROTECTED) != 0;
    if (f->flags & PHAROS_DOT11_F_MFP_SEEN) {
        ap->rsn.mfp_capable = true;
    }
    /* The full element walk needs the frame body, which the capture ring
     * holds rather than the summary bus. M5 wires that path; until then the
     * record carries what the fixed header can tell us, and pc_grade
     * declines to grade anything it cannot see the RSN element for. */
}

static void census_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    for (unsigned i = 0; i < s_n_aps; i++) {
        pc_grade(&s_aps[i], &s_grades[i]);
    }
    xSemaphoreGive(s_lock);
}

/* Copy the table out for the UI. Returns how many records were written. */
unsigned pharos_lens_census_snapshot(pc_ap_t *aps, pc_verdict_t *grades, unsigned max)
{
    if (!aps || !grades || !s_lock) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }
    const unsigned n = (s_n_aps < max) ? s_n_aps : max;
    memcpy(aps, s_aps, n * sizeof(pc_ap_t));
    memcpy(grades, s_grades, n * sizeof(pc_verdict_t));
    xSemaphoreGive(s_lock);
    return n;
}

/* Writes a session report into caller-provided storage. Redaction is applied
 * here, at write time - see docs/POLICY.md for why never at export. */
bool pharos_lens_census_report(char *buf, size_t cap, prt_redact_t redact, uint32_t salt)
{
    prt_t w;
    prt_init(&w, buf, cap, redact, salt);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_str(&w, "lens", "wifi.census");
    prt_u32(&w, "uptime_ms", (uint32_t)(esp_timer_get_time() / 1000));

    pharos_radio_stats_t st;
    pharos_radio_stats(&st);
    prt_obj_begin(&w, "observation");
    prt_u32(&w, "frames_seen", st.frames_seen);
    prt_u32(&w, "frames_dropped", st.frames_dropped);
    prt_u32(&w, "bus_yield_permil", pharos_bus_yield_permil(&s_bus));
    prt_u32(&w, "channel_changes", st.channel_changes);
    prt_bool(&w, "band_5ghz", false); /* this radio is deaf above 2.4 GHz */
    prt_obj_end(&w);

    prt_arr_begin(&w, "access_points");
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (unsigned i = 0; i < s_n_aps; i++) {
            prt_obj_begin(&w, NULL);
            prt_mac(&w, "bssid", s_aps[i].bssid);
            prt_strn(&w, "ssid", s_aps[i].ssid, s_aps[i].ssid_len);
            prt_u32(&w, "channel", s_aps[i].channel);
            prt_i32(&w, "rssi", s_aps[i].rssi);
            prt_u32(&w, "beacons", s_aps[i].beacons);
            prt_str(&w, "grade", pc_grade_name(s_grades[i].grade));
            prt_u32(&w, "score", s_grades[i].score);
            prt_u32(&w, "ceilings", s_grades[i].caps_applied);
            prt_str(&w, "advice", pc_grade_advice(&s_grades[i]));
            prt_obj_end(&w);
        }
        xSemaphoreGive(s_lock);
    }
    prt_arr_end(&w);
    prt_obj_end(&w);
    return prt_finish(&w);
}

/* ---- Twin: the same table, read as groups --------------------------- */

static void twin_tick(uint32_t dt_ms)
{
    census_tick(dt_ms);
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }

    pt_context_t ctx = {
        .dwell_permil = pharos_radio_dwell_permil(pharos_radio_channel()),
    };
    pt_verdict_t worst;
    memset(&worst, 0, sizeof(worst));

    for (unsigned i = 0; i < s_n_aps; i++) {
        if (s_aps[i].ssid_len == 0) {
            continue; /* hidden networks cannot be grouped by name */
        }
        /* Only evaluate a group from its first member, so each SSID is
         * judged once per pass. */
        bool first = true;
        for (unsigned k = 0; k < i; k++) {
            if (s_aps[k].ssid_len == s_aps[i].ssid_len &&
                memcmp(s_aps[k].ssid, s_aps[i].ssid, s_aps[i].ssid_len) == 0) {
                first = false;
                break;
            }
        }
        if (!first) {
            continue;
        }

        pc_ap_t group[PT_MAX_GROUP];
        unsigned n = 0;
        for (unsigned j = 0; j < s_n_aps && n < PT_MAX_GROUP; j++) {
            if (s_aps[j].ssid_len == s_aps[i].ssid_len &&
                memcmp(s_aps[j].ssid, s_aps[i].ssid, s_aps[i].ssid_len) == 0) {
                group[n++] = s_aps[j];
            }
        }
        if (n < 2) {
            continue;
        }

        pt_verdict_t v;
        pt_evaluate(group, n, &s_profile, &ctx, &v);
        if (v.score > worst.score) {
            worst = v;
            memcpy(s_twin_ssid, s_aps[i].ssid, s_aps[i].ssid_len);
            s_twin_ssid[s_aps[i].ssid_len] = '\0';
        }
    }

    if (worst.band != s_twin_worst.band) {
        ESP_LOGI(TAG, "twin: %s \"%s\" score=%u/%u families=0x%02x",
                 pt_band_name(worst.band), s_twin_ssid, worst.score, worst.ceiling,
                 worst.families);
    }
    s_twin_worst = worst;
    xSemaphoreGive(s_lock);
}

bool pharos_lens_twin_snapshot(pt_verdict_t *out, char *ssid, size_t ssid_cap)
{
    if (!out || !s_lock) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    *out = s_twin_worst;
    if (ssid && ssid_cap) {
        strncpy(ssid, s_twin_ssid, ssid_cap - 1);
        ssid[ssid_cap - 1] = '\0';
    }
    xSemaphoreGive(s_lock);
    return true;
}

/* Adopt everything currently in view as the site baseline. A deliberate act:
 * it is only meaningful when the operator knows the estate is clean, which is
 * why it is a button and not something the firmware does on its own. */
unsigned pharos_lens_twin_adopt_profile(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return 0;
    }
    pt_profile_reset(&s_profile);
    for (unsigned i = 0; i < s_n_aps; i++) {
        pt_profile_add(&s_profile, s_aps[i].bssid);
    }
    const unsigned n = s_profile.n;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "site baseline adopted: %u access points", n);
    return n;
}

static struct pharos_bus *census_ingest(void) { return &s_bus; }

static const pharos_lens_t k_census = {
    .id = "wifi.census",
    .name = "Census",
    .summary = "Grades every nearby network on what it takes to break in",
    .glyph = "list",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_STORAGE_W,
    .budget_ma = 130,
    .on_mount = census_mount,
    .on_start = census_start,
    .on_stop = census_stop,
    .on_tick = census_tick,
    .on_event = census_event,
    .ingest = census_ingest,
};

/* Aegis: Twin speaks for the IMPERSONATE stage. Census itself reports nothing -
 * grading how strong the neighbours' Wi-Fi is says nothing about whether an
 * attack is under way, and feeding it in would be noise. */
static bool k_twin_stage(uint8_t *stage, uint8_t *score, uint8_t *ceiling)
{
    *stage = 1; /* PA_STAGE_IMPERSONATE */
    *score = s_twin_worst.score;
    *ceiling = s_twin_worst.ceiling;
    return true;
}

static const pharos_lens_t k_twin = {
    .id = "wifi.twin",
    .name = "Twin",
    .summary = "Finds the radio wearing a name that belongs to somebody else",
    .glyph = "masks",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_STORAGE_W,
    .budget_ma = 130,
    .on_mount = census_mount,
    .on_start = census_start,
    .on_stop = census_stop,
    .on_tick = twin_tick,
    .on_event = census_event,
    .ingest = census_ingest,
    .stage_report = k_twin_stage,
};

PHAROS_LENS_REGISTER(&k_census);
PHAROS_LENS_REGISTER(&k_twin);
