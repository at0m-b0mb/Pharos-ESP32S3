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
#include "pharos_pulse.h"
#include "pharos_skew.h"
#include "pharos_lens.h"
#include "pharos_lens_census.h"
#include "pharos_survey.h"
#include "pharos_survey_hook.h"
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
/* The shared activity ribbon: one call per event in, one call per repaint
 * out. Before this, every lens but Watch drew an empty timeline. */
static pharos_pulse_t s_pulse;

/* The physical half of Twin's evidence: which oscillator is behind a name.
 * See pharos_skew.h - every other family here is something the attacker
 * chose to write, and this one is not. */
EXT_RAM_BSS_ATTR static psk_engine_t s_skew;

static void census_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }

    pharos_pulse_note(&s_pulse, ev->t_us);

    if (ev->u.dot11.tsf) {
        psk_observe(&s_skew, ev->u.dot11.a3, ev->u.dot11.tsf, ev->t_us);
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

static bool k_census_display(struct pharos_lens_display *o)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    /* THE WORST GRADE, AND THE COMPARISON THAT WAS THE WRONG WAY ROUND.
     *
     * pc_grade_t counts UPWARD to better: UNGRADED, F, E, D, C, B, A, A+. This
     * loop started at A_PLUS - the maximum - and looked for a grade GREATER
     * than it, which nothing can be. It therefore never fired, `worst` stayed
     * at zero, and the headline reported whichever access point happened to
     * land at index 0 while the line under it said "worst of N".
     *
     * That is the worst class of bug this project can have: not a crash, not a
     * blank screen, but a confident wrong answer that looks exactly like a
     * right one. A room containing an open network would show a B because the
     * first AP heard happened to be a B.
     *
     * UNGRADED is skipped rather than treated as the worst: "not enough
     * observation to say anything" is not a bad grade, and letting it win
     * would report every room as unassessable the moment a new AP appeared. */
    unsigned n = s_n_aps;
    bool have_graded = false;
    pc_grade_t worst_g = PC_GRADE_A_PLUS;
    unsigned worst = 0;
    for (unsigned i = 0; i < s_n_aps; i++) {
        const pc_grade_t g = s_grades[i].grade;
        if (g == PC_GRADE_UNGRADED) {
            continue;
        }
        if (!have_graded || g < worst_g) {
            have_graded = true;
            worst_g = g;
            worst = i;
        }
    }
    const pc_verdict_t v = have_graded ? s_grades[worst] : (pc_verdict_t){ 0 };
    const unsigned n_graded = have_graded ? n : 0u;
    xSemaphoreGive(s_lock);
    n = n_graded;

    if (!n) {
        snprintf(o->big, sizeof(o->big), "--");
        snprintf(o->band, sizeof(o->band), "listening");
        snprintf(o->detail, sizeof(o->detail), "no networks heard yet");
        o->has_score = false;
        return true;
    }
    /* The headline is the WORST grade in the room, because that is the one
     * that matters - an estate is as strong as its softest network. */
    snprintf(o->big, sizeof(o->big), "%s", pc_grade_name(v.grade));
    snprintf(o->band, sizeof(o->band), "worst of %u", n);
    /* A posture grade is not a sweep-confidence reading: pc_verdict_t has no
     * ceiling, it has the CAPS it applied - the reasons a grade could not go
     * higher. Report those instead of inventing a ceiling. */
    snprintf(o->detail, sizeof(o->detail), "score %u  caps 0x%02x", v.score,
             (unsigned)v.caps_applied);
    snprintf(o->advice, sizeof(o->advice), "%s", pc_grade_advice(&v));
    o->score = v.score; o->ceiling = 100; o->has_score = true;

    /* CENSUS IS A SURVEY, NOT A WATCH.
     *
     * This score says how badly configured the networks AROUND you are. A
     * neighbour with WPS switched on scores high and is not attacking anybody
     * - it is not even your network. Left to derive severity from the number,
     * the home ring turned that into a red dot and a screen shouting ALERT,
     * which is the alarm-inventing this project refuses everywhere else.
     *
     * So the ring is told directly: worth knowing, never an alarm. The one
     * exception is an OPEN network carrying traffic, which is a live exposure
     * rather than a weak configuration - and even that is ELEVATED, not an
     * attack in progress. */
    o->has_alert = true;
    if (v.caps_applied & PC_CAP_OPEN) {
        /* A network with no key at all is a live exposure rather than a weak
         * configuration - and still not an attack in progress. */
        o->alert = 2u;
    } else if (v.caps_applied) {
        o->alert = 1u;
    } else {
        /* NOTHING ACTIONABLE FOUND IS NOT "WORTH KNOWING".
         *
         * This was pinned at 1 unconditionally, so Census reported "worth a
         * look" about a room whose networks were all fine - and about a room
         * it had not finished grading. A watch with nothing to say must say
         * nothing, or the ring's amber stops meaning anything. */
        o->alert = 0u;
    }

    /* FEED THE SESSION SURVEY.
     *
     * Census grades every network it hears and then loses all of it when the
     * rotation moves on five seconds later - which is most of what this device
     * knows about a place, thrown away. The survey deduplicates by address, so
     * pushing the whole table on every display call is both correct and the
     * simplest thing that works. */
    /* ONCE A SECOND, NOT TEN TIMES.
     *
     * display() is called at the repaint rate, and walking the whole table to
     * push facts the survey has already deduplicated is work with no output.
     * The survey only needs to have heard each thing once; a second is far
     * inside the rotation's own dwell, so nothing is missed. */
    static uint64_t s_fed_us;
    const uint64_t feed_now = (uint64_t)esp_timer_get_time();
    if (feed_now - s_fed_us >= 1000000ull) {
        s_fed_us = feed_now;
    if (s_lock && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        for (unsigned i = 0; i < s_n_aps; i++) {
            const pc_ap_t *ap = &s_aps[i];
            const pc_verdict_t *g = &s_grades[i];
            uint32_t f = 0;
            const uint8_t c = g->caps_applied;
            if (c & PC_CAP_OPEN)    f |= PSV_NET_OPEN;
            if (c & PC_CAP_NO_MFP)  f |= PSV_NET_NO_MFP;
            if (c & PC_CAP_WPS_PIN) f |= PSV_NET_WPS;
            if (c & (PC_CAP_WEP | PC_CAP_WPA1 | PC_CAP_TKIP)) f |= PSV_NET_WEAK;
            if (ap->hidden) f |= PSV_NET_HIDDEN;
            if (ap->rsn.has_sae || ap->rsn.has_owe) f |= PSV_NET_MODERN;
            /* Only networks heard well enough to grade. An ungraded entry is
             * an admission of not knowing, and must not be counted as a fault
             * nor as good news. */
            if (g->grade != PC_GRADE_UNGRADED) {
                pharos_survey_network(ap->bssid, (uint8_t)g->grade, f);
            }
        }
        xSemaphoreGive(s_lock);
    }
    }
    o->has_history = pharos_pulse_fill(&s_pulse, (uint64_t)esp_timer_get_time(), o->history);
    return true;
}

/* THE LIST. This is what the operator actually came for.
 *
 * A grade on the front is a summary, and a summary you cannot open is a claim
 * rather than a finding. Census knows every network it heard, its channel, how
 * strong it is and exactly why it scored what it did - so it can say so.
 *
 * Sorted worst-first, because the softest network in the room is the one you
 * do something about, and it is the one the headline grade came from.
 *
 * Runs on the UI task, so the lock is taken with a zero timeout: a detail page
 * that stalls the repaint to be complete is a worse trade than one that
 * redraws a frame later. */
static bool k_census_row(unsigned index, struct pharos_lens_row *out)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    /* Rank with the ENGINE's own comparator, not a hand-rolled one.
     *
     * The first version of this sorted by grade ascending, which put
     * PC_GRADE_UNGRADED - enum value 0 - at the very top, so the list opened
     * with seven rows of "--" and the actual findings were pushed onto page
     * two. pc_compare() already knows better: ungraded last because it is
     * unknown rather than good, then worst grade first, then strongest
     * signal. Its comment says so in as many words, which is what makes
     * duplicating it here a mistake rather than a shortcut.
     *
     * The table itself is never reordered - it belongs to the analytics core
     * and this is only a view of it. */
    unsigned order[CENSUS_MAX_AP];
    const unsigned n = s_n_aps;
    for (unsigned i = 0; i < n; i++) {
        order[i] = i;
    }
    for (unsigned i = 1; i < n; i++) {
        const unsigned key = order[i];
        unsigned j = i;
        while (j > 0 &&
               pc_compare(&s_aps[key], &s_grades[key],
                          &s_aps[order[j - 1]], &s_grades[order[j - 1]]) < 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }

    bool ok = false;
    if (index < n) {
        const pc_ap_t *ap = &s_aps[order[index]];
        const pc_verdict_t *v = &s_grades[order[index]];

        /* Name, then where it is and how loud - a network you cannot locate is
         * not actionable. A hidden SSID is named as hidden rather than left
         * blank, because "no name" is itself the finding. */
        char name[16];
        if (ap->hidden || ap->ssid_len == 0) {
            snprintf(name, sizeof(name), "<hidden>");
        } else {
            snprintf(name, sizeof(name), "%.14s", ap->ssid);
        }
        /* Padded AND truncated to 14: the pad keeps the columns aligned down
         * the page, and without the precision a long SSID would push the
         * channel and level off the end of the row. */
        snprintf(out->left, sizeof(out->left), "%-14.14s c%-2u %d", name,
                 (unsigned)ap->channel, (int)ap->rssi);
        snprintf(out->right, sizeof(out->right), "%s", pc_grade_name(v->grade));

        /* The tone is the grade, so the column reads as a traffic light down
         * the page without anyone having to know the letters. */
        if (v->grade >= PC_GRADE_A)        out->tone = PHAROS_TONE_GOOD;
        else if (v->grade >= PC_GRADE_C)   out->tone = PHAROS_TONE_WARN;
        else if (v->grade == PC_GRADE_UNGRADED) out->tone = PHAROS_TONE_DIM;
        else                               out->tone = PHAROS_TONE_BAD;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* OPENING ONE NETWORK.
 *
 * The list answers "which is worst". This answers "why", which is the question
 * anybody who intends to DO something has to ask next - a grade nobody can
 * interrogate is a claim rather than a finding.
 *
 * Everything here is what was actually heard in the beacon, not a lookup: the
 * cipher offered, whether management frames are protected, whether WPS is
 * advertised, and - the one that matters most - the specific ceiling that
 * stopped the grade going higher, which is the single thing to fix. */
static bool k_census_expand(unsigned row, unsigned sub,
                            struct pharos_lens_row *out)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    unsigned order[CENSUS_MAX_AP];
    const unsigned n = s_n_aps;
    for (unsigned i = 0; i < n; i++) {
        order[i] = i;
    }
    for (unsigned i = 1; i < n; i++) {
        const unsigned key = order[i];
        unsigned j = i;
        while (j > 0 &&
               pc_compare(&s_aps[key], &s_grades[key],
                          &s_aps[order[j - 1]], &s_grades[order[j - 1]]) < 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    if (row >= n) {
        xSemaphoreGive(s_lock);
        return false;
    }

    /* AN INDEX IS NOT AN IDENTITY.
     *
     * This list re-sorts as the air changes - a grade shifts, a network stops
     * beaconing, a new one appears - so row 3 is not the same network from one
     * second to the next. Reading the AP out by index on every line meant a
     * page could begin describing one network and finish describing another,
     * with nothing on the glass to say so. On a tool whose whole purpose is
     * telling you which network is weak, that is the worst kind of wrong:
     * quietly, plausibly attributing one network's failings to another.
     *
     * So the page latches the whole record on its first line and reads every
     * subsequent line out of that copy. The UI always walks a page from sub 0,
     * so the latch is taken exactly when the page starts being drawn. */
    static pc_ap_t s_open_ap;
    static pc_verdict_t s_open_v;
    if (sub == 0) {
        s_open_ap = s_aps[order[row]];
        s_open_v = s_grades[order[row]];
    }
    xSemaphoreGive(s_lock);
    const pc_ap_t ap = s_open_ap;
    const pc_verdict_t v = s_open_v;

    out->tone = PHAROS_TONE_NEUTRAL;
    switch (sub) {
    case 0:
        snprintf(out->left, sizeof(out->left), "network");
        snprintf(out->right, sizeof(out->right), "%.11s",
                 (ap.hidden || !ap.ssid_len) ? "<hidden>" : ap.ssid);
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "grade");
        snprintf(out->right, sizeof(out->right), "%s  %u",
                 pc_grade_name(v.grade), v.score);
        out->tone = (v.grade >= PC_GRADE_A)      ? PHAROS_TONE_GOOD
                  : (v.grade >= PC_GRADE_C)      ? PHAROS_TONE_WARN
                  : (v.grade == PC_GRADE_UNGRADED) ? PHAROS_TONE_DIM
                                                   : PHAROS_TONE_BAD;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "address");
        snprintf(out->right, sizeof(out->right), "%02x:%02x:%02x",
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "channel / signal");
        snprintf(out->right, sizeof(out->right), "%u / %d", (unsigned)ap.channel,
                 (int)ap.rssi);
        return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "authentication");
        if (!ap.privacy && !ap.rsn.has_rsn) {
            snprintf(out->right, sizeof(out->right), "OPEN");
            out->tone = PHAROS_TONE_BAD;
        } else if (ap.rsn.has_sae) {
            snprintf(out->right, sizeof(out->right), "WPA3");
            out->tone = PHAROS_TONE_GOOD;
        } else if (ap.rsn.has_owe) {
            snprintf(out->right, sizeof(out->right), "OWE");
            out->tone = PHAROS_TONE_WARN;
        } else if (ap.akm_8021x) {
            snprintf(out->right, sizeof(out->right), "802.1X");
            out->tone = PHAROS_TONE_GOOD;
        } else if (ap.rsn.has_rsn) {
            snprintf(out->right, sizeof(out->right), "WPA2 PSK");
            out->tone = PHAROS_TONE_WARN;
        } else {
            snprintf(out->right, sizeof(out->right), "WEP/WPA1");
            out->tone = PHAROS_TONE_BAD;
        }
        return true;
    case 5:
        /* The one that decides whether a deauthentication flood works here. */
        snprintf(out->left, sizeof(out->left), "802.11w (MFP)");
        if (ap.rsn.mfp_required) {
            snprintf(out->right, sizeof(out->right), "required");
            out->tone = PHAROS_TONE_GOOD;
        } else if (ap.rsn.mfp_capable) {
            snprintf(out->right, sizeof(out->right), "optional");
            out->tone = PHAROS_TONE_WARN;
        } else {
            snprintf(out->right, sizeof(out->right), "NONE");
            out->tone = PHAROS_TONE_BAD;
        }
        return true;
    case 6:
        snprintf(out->left, sizeof(out->left), "cipher");
        snprintf(out->right, sizeof(out->right), "%s",
                 ap.tkip_pairwise ? "TKIP" : (ap.ccmp_pairwise ? "CCMP" : "none"));
        out->tone = ap.tkip_pairwise ? PHAROS_TONE_BAD
                  : (ap.ccmp_pairwise ? PHAROS_TONE_GOOD : PHAROS_TONE_WARN);
        return true;
    case 7:
        snprintf(out->left, sizeof(out->left), "WPS");
        snprintf(out->right, sizeof(out->right), "%s",
                 ap.wps_pin ? "PIN" : (ap.wps_present ? "on" : "off"));
        out->tone = ap.wps_pin ? PHAROS_TONE_BAD
                  : (ap.wps_present ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD);
        return true;
    case 8:
        snprintf(out->left, sizeof(out->left), "hidden name");
        snprintf(out->right, sizeof(out->right), "%s", ap.hidden ? "yes" : "no");
        /* Amber either way when hidden: hiding an SSID is not security, and a
         * green tick here would suggest it were. */
        out->tone = ap.hidden ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    case 9:
        snprintf(out->left, sizeof(out->left), "beacons heard");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)ap.beacons);
        out->tone = (ap.beacons < 3u) ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    case 10: {
        /* THE ONE THING TO FIX.
         *
         * This read "nothing" on a network graded C with two amber rows above
         * it, which is worse than saying nothing at all - it told somebody
         * their network was fine while the page said otherwise. The cause: it
         * only looked at the CAPS, and a network can lose most of its points
         * to ordinary deductions without tripping a single ceiling.
         *
         * So caps first, because a ceiling is the hardest bound, and then the
         * component that actually lost the most of its allowance. */
        snprintf(out->left, sizeof(out->left), "fix first");
        const uint8_t c = v.caps_applied;
        const char *w = (c & PC_CAP_OPEN)    ? "add a key"
                      : (c & PC_CAP_WEP)     ? "drop WEP"
                      : (c & PC_CAP_WPA1)    ? "drop WPA1"
                      : (c & PC_CAP_TKIP)    ? "drop TKIP"
                      : (c & PC_CAP_WPS_PIN) ? "kill WPS"
                      : (c & PC_CAP_NO_MFP)  ? "turn on 11w"
                                             : NULL;
        if (!w) {
            /* Points lost, out of each component's own allowance. */
            const int lost_auth = 45 - (int)v.c_auth;
            const int lost_mfp = 25 - (int)v.c_mfp;
            const int lost_ciph = 15 - (int)v.c_cipher;
            const int lost_exp = 15 - (int)v.c_exposure;
            int worst = lost_auth;
            w = "use WPA3";
            if (lost_mfp > worst) { worst = lost_mfp; w = "turn on 11w"; }
            if (lost_ciph > worst) { worst = lost_ciph; w = "the cipher"; }
            if (lost_exp > worst) {
                worst = lost_exp;
                /* Exposure is WPS and the hidden-SSID theatre; name whichever
                 * of them this network is actually doing. */
                w = ap.wps_present ? "kill WPS" : "stop hiding";
            }
            if (worst <= 0) {
                w = "nothing";
            }
        }
        snprintf(out->right, sizeof(out->right), "%s", w);
        out->tone = (v.grade >= PC_GRADE_A) ? PHAROS_TONE_GOOD : PHAROS_TONE_BAD;
        return true;
    }
    case 11:
        /* Where the points went, so "fix first" is a conclusion rather than an
         * assertion. 45/25/15/15 are the engine's own allowances. */
        snprintf(out->left, sizeof(out->left), "auth + 11w, of 70");
        snprintf(out->right, sizeof(out->right), "%u+%u",
                 (unsigned)(v.c_auth > 45u ? 45u : v.c_auth),
                 (unsigned)(v.c_mfp > 25u ? 25u : v.c_mfp));
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 12:
        snprintf(out->left, sizeof(out->left), "cipher + rest, of 30");
        snprintf(out->right, sizeof(out->right), "%u+%u",
                 (unsigned)(v.c_cipher > 15u ? 15u : v.c_cipher),
                 (unsigned)(v.c_exposure > 15u ? 15u : v.c_exposure));
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 13:
        if (!(v.notes & (PC_NOTE_TRANSITION | PC_NOTE_THIN | PC_NOTE_HIDDEN |
                         PC_NOTE_OWE | PC_NOTE_ENTERPRISE))) {
            return false;
        }
        snprintf(out->left, sizeof(out->left), "%.25s",
                 (v.notes & PC_NOTE_TRANSITION) ? "WPA3 falls back to WPA2"
               : (v.notes & PC_NOTE_OWE)        ? "encrypted, open to all"
               : (v.notes & PC_NOTE_ENTERPRISE) ? "802.1X: keys per user"
               : (v.notes & PC_NOTE_THIN)       ? "graded on few beacons"
                                                : "hiding a name is not it");
        snprintf(out->right, sizeof(out->right), "note");
        out->tone = (v.notes & (PC_NOTE_TRANSITION | PC_NOTE_THIN))
                        ? PHAROS_TONE_WARN
                        : PHAROS_TONE_DIM;
        return true;
    default:
        return false;
    }
}

static const pharos_lens_t k_census = {
    .id = "wifi.census",
    .purpose = "network security",
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
    .display = k_census_display,
    .row = k_census_row,
    .row_head_left = "NETWORK   CH  dBm",
    .row_head_right = "GRADE",
    .row_expand = k_census_expand,
};

/* ---- reading the table from another lens; see pharos_lens_census.h ---- */

bool pharos_lens_census_at(unsigned index, pc_ap_t *ap, pc_verdict_t *verdict)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    const bool ok = (index < s_n_aps);
    if (ok) {
        if (ap)      *ap = s_aps[index];
        if (verdict) *verdict = s_grades[index];
    }
    xSemaphoreGive(s_lock);
    return ok;
}

unsigned pharos_lens_census_count(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return 0;
    }
    const unsigned n = s_n_aps;
    xSemaphoreGive(s_lock);
    return n;
}

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

static bool k_twin_display(struct pharos_lens_display *o)
{
    pt_verdict_t v = s_twin_worst;
    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", pt_band_name(v.band));
    snprintf(o->detail, sizeof(o->detail), "\"%s\"  ceil %u", s_twin_ssid, v.ceiling);
    snprintf(o->advice, sizeof(o->advice), "%s", pt_band_advice(v.band));
    o->score = v.score; o->ceiling = v.ceiling; o->has_score = true;

    /* Three heuristics and one measurement. The fourth chip is lit only by
     * something the attacker cannot choose: a BSSID whose crystal changed
     * rate is a BSSID with a different radio behind it. */
    o->families = (uint8_t)(v.families |
                            (psk_any_changed(&s_skew) ? (1u << 3) : 0u));
    o->fam_label[0] = "POSTURE";
    o->fam_label[1] = "NAME";
    o->fam_label[2] = "ACTS";
    o->fam_label[3] = "CLOCK";
    o->has_history = pharos_pulse_fill(&s_pulse, (uint64_t)esp_timer_get_time(),
                                       o->history);
    return true;
}

/* Twin names the radio it suspects and says what is wrong with it. A score
 * saying "somebody is impersonating a network" without naming WHICH network
 * and WHICH radio is a fright with no remedy. */
static bool k_twin_row(unsigned index, struct pharos_lens_row *out)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    const pt_verdict_t v = s_twin_worst;
    char ssid[PC_SSID_MAX + 1];
    snprintf(ssid, sizeof(ssid), "%s", s_twin_ssid);
    xSemaphoreGive(s_lock);

    /* The measurement first, because it is the only line here an attacker
     * cannot arrange to look innocent. */
    if (index == 0u) {
        psk_verdict_t k;
        snprintf(out->left, sizeof(out->left), "clock behind name");
        if (psk_first_changed(&s_skew, &k, 0)) {
            /* Clamped so the width is provable; two crystals tens of ppm
             * apart is the interesting case and it fits easily. */
            int a = k.from_ppm;
            int b = k.to_ppm;
            if (a > 99) { a = 99; }
            if (a < -99) { a = -99; }
            if (b > 99) { b = 99; }
            if (b < -99) { b = -99; }
            snprintf(out->right, sizeof(out->right), "%d>%d", a, b);
            out->tone = PHAROS_TONE_BAD;
        } else {
            unsigned ready = 0, measuring = 0;
            psk_progress(&s_skew, &ready, &measuring);
            if (ready) {
                snprintf(out->right, sizeof(out->right), "%u steady",
                         ready > 99u ? 99u : ready);
                out->tone = PHAROS_TONE_GOOD;
            } else if (measuring) {
                snprintf(out->right, sizeof(out->right), "measuring");
                out->tone = PHAROS_TONE_DIM;
            } else {
                snprintf(out->right, sizeof(out->right), "-");
                out->tone = PHAROS_TONE_DIM;
            }
        }
        return true;
    }
    index--;

    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "network");
        snprintf(out->right, sizeof(out->right), "%.11s", ssid[0] ? ssid : "--");
        out->tone = PHAROS_TONE_NEUTRAL; return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "radios on that name");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.members);
        /* Multiplicity alone scores ZERO - a roaming estate is many radios on
         * one name and is not an attack. On the page so nobody reads the
         * count as the finding. */
        out->tone = PHAROS_TONE_DIM; return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "suspect radio");
        snprintf(out->right, sizeof(out->right), "%02x:%02x:%02x",
                 v.suspect[3], v.suspect[4], v.suspect[5]);
        out->tone = v.score >= 40 ? PHAROS_TONE_BAD : PHAROS_TONE_DIM; return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "security gap");
        snprintf(out->right, sizeof(out->right), "%u vs %u",
                 (unsigned)v.worst_grade_score, (unsigned)v.best_grade_score);
        /* The posture family: one radio on the name is weaker than its
         * siblings, which is what an evil twin has to be to be useful. */
        out->tone = (v.worst_grade_score + 20u < v.best_grade_score)
                        ? PHAROS_TONE_BAD : PHAROS_TONE_NEUTRAL;
        return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "louder than siblings");
        snprintf(out->right, sizeof(out->right), "%d dB", (int)v.rssi_excess);
        out->tone = (v.rssi_excess >= 10) ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    default: return false;
    }
}

static const pharos_lens_t k_twin = {
    .id = "wifi.twin",
    .purpose = "evil twin APs",
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
    .display = k_twin_display,
    .row = k_twin_row,
    .row_head_left = "IMPERSONATION",
    .row_head_right = "VALUE",
};

PHAROS_LENS_REGISTER(&k_census);
PHAROS_LENS_REGISTER(&k_twin);
