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
#include "pharos_lens_census.h"
#include "pharos_pulse.h"
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

/* Radios announcing the guarded network's NAME that are not the guarded
 * network's own BSSID. See frame_wears_my_name(). Bounded: a name being worn
 * by more radios than this is already the finding. */
#define WARD_MAX_IMPOSTORS 4
static uint8_t s_impostor[WARD_MAX_IMPOSTORS][6];

/* Set when a tap found nothing to adopt, so the screen can say so. */
static bool s_adopt_failed;

/* NETWORKS WARD HEARD FOR ITSELF.
 *
 * Adoption used to read the Census lens' table, which made choosing a network
 * depend on a DIFFERENT lens having been run first. From a cold boot the tap
 * found nothing, and even after running Census the table is cleared on mount,
 * so the answer depended on the order somebody happened to press things in.
 * That is not a thing an operator can be expected to know.
 *
 * Ward already receives every beacon on the channels it sweeps - it simply
 * threw them away while no target was set. It now keeps them. The lens that
 * asks you to pick a network can find the networks itself, which is the only
 * arrangement that works from a cold start with nothing else running. */
#define WARD_MAX_SEEN 16
typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    bool in_use;
} ward_seen_t;
static ward_seen_t s_seen[WARD_MAX_SEEN];
static unsigned s_n_seen;

static void ward_note_network(const pharos_ev_dot11_t *d)
{
    if (d->subtype != PHAROS_ST_BEACON && d->subtype != PHAROS_ST_PROBE_RESP) {
        return;
    }
    if (d->ssid_len == 0) {
        return; /* hidden: nothing to name it by on the adopt screen */
    }
    for (unsigned i = 0; i < s_n_seen; i++) {
        if (memcmp(s_seen[i].bssid, d->a3, 6) == 0) {
            /* Keep the loudest reading: adoption picks the nearest network,
             * and one weak sample of a close AP should not demote it. */
            if (d->rssi > s_seen[i].rssi) {
                s_seen[i].rssi = d->rssi;
                s_seen[i].channel = d->channel;
            }
            return;
        }
    }
    if (s_n_seen >= WARD_MAX_SEEN) {
        /* Replace the faintest: the list exists to offer the nearest. */
        unsigned worst = 0;
        for (unsigned i = 1; i < s_n_seen; i++) {
            if (s_seen[i].rssi < s_seen[worst].rssi) worst = i;
        }
        if (d->rssi <= s_seen[worst].rssi) {
            return;
        }
        s_n_seen = worst; /* overwrite that slot below */
    }
    ward_seen_t *e = &s_seen[s_n_seen < WARD_MAX_SEEN ? s_n_seen : 0];
    memset(e, 0, sizeof(*e));
    memcpy(e->bssid, d->a3, 6);
    const uint8_t n = d->ssid_len > 32 ? 32 : d->ssid_len;
    memcpy(e->ssid, d->ssid, n);
    e->ssid[n] = '\0';
    e->channel = d->channel;
    e->rssi = d->rssi;
    e->in_use = true;
    if (s_n_seen < WARD_MAX_SEEN) s_n_seen++;
}
static unsigned s_n_impostors;
static uint32_t s_impostor_frames;
static int8_t s_impostor_rssi;

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

/* SOMEBODY ELSE ANNOUNCING YOUR NAME.
 *
 * The filter above is exact about WHICH network a frame concerns, and that is
 * right for everything Ward feeds to the deauthentication engine. But it made
 * this lens blind to the one attack most specific to its own purpose.
 *
 * Ward guards a network by BSSID. An evil twin is, by definition, a DIFFERENT
 * radio wearing your NAME - so its beacons carry your SSID and its own BSSID,
 * match none of the three addresses, and were dropped before anything looked
 * at them. The lens whose entire job is "watch MY network" could not see the
 * attack aimed at exactly that.
 *
 * The SSID was already being stored by ward_guard() and never read. This asks
 * the question it was stored for: is a radio that is not mine announcing my
 * name? That is not traffic on the guarded network - it is an impostor of it,
 * so it is counted separately and never fed to the flood engine, which is
 * about volume on one BSSID and would be misled by a second radio's beacons.
 *
 * Hidden and unnamed networks are excluded: an empty SSID matches every other
 * empty SSID, which would report the whole neighbourhood as impostors. */
static bool frame_wears_my_name(const pharos_ev_dot11_t *d)
{
    if (!s_target_ssid[0] || d->ssid_len == 0) {
        return false;
    }
    if (d->subtype != PHAROS_ST_BEACON && d->subtype != PHAROS_ST_PROBE_RESP) {
        return false;
    }
    const size_t n = strlen(s_target_ssid);
    if ((size_t)d->ssid_len != n) {
        return false;
    }
    if (memcmp(d->ssid, s_target_ssid, n) != 0) {
        return false;
    }
    /* Our own access point announcing itself is not an impostor. */
    return memcmp(d->a3, s_target, 6) != 0;
}

/* The shared activity ribbon: one call per event in, one call per repaint
 * out. Before this, every lens but Watch drew an empty timeline. */
static pharos_pulse_t s_pulse;

static void ward_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }

    pharos_pulse_note(&s_pulse, ev->t_us);
    s_frames_seen++;
    if (!s_have_target) {
        /* No target yet, so nothing to grade - but this is exactly when the
         * operator is about to choose one, and these are the frames that name
         * the choices. */
        ward_note_network(&ev->u.dot11);
        return;
    }
    if (!frame_is_mine(&ev->u.dot11)) {
        /* Not traffic ON the guarded network - but possibly a radio wearing
         * its NAME, which is the attack aimed most precisely at this lens.
         * Counted here and never fed to the flood engine below: that engine
         * measures volume on one BSSID and a second radio's beacons would
         * corrupt exactly the number it exists to compute. */
        if (frame_wears_my_name(&ev->u.dot11)) {
            const uint8_t *b = ev->u.dot11.a3;
            bool known = false;
            for (unsigned i = 0; i < s_n_impostors; i++) {
                if (memcmp(s_impostor[i], b, 6) == 0) { known = true; break; }
            }
            if (!known && s_n_impostors < WARD_MAX_IMPOSTORS) {
                memcpy(s_impostor[s_n_impostors++], b, 6);
            }
            s_impostor_frames++;
            s_impostor_rssi = ev->u.dot11.rssi;
        }
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
    s_n_impostors = 0;
    s_impostor_frames = 0;
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
    /* Ward's OWN list first - it hears every beacon on the channels it sweeps
     * and no longer depends on another lens having been run. Census is still
     * consulted afterwards, because if it HAS run it has graded these networks
     * and may know one Ward has not heard yet on its current channel. */
    int8_t best = -127;
    pc_ap_t chosen;
    bool found = false;

    for (unsigned i = 0; i < s_n_seen; i++) {
        if (!s_seen[i].in_use || s_seen[i].rssi <= best) {
            continue;
        }
        best = s_seen[i].rssi;
        memset(&chosen, 0, sizeof(chosen));
        memcpy(chosen.bssid, s_seen[i].bssid, 6);
        const size_t n = strlen(s_seen[i].ssid);
        chosen.ssid_len = (uint8_t)(n > 32 ? 32 : n);
        memcpy(chosen.ssid, s_seen[i].ssid, chosen.ssid_len);
        chosen.channel = s_seen[i].channel;
        chosen.rssi = s_seen[i].rssi;
        found = true;
    }

    pc_ap_t ap;
    pc_verdict_t v;
    for (unsigned i = 0; pharos_lens_census_at(i, &ap, &v); i++) {
        if (ap.rssi > best) {
            best = ap.rssi;
            chosen = ap;
            found = true;
        }
    }
    if (!found) {
        /* THE ONLY ACTION THIS SCREEN OFFERS, FAILING SILENTLY.
         *
         * Adoption picks the loudest access point from the CENSUS list, which
         * is empty until Census has run. Start Ward from a cold boot and the
         * detail row says "tap centre to adopt nearest" - and the tap does
         * nothing at all, with no explanation. The operator's reasonable
         * conclusion is that the touch screen is broken.
         *
         * Say what happened instead. The condition is temporary and the fix
         * is one lens away, so the message names it. */
        s_adopt_failed = true;
        ESP_LOGW(TAG, "nothing to adopt: no named network heard yet "
                      "(give it a few seconds to sweep)");
        return;
    }
    s_adopt_failed = false;
    {
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
    o->has_history = pharos_pulse_fill(&s_pulse, (uint64_t)esp_timer_get_time(), o->history);
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
            if (s_adopt_failed) {
            snprintf(out->left, sizeof(out->left), "no network heard yet");
            snprintf(out->right, sizeof(out->right), "sweeping");
            out->tone = PHAROS_TONE_WARN;
            return true;
        }
        if (s_n_seen) {
            snprintf(out->left, sizeof(out->left), "tap centre to adopt");
            snprintf(out->right, sizeof(out->right), "%u seen",
                     s_n_seen > 99u ? 99u : s_n_seen);
            out->tone = PHAROS_TONE_NEUTRAL;
            return true;
        }
        snprintf(out->left, sizeof(out->left), "tap centre to adopt");
            snprintf(out->right, sizeof(out->right), "nearest");
            out->tone = PHAROS_TONE_DIM;
            return true;
        }
        return false;
    }

    switch (index) {
    case 0:
        /* LEAD WITH IT. A second radio announcing the guarded name is the
         * attack aimed most precisely at this lens, and it outranks any
         * volume figure below - a quiet network with an impostor on it is
         * not a quiet network. */
        snprintf(out->left, sizeof(out->left), "wearing your name");
        if (!s_have_target) {
            snprintf(out->right, sizeof(out->right), "-");
            out->tone = PHAROS_TONE_DIM;
        } else if (s_n_impostors == 0u) {
            snprintf(out->right, sizeof(out->right), "no one");
            out->tone = PHAROS_TONE_GOOD;
        } else {
            if (s_n_impostors == 1u) {
                snprintf(out->right, sizeof(out->right), "1 radio");
            } else {
                snprintf(out->right, sizeof(out->right), "%u radios",
                         s_n_impostors > 9u ? 9u : s_n_impostors);
            }
            out->tone = PHAROS_TONE_BAD;
        }
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "guarding");
        snprintf(out->right, sizeof(out->right), "%.11s",
                 ssid[0] ? ssid : "<hidden>");
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "address");
        snprintf(out->right, sizeof(out->right), "%02x:%02x:%02x", addr[3],
                 addr[4], addr[5]);
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "channel  (camped)");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)ch);
        out->tone = pharos_radio_is_camped() ? PHAROS_TONE_GOOD
                                             : PHAROS_TONE_WARN;
        return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "its frames heard");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)mine);
        out->tone = mine ? PHAROS_TONE_GOOD : PHAROS_TONE_BAD;
        return true;
    case 5:
        snprintf(out->left, sizeof(out->left), "everything else");
        snprintf(out->right, sizeof(out->right), "%u",
                 (unsigned)(seen > mine ? seen - mine : 0u));
        /* Dim on purpose: the whole point is that this number is ignored. */
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 6:
        snprintf(out->left, sizeof(out->left), "disconnects at it");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.observed);
        out->tone = v.observed ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD;
        return true;
    case 7:
        snprintf(out->left, sizeof(out->left), "confidence ceiling");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.ceiling);
        out->tone = (v.ceiling >= 80) ? PHAROS_TONE_GOOD : PHAROS_TONE_WARN;
        return true;
    case 8: {
        snprintf(out->left, sizeof(out->left), "clients knocked off");
        snprintf(out->right, sizeof(out->right), "%u",
                 (unsigned)v.distinct_victims);
        out->tone = v.distinct_victims ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
        return true;
    }
    case 9: {
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
    .purpose = "your chosen network",
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
