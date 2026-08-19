/* Pharos lens: Sentinel - the site baseline and what changed since
 *
 * The other lenses answer "is anything wrong right now". Sentinel answers the
 * question that actually starts incidents: "what changed since I last swept
 * this building?" It surveys the band exactly as Census does, builds the same
 * table of access points, and then - once you have adopted a baseline while
 * you believe the estate is clean - diffs every later sweep against it.
 *
 * All of the judgement lives in the pure, host-tested engine (pharos_sentinel):
 * this file only translates beacons into records and hands the table to
 * ps_adopt()/ps_compare(). Adopting is a deliberate operator act - a button on
 * the dial, or `sentinel adopt` on the console - never something the firmware
 * decides for itself, because a baseline is only meaningful when a human
 * vouches that the estate was clean at that moment.
 */
#include <stdio.h>
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
#include "pharos_sentinel.h"

static const char *TAG = "lens.sentinel";

#define SENTINEL_RING 512
#define SENTINEL_MAX_AP 64

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[SENTINEL_RING];
static pharos_bus_t s_bus;

EXT_RAM_BSS_ATTR static pc_ap_t s_aps[SENTINEL_MAX_AP];
static unsigned s_n_aps;
static SemaphoreHandle_t s_lock;

EXT_RAM_BSS_ATTR static ps_baseline_t s_baseline;
EXT_RAM_BSS_ATTR static ps_verdict_t s_verdict;
static uint32_t s_sweep_ms;
static ps_band_t s_last_band;

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
    if (s_n_aps >= SENTINEL_MAX_AP) {
        return NULL; /* full: the UI shows the count so this is never silent */
    }
    ap = &s_aps[s_n_aps++];
    memset(ap, 0, sizeof(*ap));
    memcpy(ap->bssid, bssid, 6);
    return ap;
}

static bool sentinel_mount(void)
{
    memset(s_aps, 0, sizeof(s_aps));
    s_n_aps = 0;
    s_sweep_ms = 0;
    memset(&s_verdict, 0, sizeof(s_verdict));
    s_last_band = PS_BAND_UNCHANGED;
    /* The baseline deliberately survives a mount: an operator adopts once and
     * expects it to hold while they walk the site and re-enter the lens. It is
     * cleared only by an explicit re-adopt or a power cycle. */
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL && pharos_bus_init(&s_bus, s_slots, SENTINEL_RING);
}

static bool sentinel_start(void)
{
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    plan.dwell_ms = 350; /* three beacons per visit, same as Census */
    plan.want_mgmt = true;
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void sentinel_stop(void)
{
    pharos_radio_rx_stop();
}

static void sentinel_event(const pharos_event_t *ev)
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

    /* Name and posture, now that the hot path carries them. Before v1.13.0
     * this record was left empty and the grading engines - which correctly
     * refuse to grade what they cannot see - returned nothing at all. */
    if (f->ssid_len) {
        const uint8_t n = f->ssid_len > PC_SSID_MAX ? PC_SSID_MAX : f->ssid_len;
        memcpy(ap->ssid, f->ssid, n);
        ap->ssid[n] = '\0';
        ap->ssid_len = n;
        ap->hidden = false;
    } else if (ap->ssid_len == 0) {
        ap->hidden = true; /* beaconing without a name */
    }
    if (f->rsn_flags & PHAROS_RSN_F_PRESENT) {
        ap->rsn.has_rsn = true;
        ap->rsn.mfp_capable  = (f->rsn_flags & PHAROS_RSN_F_MFP_CAPABLE) != 0;
        ap->rsn.mfp_required = (f->rsn_flags & PHAROS_RSN_F_MFP_REQUIRED) != 0;
        ap->rsn.has_sae = (f->rsn_flags & PHAROS_RSN_F_SAE) != 0;
        ap->rsn.has_psk = (f->rsn_flags & PHAROS_RSN_F_PSK) != 0;
        ap->rsn.has_owe = (f->rsn_flags & PHAROS_RSN_F_OWE) != 0;
        /* An RSN element means a modern cipher suite in practice; TKIP-only
         * networks are vanishingly rare and would need the full pairwise list
         * to prove, so this is left as CCMP rather than guessed at. */
        ap->ccmp_pairwise = true;
    }
    ap->wps_present = (f->rsn_flags & PHAROS_RSN_F_WPS) != 0;
    if (f->flags & PHAROS_DOT11_F_MFP_SEEN) {
        ap->rsn.mfp_capable = true;
    }
}

static void sentinel_tick(uint32_t dt_ms)
{
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    s_sweep_ms += dt_ms;

    ps_context_t ctx = {
        .dwell_permil = (uint16_t)pharos_radio_dwell_permil(pharos_radio_channel()),
        .sweep_ms = s_sweep_ms,
    };
    ps_compare(&s_baseline, s_aps, s_n_aps, &ctx, &s_verdict);

    if (s_verdict.band != s_last_band) {
        ESP_LOGI(TAG, "%s score=%u/%u new=%u down=%u missing=%u \"%s\"",
                 ps_band_name(s_verdict.band), s_verdict.score, s_verdict.ceiling,
                 s_verdict.n_new, s_verdict.n_downgrade, s_verdict.n_missing,
                 s_verdict.headline);
        s_last_band = s_verdict.band;
    }
    xSemaphoreGive(s_lock);
}

/* Copy the latest diff out for the UI or the console `status` line. */
bool pharos_lens_sentinel_snapshot(ps_verdict_t *out)
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

/* Adopt everything currently in view as the trusted baseline. Deliberate: it
 * is only meaningful when the operator knows the estate is clean, which is why
 * it is a command, not something the firmware does on its own. Returns how many
 * networks were captured. */
unsigned pharos_lens_sentinel_adopt(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return 0;
    }
    const unsigned n = ps_adopt(&s_baseline, s_aps, s_n_aps,
                                (uint64_t)esp_timer_get_time());
    s_sweep_ms = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "baseline adopted: %u networks", n);
    return n;
}

/* Writes a session report into caller-provided storage. Redaction is applied
 * here, at write time - see docs/POLICY.md for why never at export. */
bool pharos_lens_sentinel_report(char *buf, size_t cap, prt_redact_t redact, uint32_t salt)
{
    prt_t w;
    prt_init(&w, buf, cap, redact, salt);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_str(&w, "lens", "wifi.sentinel");
    prt_u32(&w, "uptime_ms", (uint32_t)(esp_timer_get_time() / 1000));

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        prt_obj_begin(&w, "baseline");
        prt_bool(&w, "adopted", s_baseline.adopted);
        prt_u32(&w, "networks", s_baseline.n);
        prt_obj_end(&w);

        prt_obj_begin(&w, "verdict");
        prt_str(&w, "band", ps_band_name(s_verdict.band));
        prt_u32(&w, "score", s_verdict.score);
        prt_u32(&w, "ceiling", s_verdict.ceiling);
        prt_u32(&w, "new", s_verdict.n_new);
        prt_u32(&w, "missing", s_verdict.n_missing);
        prt_u32(&w, "downgrade", s_verdict.n_downgrade);
        prt_u32(&w, "upgrade", s_verdict.n_upgrade);
        prt_u32(&w, "moved", s_verdict.n_moved);
        prt_u32(&w, "renamed", s_verdict.n_renamed);
        prt_obj_end(&w);

        prt_arr_begin(&w, "findings");
        for (unsigned i = 0; i < s_verdict.n_findings; i++) {
            const ps_finding_t *fi = &s_verdict.findings[i];
            prt_obj_begin(&w, NULL);
            prt_str(&w, "change", ps_change_name(fi->change));
            prt_mac(&w, "bssid", fi->bssid);
            prt_strn(&w, "ssid", fi->ssid, (uint8_t)strlen(fi->ssid));
            prt_u32(&w, "severity", fi->severity);
            prt_u32(&w, "grade_was", fi->was);
            prt_u32(&w, "grade_now", fi->now);
            prt_obj_end(&w);
        }
        prt_arr_end(&w);
        xSemaphoreGive(s_lock);
    }
    prt_obj_end(&w);
    return prt_finish(&w);
}

static struct pharos_bus *sentinel_ingest(void) { return &s_bus; }

/* Aegis: Sentinel speaks for the DRIFT stage - the estate not being configured
 * the way the baseline recorded it. */
static bool k_sentinel_stage(uint8_t *stage, uint8_t *score, uint8_t *ceiling)
{
    *stage = 4; /* PA_STAGE_DRIFT */
    *score = s_verdict.score;
    *ceiling = s_verdict.ceiling;
    return true;
}

static bool k_sentinel_display(struct pharos_lens_display *o)
{
    ps_verdict_t v = s_verdict;
    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", ps_band_name(v.band));
    snprintf(o->detail, sizeof(o->detail), "new %u  down %u  miss %u",
             v.n_new, v.n_downgrade, v.n_missing);
    snprintf(o->advice, sizeof(o->advice), "%s",
             s_baseline.adopted ? v.headline : "no baseline yet - console: sentinel adopt");
    o->score = v.score; o->ceiling = v.ceiling; o->has_score = true;
    return true;
}

/* What changed since the baseline, broken out by KIND. "Six findings" is not
 * actionable; "one network downgraded its security" is. */
static bool k_sentinel_row(unsigned index, struct pharos_lens_row *out)
{
    ps_verdict_t v;
    if (!pharos_lens_sentinel_snapshot(&v)) return false;
    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "new networks");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_new);
        out->tone = v.n_new ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD; return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "gone missing");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_missing);
        out->tone = v.n_missing ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD; return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "security dropped");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_downgrade);
        /* The one that matters most: a network that got WEAKER since you
         * last looked is either misconfigured or impersonated. */
        out->tone = v.n_downgrade ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD; return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "security improved");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_upgrade);
        out->tone = PHAROS_TONE_GOOD; return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "changed channel");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_moved);
        out->tone = PHAROS_TONE_DIM; return true;
    case 5:
        snprintf(out->left, sizeof(out->left), "renamed");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_renamed);
        out->tone = v.n_renamed ? PHAROS_TONE_WARN : PHAROS_TONE_DIM; return true;
    case 6:
        snprintf(out->left, sizeof(out->left), "findings total");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_findings);
        out->tone = PHAROS_TONE_DIM; return true;
    default: return false;
    }
}

static const pharos_lens_t k_sentinel = {
    .id = "wifi.sentinel",
    .name = "Sentinel",
    .summary = "What changed since you last swept this site",
    .glyph = "history",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_STORAGE_W,
    .budget_ma = 130,
    .on_mount = sentinel_mount,
    .on_start = sentinel_start,
    .on_stop = sentinel_stop,
    .on_tick = sentinel_tick,
    .on_event = sentinel_event,
    .ingest = sentinel_ingest,
    .stage_report = k_sentinel_stage,
    .display = k_sentinel_display,
    .row = k_sentinel_row,
    .row_head_left = "CHANGE SINCE BASELINE",
    .row_head_right = "COUNT",
};

PHAROS_LENS_REGISTER(&k_sentinel);
