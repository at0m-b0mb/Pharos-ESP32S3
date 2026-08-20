/* Pharos lens: Ward - watch ONE network, the one you care about
 *
 * Every other radio lens here grades the whole room, which is the right thing
 * for a sweep and the wrong thing for the commonest defensive job there is:
 * somebody has a network they are responsible for, and they want to know if
 * anybody is doing something to IT.
 *
 * Watch will happily report a deauthentication flood - against the café
 * downstairs. Census grades the neighbours alongside your own access point.
 * Nothing on the device could be told "this one is mine".
 *
 * Ward is that. Pick a network, and it camps on that network's channel and
 * feeds the detection engine ONLY frames belonging to it. Everything the
 * engine then says is about your network, which means:
 *
 *   - the score is not diluted by a busy room, and not inflated by somebody
 *     else's argument two floors down;
 *   - camping raises the confidence ceiling, because the receiver is standing
 *     still on one channel rather than sweeping thirteen (see pharos_watch.h);
 *   - "nothing is happening" becomes a statement about your network rather
 *     than about the average of every network in earshot.
 *
 * ---------------------------------------------------------------------------
 * WHAT REPLACED WHAT, AND WHY
 *
 * This lens took the place of Range, a training simulator that played
 * synthesised attacks through the real engines. The arithmetic was genuine and
 * the traffic was not, so it wore a SIMULATION banner - and a device with two
 * screens showing invented data is a device somebody learns to distrust. The
 * scenarios were not thrown away: Footprint still plays them on demand, where
 * a drill is explicitly asked for and explicitly labelled.
 *
 * ---------------------------------------------------------------------------
 * THE HONESTY THAT MATTERS HERE
 *
 * Filtering to one BSSID is a strong claim: it means everything reported is
 * about that network. So the filter has to be exact, and the lens has to be
 * clear when it has nothing to filter FOR. Until a target is chosen it says so
 * and grades nothing - it does not quietly fall back to watching everything
 * and let somebody believe the reading is about their network.
 *
 * It cannot transmit, and it cannot protect anything by itself. It watches.
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
#include "pharos_lens.h"
#include "pharos_lens_census.h"
#include "pharos_radio.h"
#include "pharos_watch.h"

static const char *TAG = "lens.ward";

#define WARD_RING 512

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[WARD_RING];
static pharos_bus_t s_bus;
EXT_RAM_BSS_ATTR static pw_engine_t s_engine;
static pw_verdict_t s_verdict;
static SemaphoreHandle_t s_lock;

/* The network under guard. All-zero means none chosen yet. */
static uint8_t s_target[6];
static char s_target_ssid[33];
static uint8_t s_target_channel;
static bool s_have_target;

/* Counted separately so the lens can tell "your network is quiet" from "I have
 * not heard your network at all", which are very different facts - the second
 * one usually means you walked out of range, and reporting it as quiet would
 * be the most dangerous kind of reassurance. */
static uint32_t s_frames_mine;
static uint32_t s_frames_seen;
static uint64_t s_last_mine_us;

static bool ward_mount(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    pw_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    s_frames_mine = 0;
    s_frames_seen = 0;
    s_last_mine_us = 0;
    return s_lock && pharos_bus_init(&s_bus, s_slots, WARD_RING);
}

static bool ward_start(void)
{
    /* Camp when there is something to camp on. Standing still on one channel
     * is what buys the confidence ceiling, and it is only defensible once the
     * operator has said which network they mean. */
    pharos_scan_plan_t plan = (s_have_target && s_target_channel)
                                  ? pharos_scan_plan_camp(s_target_channel)
                                  : pharos_scan_plan_survey();
    plan.want_mgmt = true;
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void ward_stop(void) { pharos_radio_rx_stop(); }

/* Does this frame belong to the network under guard?
 *
 * A management frame's BSSID is normally a3, but for the frames that matter
 * most here - a deauthentication aimed at a client - the address that names
 * the network can be the transmitter or the receiver depending on direction.
 * Matching any of the three catches an attack in either direction while still
 * being exact about WHICH network it concerns. */
static bool frame_is_mine(const pharos_ev_dot11_t *d)
{
    return memcmp(d->a3, s_target, 6) == 0 ||
           memcmp(d->a2, s_target, 6) == 0 ||
           memcmp(d->a1, s_target, 6) == 0;
}

static void ward_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }
    s_frames_seen++;
    if (!s_have_target) {
        return; /* nothing to filter for; grade nothing */
    }
    if (!frame_is_mine(&ev->u.dot11)) {
        return;
    }
    s_frames_mine++;
    s_last_mine_us = ev->t_us;
    pw_observe(&s_engine, &ev->u.dot11, ev->t_us);
}

static struct pharos_bus *ward_ingest(void) { return &s_bus; }

/* Adopt a network. Called from the console and from the detail page. */
void pharos_lens_ward_guard(const uint8_t bssid[6], const char *ssid,
                            uint8_t channel)
{
    if (!bssid) {
        return;
    }
    memcpy(s_target, bssid, 6);
    snprintf(s_target_ssid, sizeof(s_target_ssid), "%s", ssid ? ssid : "");
    s_target_channel = channel;
    s_have_target = true;

    /* A new target means the old evidence is about a different network. */
    pw_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    s_frames_mine = 0;
    s_last_mine_us = 0;

    ESP_LOGI(TAG, "guarding %02x:%02x:%02x:%02x:%02x:%02x \"%s\" on ch%u",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
             s_target_ssid, (unsigned)channel);

    if (pharos_lens_active() && strcmp(pharos_lens_active()->id, "wifi.ward") == 0) {
        pharos_radio_rx_stop();
        ward_start();
    }
}

bool pharos_lens_ward_target(uint8_t bssid[6], char *ssid, size_t cap,
                             uint8_t *channel)
{
    if (!s_have_target) {
        return false;
    }
    if (bssid)   memcpy(bssid, s_target, 6);
    if (ssid && cap) snprintf(ssid, cap, "%s", s_target_ssid);
    if (channel) *channel = s_target_channel;
    return true;
}

/* PICK THE STRONGEST NETWORK, as a starting point only.
 *
 * The nearest access point is very often the operator's own, which makes it a
 * reasonable default - and a default is all it is. The lens says which network
 * it adopted, on the glass, so a wrong guess is visible immediately rather
 * than quietly framing every later reading. */
static void ward_adopt_strongest(void)
{
    pc_ap_t ap;
    pc_verdict_t v;
    int8_t best = -127;
    pc_ap_t chosen;
    bool found = false;

    for (unsigned i = 0; pharos_lens_census_at(i, &ap, &v); i++) {
        if (ap.rssi > best) {
            best = ap.rssi;
            chosen = ap;
            found = true;
        }
    }
    if (found) {
        char name[33];
        const uint8_t n = chosen.ssid_len > 32 ? 32 : chosen.ssid_len;
        memcpy(name, chosen.ssid, n);
        name[n] = '\0';
        pharos_lens_ward_guard(chosen.bssid, name, chosen.channel);
    }
}

static void ward_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    const uint64_t now = (uint64_t)esp_timer_get_time();
    const uint8_t chan = pharos_radio_channel();
    pw_context_t ctx = {
        .dwell_permil = pharos_radio_dwell_permil(chan),
        .bus_yield_permil = pharos_bus_yield_permil(&s_bus),
        .window_ms = 15000,
    };
    pw_evaluate(&s_engine, now, &ctx, &s_verdict);
    xSemaphoreGive(s_lock);
}

/* The centre tap: adopt the strongest network, or release it. */
static void ward_select(void)
{
    if (s_have_target) {
        s_have_target = false;
        pw_reset(&s_engine);
        ESP_LOGI(TAG, "released; nothing under guard");
        pharos_radio_rx_stop();
        ward_start();
    } else {
        ward_adopt_strongest();
    }
}

static bool k_ward_display(struct pharos_lens_display *o)
{
    pw_verdict_t v;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    v = s_verdict;
    const bool have = s_have_target;
    const uint32_t mine = s_frames_mine;
    const uint32_t seen = s_frames_seen;
    const uint64_t last = s_last_mine_us;
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s", s_target_ssid);
    const uint8_t ch = s_target_channel;
    xSemaphoreGive(s_lock);

    if (!have) {
        /* NO TARGET IS NOT A CLEAN BILL OF HEALTH. */
        snprintf(o->big, sizeof(o->big), "--");
        snprintf(o->band, sizeof(o->band), "no network chosen");
        snprintf(o->detail, sizeof(o->detail), "%u frames in earshot",
                 (unsigned)seen);
        snprintf(o->advice, sizeof(o->advice), "%s",
                 "Tap centre to guard the nearest network.");
        o->has_score = false;
        o->has_alert = true;
        o->alert = 0;
        return true;
    }

    const uint64_t now = (uint64_t)esp_timer_get_time();
    const uint32_t quiet_s =
        last ? (uint32_t)((now - last) / 1000000ull) : 0u;

    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", pw_band_name(v.band));
    snprintf(o->detail, sizeof(o->detail), "%.14s  ch%u",
             ssid[0] ? ssid : "<hidden>", (unsigned)ch);

    if (!mine) {
        /* OUT OF RANGE IS NOT QUIET. Reporting "nothing is happening" about a
         * network the receiver cannot hear is the most dangerous sentence
         * this lens could produce. */
        snprintf(o->why, sizeof(o->why), "%s", "not heard yet - are you in range?");
        snprintf(o->advice, sizeof(o->advice), "%s",
                 "Nothing heard from it. Move closer to be sure.");
        o->has_score = false;
        o->has_alert = true;
        o->alert = 1;
        return true;
    }
    if (quiet_s > 60u) {
        snprintf(o->why, sizeof(o->why), "last heard %us ago", (unsigned)quiet_s);
    } else {
        snprintf(o->why, sizeof(o->why), "%u frames of yours", (unsigned)mine);
    }
    snprintf(o->advice, sizeof(o->advice), "%s", pw_band_advice(v.band));

    o->families = v.families;
    o->fam_label[0] = "RATE";
    o->fam_label[1] = "SHAPE";
    o->fam_label[2] = "FORGE";
    o->fam_label[3] = "AFTER";
    o->score = v.score;
    o->raw_score = v.raw_score;
    o->ceiling = v.ceiling;
    o->has_score = true;
    /* This one IS a threat scale - it is the deauthentication engine, pointed
     * at one network - so the ring may read it off the score. */
    return true;
}

static bool k_ward_row(unsigned index, struct pharos_lens_row *out)
{
    pw_verdict_t v;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    v = s_verdict;
    const bool have = s_have_target;
    const uint32_t mine = s_frames_mine, seen = s_frames_seen;
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s", s_target_ssid);
    const uint8_t ch = s_target_channel;
    uint8_t addr[6];
    memcpy(addr, s_target, 6);
    xSemaphoreGive(s_lock);

    if (!have) {
        if (index == 0) {
            snprintf(out->left, sizeof(out->left), "no network guarded");
            snprintf(out->right, sizeof(out->right), "--");
            out->tone = PHAROS_TONE_WARN;
            return true;
        }
        if (index == 1) {
            snprintf(out->left, sizeof(out->left), "tap centre to adopt");
            snprintf(out->right, sizeof(out->right), "nearest");
            out->tone = PHAROS_TONE_DIM;
            return true;
        }
        return false;
    }

    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "guarding");
        snprintf(out->right, sizeof(out->right), "%.11s",
                 ssid[0] ? ssid : "<hidden>");
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "address");
        snprintf(out->right, sizeof(out->right), "%02x:%02x:%02x", addr[3],
                 addr[4], addr[5]);
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "channel  (camped)");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)ch);
        out->tone = pharos_radio_is_camped() ? PHAROS_TONE_GOOD
                                             : PHAROS_TONE_WARN;
        return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "its frames heard");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)mine);
        out->tone = mine ? PHAROS_TONE_GOOD : PHAROS_TONE_BAD;
        return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "everything else");
        snprintf(out->right, sizeof(out->right), "%u",
                 (unsigned)(seen > mine ? seen - mine : 0u));
        /* Dim on purpose: the whole point is that this number is ignored. */
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 5:
        snprintf(out->left, sizeof(out->left), "disconnects at it");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.observed);
        out->tone = v.observed ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD;
        return true;
    case 6:
        snprintf(out->left, sizeof(out->left), "confidence ceiling");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.ceiling);
        out->tone = (v.ceiling >= 80) ? PHAROS_TONE_GOOD : PHAROS_TONE_WARN;
        return true;
    case 7: {
        snprintf(out->left, sizeof(out->left), "clients knocked off");
        snprintf(out->right, sizeof(out->right), "%u",
                 (unsigned)v.distinct_victims);
        out->tone = v.distinct_victims ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
        return true;
    }
    case 8: {
        const char *why = pw_forgery_name(v.forgery);
        snprintf(out->left, sizeof(out->left), "%.25s",
                 (why && why[0]) ? why : "no forgery seen");
        snprintf(out->right, sizeof(out->right), "%s",
                 (why && why[0]) ? "tell" : "ok");
        out->tone = (why && why[0]) ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
        return true;
    }
    default:
        return false;
    }
}

static const pharos_lens_t k_ward = {
    .id = "wifi.ward",
    .name = "Ward",
    .summary = "Guard one network: camp on it and watch only what targets it",
    .glyph = "shield",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN,
    .budget_ma = 135,
    .on_mount = ward_mount,
    .on_start = ward_start,
    .on_stop = ward_stop,
    .on_tick = ward_tick,
    .on_event = ward_event,
    .ingest = ward_ingest,
    .display = k_ward_display,
    .on_select = ward_select,
    .row = k_ward_row,
    .row_head_left = "UNDER GUARD",
    .row_head_right = "VALUE",
};

PHAROS_LENS_REGISTER(&k_ward);
