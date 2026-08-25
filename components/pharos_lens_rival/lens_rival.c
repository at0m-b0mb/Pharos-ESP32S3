/* Pharos lens: Rival - the other operator's hardware, announcing itself
 *
 * Plumbing only. Every judgement is in pharos_rival.c, which is pure C and
 * host-tested - including, and mostly, the list of ordinary product names it
 * must REFUSE to flag. This lens points at people in a room, so a false
 * positive here is an accusation rather than a nuisance.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_lens.h"
#include "pharos_radio.h"
#include "pharos_rival.h"
#include "pharos_survey_hook.h"

static const char *TAG = "lens.rival";

#define RIVAL_RING 512

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[RIVAL_RING];
static pharos_bus_t s_bus;
EXT_RAM_BSS_ATTR static prv_state_t s_engine;
static prv_verdict_t s_verdict;
static SemaphoreHandle_t s_lock;
static prv_band_t s_last_band;

/* ---- the raw roster --------------------------------------------------
 *
 * Every BLE advertiser seen, named or not, kept separately from the engine's
 * table of RECOGNISED hardware.
 *
 * The engine deliberately admits only devices it can classify - refusing to
 * track every passing phone is the point of it. But that makes one question
 * impossible to answer from the outside: "is my Flipper not detected because
 * the classifier is wrong, or because the device is not transmitting at all?"
 * Those need completely different fixes and the screen could not tell them
 * apart. This roster answers it. */
#define RIVAL_RAW_MAX 40
typedef struct {
    uint8_t addr[6];
    char name[PR_NAME_MAX + 1];
    int8_t rssi;
    uint16_t hits;
    uint8_t adv[31];   /* the payload itself, for eyes-on diagnosis */
    uint8_t adv_len;
    bool in_use;
} raw_adv_t;
EXT_RAM_BSS_ATTR static raw_adv_t s_raw[RIVAL_RAW_MAX];
static unsigned s_raw_n;
static uint32_t s_raw_total;

static void raw_note(const uint8_t addr[6], const char *name, int8_t rssi,
                     const uint8_t *adv, uint8_t adv_len)
{
    s_raw_total++;
    for (unsigned i = 0; i < s_raw_n; i++) {
        if (memcmp(s_raw[i].addr, addr, 6) == 0) {
            s_raw[i].hits++;
            if (rssi > s_raw[i].rssi) {
                s_raw[i].rssi = rssi;
            }
            if (s_raw[i].name[0] == '\0' && name && name[0]) {
                snprintf(s_raw[i].name, sizeof(s_raw[i].name), "%s", name);
            }
            if (s_raw[i].adv_len == 0 && adv && adv_len) {
                const uint8_t n = adv_len > 31u ? 31u : adv_len;
                memcpy(s_raw[i].adv, adv, n);
                s_raw[i].adv_len = n;
            }
            return;
        }
    }
    if (s_raw_n >= RIVAL_RAW_MAX) {
        return;
    }
    raw_adv_t *r = &s_raw[s_raw_n++];
    memset(r, 0, sizeof(*r));
    memcpy(r->addr, addr, 6);
    r->rssi = rssi;
    r->hits = 1;
    r->in_use = true;
    if (name && name[0]) {
        snprintf(r->name, sizeof(r->name), "%s", name);
    }
    if (adv && adv_len) {
        const uint8_t n = adv_len > 31u ? 31u : adv_len;
        memcpy(r->adv, adv, n);
        r->adv_len = n;
    }
}

unsigned pharos_lens_rival_raw(unsigned index, uint8_t addr[6], char *name,
                               size_t cap, int8_t *rssi, uint16_t *hits,
                               uint8_t *adv, uint8_t *adv_len)
{
    if (index >= s_raw_n) {
        return 0;
    }
    if (addr) memcpy(addr, s_raw[index].addr, 6);
    if (name && cap) snprintf(name, cap, "%s", s_raw[index].name);
    if (rssi) *rssi = s_raw[index].rssi;
    if (hits) *hits = s_raw[index].hits;
    if (adv && adv_len) {
        memcpy(adv, s_raw[index].adv, s_raw[index].adv_len);
        *adv_len = s_raw[index].adv_len;
    }
    return s_raw_n;
}

uint32_t pharos_lens_rival_raw_total(void) { return s_raw_total; }

static bool rival_mount(void)
{
    prv_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    memset(s_raw, 0, sizeof(s_raw));
    s_raw_n = 0;
    s_raw_total = 0;
    s_last_band = PRV_BAND_CLEAR;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock && pharos_bus_init(&s_bus, s_slots, RIVAL_RING);
}

static bool rival_start(void)
{
    /* Wi-Fi first: some of this hardware announces itself as an access point
     * rather than over Bluetooth, and the beacon stream is where those names
     * live. */
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    plan.want_mgmt = true;
    if (!pharos_radio_rx_start(&plan, &s_bus)) {
        return false;
    }
    if (!pharos_radio_ble_scan_start(&s_bus)) {
        /* Half of this lens is the Bluetooth half. Say so and stop, rather
         * than running a crippled scan that looks the same on the glass. */
        ESP_LOGE(TAG, "BLE observer would not start - Rival is half blind");
        pharos_radio_rx_stop();
        return false;
    }
    return true;
}

static void rival_stop(void)
{
    pharos_radio_ble_scan_stop();
    pharos_radio_rx_stop();
}

/* Analytics core. */
static void rival_event(const pharos_event_t *ev)
{
    if (!ev) {
        return;
    }
    if (ev->type == PHAROS_EV_BLE_ADV) {
        /* Pull the local name out of the advertisement, if it carries one.
         * AD structures are [len][type][payload]; 0x08 is a shortened local
         * name and 0x09 a complete one. */
        char name[PR_NAME_MAX + 1];
        name[0] = '\0';
        const uint8_t *d = ev->u.ble.data;
        const uint8_t dlen = ev->u.ble.data_len;
        uint8_t i = 0;
        while (i + 1u < dlen) {
            const uint8_t l = d[i];
            if (l == 0 || (uint16_t)i + 1u + l > (uint16_t)dlen) {
                break; /* malformed or truncated - stop rather than over-read */
            }
            const uint8_t type = d[i + 1];
            if (type == 0x08 || type == 0x09) {
                uint8_t n = (uint8_t)(l - 1);
                if (n > PR_NAME_MAX) {
                    n = PR_NAME_MAX;
                }
                memcpy(name, &d[i + 2], n);
                name[n] = '\0';
                break;
            }
            i = (uint8_t)(i + 1u + l);
        }
        raw_note(ev->u.ble.addr, name, ev->u.ble.rssi,
                 ev->u.ble.data, ev->u.ble.data_len);
        /* The RAW payload goes to the engine, not just the name. A passive
         * listener never sees the scan response, so the advertisement is the
         * only place a signature can be - and it is where the Flipper's is. */
        prv_observe_ble_adv(&s_engine, ev->u.ble.addr, name[0] ? name : NULL,
                            ev->u.ble.data, ev->u.ble.data_len,
                            ev->u.ble.rssi, ev->t_us);
        return;
    }
    if (ev->type != PHAROS_EV_DOT11) {
        return;
    }
    const pharos_ev_dot11_t *f = &ev->u.dot11;
    if (f->type != PHAROS_FT_MGMT || f->subtype != PHAROS_ST_BEACON) {
        return;
    }
    /* NOT gated on ssid_len. A Pwnagotchi advertisement is a beacon with no
     * SSID at all - dropping nameless beacons here is precisely how the first
     * version of this lens managed to be blind to the device it claimed to
     * find. The whisper flag comes from the radio, which spots the 222/224-226
     * elements in the hot path and lifts the unit's name out of the payload. */
    const bool whisper = (f->flags & PHAROS_DOT11_F_WHISPER) != 0;
    if (!whisper && f->ssid_len == 0) {
        return;
    }
    /* OPEN, from the beacon's own RSN element.
     *
     * This is the bit that separates an evil portal from a smart plug: both
     * are Espressif radios running an access point, and only the portal needs
     * to be joinable without a key. The radio already parses RSN in the hot
     * path, so it costs nothing to pass. No RSN element at all means no
     * encryption advertised - which is what open looks like on the air. */
    const bool open_network = (f->rsn_flags & PHAROS_RSN_F_PRESENT) == 0;
    prv_observe_beacon_ex(&s_engine, f->a2, f->ssid, f->ssid_len, whisper,
                          open_network, f->rssi, ev->t_us);
}

static void rival_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    prv_verdict_t v;
    prv_evaluate(&s_engine, (uint64_t)esp_timer_get_time(), &v);
    if (v.band != s_last_band) {
        ESP_LOGI(TAG, "%s score=%u devices=%u flipper=%u wifi=%u adv=%u/s \"%s\"",
                 prv_band_name(v.band), v.score, v.n_devices, v.n_flipper,
                 v.n_wifi_tools, (unsigned)v.peak_adv_per_s,
                 v.worst_name[0] ? v.worst_name : "-");
        s_last_band = v.band;
    }
    s_verdict = v;
    xSemaphoreGive(s_lock);
}

bool pharos_lens_rival_snapshot(prv_verdict_t *out)
{
    if (!out || !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    *out = s_verdict;
    xSemaphoreGive(s_lock);
    return true;
}

static struct pharos_bus *rival_ingest(void) { return &s_bus; }

static bool k_rival_display(struct pharos_lens_display *o)
{
    prv_verdict_t v;
    if (!pharos_lens_rival_snapshot(&v)) {
        return false;
    }
    snprintf(o->big, sizeof(o->big), "%u", v.n_devices);
    (void)v.n_pwnagotchi;
    snprintf(o->band, sizeof(o->band), "%s", prv_band_name(v.band));
    if (v.worst_kind != PRV_KIND_NONE && v.worst_age_s >= 2u) {
        /* GOING QUIET IS THE INTERESTING STATE. It is what switching
         * something off looks like from out here, and the face used to show
         * a signal level right up until the row vanished - so the seconds
         * between "it is here" and "it is gone" said nothing at all. Counting
         * them out loud turns a wait into a reading. */
        snprintf(o->detail, sizeof(o->detail), "%.16s quiet %us",
                 prv_kind_name(v.worst_kind), (unsigned)v.worst_age_s);
    } else if (v.worst_kind != PRV_KIND_NONE) {
        snprintf(o->detail, sizeof(o->detail), "%.20s %ddBm",
                 prv_kind_name(v.worst_kind), (int)v.worst_rssi);
    } else {
        snprintf(o->detail, sizeof(o->detail), "listening");
    }
    /* The short form: the long one does not fit the glass. */
    switch (v.band) {
    case PRV_BAND_CLEAR:
        snprintf(o->advice, sizeof(o->advice), "Nothing announced itself.");
        break;
    case PRV_BAND_NOTED:
        snprintf(o->advice, sizeof(o->advice), "Owning a tool is not an offence.");
        break;
    case PRV_BAND_CAPABLE:
        snprintf(o->advice, sizeof(o->advice), "Capable, but not being used.");
        break;
    default:
        snprintf(o->advice, sizeof(o->advice), "Something is being run.");
        break;
    }
    if (v.notes & PRV_NOTE_PAIR_SPAM) {
        /* The specific finding beats the generic one: "12 fake accessories"
         * tells an operator what is happening to the phones around them.
         *
         * Which SHAPE of spam matters too. A tool cycling payloads and a tool
         * hammering one from a new address every time look identical on a
         * score and are different things to describe to somebody. */
        if (v.pair_models >= PRV_SPAM_MODELS) {
            snprintf(o->why, sizeof(o->why), "%u fake accessories broadcasting",
                     (unsigned)v.pair_models);
        } else {
            snprintf(o->why, sizeof(o->why), "one popup, %u faked senders",
                     (unsigned)v.pair_addrs);
        }
    } else if (v.notes & PRV_NOTE_SPAM) {
        snprintf(o->why, sizeof(o->why), "advertisement flood %u/s",
                 (unsigned)v.peak_adv_per_s);
    } else if (v.worst_kind != PRV_KIND_NONE) {
        /* What the most capable thing in the room can actually DO. This used
         * to be a second row per device in the list, where it was miscounted
         * as another device; under the score it reads as what it is. */
        snprintf(o->why, sizeof(o->why), "%.47s", prv_kind_note(v.worst_kind));
    }
    o->families = v.families;
    o->fam_label[0] = "HERE";
    o->fam_label[1] = "ABLE";
    o->fam_label[2] = "INUSE";
    o->fam_label[3] = NULL;
    o->score = v.score;
    o->raw_score = v.raw_score;
    o->ceiling = 100;
    o->has_score = true;

    /* PRESENCE IS NOT AN ATTACK.
     *
     * The engine already caps a bare sighting at PRV_CAP_PRESENCE_ONLY,
     * because owning a Flipper is not an offence - but 55 read off the score
     * still lands the home ring on "something is up" for somebody sitting
     * near a colleague's toolkit. A ring that says that all day teaches its
     * operator to ignore it, which costs exactly the moment the tool is
     * actually used.
     *
     * So presence is "worth knowing" and no more. The step up requires the
     * engine to have seen the hardware BEING USED - the band it reserves for
     * address rotation, pairing spam and the rest. */
    o->has_alert = true;
    o->alert = (v.band >= PRV_BAND_ACTIVE)  ? 2u
             : (v.band >= PRV_BAND_CAPABLE) ? 1u
                                            : 0u;

    /* FEED THE SESSION SURVEY.
     *
     * The one thing a live reading genuinely cannot tell you: that a Flipper
     * was in the room twenty minutes ago and has since left. Rival is honest
     * about the present tense and drops hardware once it goes quiet - which is
     * right for a live dot and useless for "what happened while I was here".
     *
     * So every kind currently on the list is pushed as present, and the survey
     * keeps the ones that stop appearing, in the past tense. */
    {
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
        const uint64_t now = (uint64_t)esp_timer_get_time();
        prv_device_t d;
        for (unsigned i = 0; i < PR_MAX_SIGHTINGS; i++) {
            if (!prv_device_at_now(&s_engine, i, now, &d)) {
                break;
            }
            pharos_survey_tool((uint8_t)d.kind, true);
        }
    }
    }
    return true;
}

static bool k_rival_row(unsigned index, struct pharos_lens_row *out)
{
    prv_verdict_t v;
    if (!pharos_lens_rival_snapshot(&v)) {
        return false;
    }
    /* Three rows of context first - including, permanently, the two things
     * this receiver is structurally deaf to. A quiet Rival screen must never
     * be read as "there is nothing here". */
    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "advertisers/sec");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.peak_adv_per_s);
        out->tone = (v.notes & PRV_NOTE_SPAM) ? PHAROS_TONE_BAD : PHAROS_TONE_DIM;
        return true;
    case 4:
        /* Diversity, not volume - the number that separates a spammer from a
         * room full of real headphones. */
        snprintf(out->left, sizeof(out->left), "popup models / advs");
        snprintf(out->right, sizeof(out->right), "%u/%u",
                 (unsigned)v.pair_models, (unsigned)v.pair_advs);
        out->tone = (v.notes & PRV_NOTE_PAIR_SPAM) ? PHAROS_TONE_BAD
                                                   : PHAROS_TONE_DIM;
        return true;
    case 6:
        /* The other half of the spam test, and the half that catches a tool
         * repeating a single payload: real accessories keep an address for
         * minutes, these draw a fresh one per advertisement. */
        snprintf(out->left, sizeof(out->left), "faked senders");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.pair_addrs);
        out->tone = (v.pair_addrs >= PRV_SPAM_ADDRS) ? PHAROS_TONE_BAD
                                                     : PHAROS_TONE_DIM;
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "classic Bluetooth");
        snprintf(out->right, sizeof(out->right), "deaf");
        out->tone = PHAROS_TONE_WARN;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "sub-GHz / NFC / IR");
        snprintf(out->right, sizeof(out->right), "deaf");
        out->tone = PHAROS_TONE_WARN;
        return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "hardware identified");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_devices);
        out->tone = v.n_devices ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD;
        return true;
    case 5:
        /* Addresses vs hardware. With BLE randomisation these are different
         * numbers, and the gap between them is itself the finding. */
        snprintf(out->left, sizeof(out->left), "addresses used");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_addresses);
        out->tone = (v.n_addresses > v.n_devices * 4u && v.n_devices)
                        ? PHAROS_TONE_BAD : PHAROS_TONE_DIM;
        return true;
    default:
        break;
    }

    /* Then the hardware itself: ONE ROW PER DEVICE.
     *
     * This used to print two rows each - the device, then an indented line
     * explaining what that hardware can do - and the operator counted two
     * Flippers in a room containing one. They were right to: on a list where
     * every line is a device, a line that is not a device is a bug, and no
     * amount of indentation fixes it. The count row above said 1 and the list
     * below said otherwise, which is exactly the kind of small contradiction
     * that costs a tool its credibility.
     *
     * The capability note now rides on the live face instead, under the score,
     * where it is context about a finding rather than an item in a list. */
    const unsigned k = index - 7u;
    prv_device_t d;
    /* Stale-aware, so the list cannot show hardware the count above it has
     * already dropped - a Flipper that has been switched off must stop being
     * reported, and within half a minute it does. */
    if (!prv_device_at_now(&s_engine, k, (uint64_t)esp_timer_get_time(), &d)) {
        return false;
    }
    /* Prefer the device's own name over its address: "Flipper R3ghon" is
     * something an operator can ask a room about, and two hex bytes are not.
     * Falls back to the address when nothing named itself, which for a passive
     * listener is most of the time. */
    if (d.name[0]) {
        snprintf(out->left, sizeof(out->left), "%.12s %.11s",
                 prv_kind_name(d.kind), d.name);
    } else {
        snprintf(out->left, sizeof(out->left), "%.15s %02x:%02x",
                 prv_kind_name(d.kind), d.addr[4], d.addr[5]);
    }
    /* A ROW THAT IS ON ITS WAY OUT SHOULD LOOK LIKE ONE.
     *
     * Silence is the interesting state here - it is how switching something
     * off looks from the outside - and the list showed nothing at all until
     * the entry vanished. So an entry that has missed its usual cadence says
     * how long since it was last heard, and goes dim while it says it. The
     * wait then reads as a countdown rather than as the screen being wrong,
     * which is the whole difference between "it took some time to remove the
     * Flipper" and "I watched it let go". */
    const uint64_t now = (uint64_t)esp_timer_get_time();
    const uint64_t quiet_us = (now > d.last_us) ? (now - d.last_us) : 0ull;
    const bool fading = quiet_us > (prv_expiry_us(&d) / 3ull);

    if (fading) {
        snprintf(out->right, sizeof(out->right), "%us ago",
                 (unsigned)(quiet_us / 1000000ull));
    } else if (d.addresses > 3u) {
        /* Address rotation displaces the signal once it starts: hardware
         * using dozens of addresses a minute is hardware doing something. */
        snprintf(out->right, sizeof(out->right), "%u addr",
                 (unsigned)d.addresses);
    } else {
        snprintf(out->right, sizeof(out->right), "%d dBm", (int)d.best_rssi);
    }
    out->tone = fading                          ? PHAROS_TONE_DIM
              : (d.kind >= PRV_KIND_DEAUTHER)   ? PHAROS_TONE_BAD
                                                : PHAROS_TONE_NEUTRAL;
    return true;
}

static const pharos_lens_t k_rival = {
    .id = "rf.rival",
    .purpose = "hacking hardware",
    .name = "Rival",
    .summary = "Finds the other operator's hardware announcing itself",
    .glyph = "crosshair",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_BLE_SCAN,
    .budget_ma = 150,
    .on_mount = rival_mount,
    .on_start = rival_start,
    .on_stop = rival_stop,
    .on_tick = rival_tick,
    .on_event = rival_event,
    .ingest = rival_ingest,
    .display = k_rival_display,
    .row = k_rival_row,
    .row_head_left = "HARDWARE IN RANGE",
    .row_head_right = "SIGNAL",
};

PHAROS_LENS_REGISTER(&k_rival);
