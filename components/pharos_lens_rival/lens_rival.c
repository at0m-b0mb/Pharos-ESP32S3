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
    prv_observe_beacon(&s_engine, f->a2, f->ssid, f->ssid_len, whisper, f->rssi,
                       ev->t_us);
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
    if (v.worst_kind != PRV_KIND_NONE) {
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
    if (v.notes & PRV_NOTE_SPAM) {
        snprintf(o->why, sizeof(o->why), "advertisement flood %u/s",
                 (unsigned)v.peak_adv_per_s);
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
    default:
        break;
    }

    /* Then the hardware itself, most capable first. Two rows each: what it is
     * and how loud, then what that hardware can actually DO - because "Flipper
     * Zero" means nothing to somebody who has not met one. */
    const unsigned k = index - 3u;
    prv_device_t d;
    if (!prv_device_at(&s_engine, k / 2u, &d)) {
        return false;
    }
    if ((k & 1u) == 0u) {
        /* Prefer the device's own name over its address: "Flipper R3ghon" is
         * something an operator can ask a room about, and two hex bytes are
         * not. Falls back to the address when nothing named itself, which for
         * a passive listener is most of the time. */
        if (d.name[0]) {
            snprintf(out->left, sizeof(out->left), "%.12s %.11s",
                     prv_kind_name(d.kind), d.name);
        } else {
            snprintf(out->left, sizeof(out->left), "%.15s %02x:%02x",
                     prv_kind_name(d.kind), d.addr[4], d.addr[5]);
        }
        snprintf(out->right, sizeof(out->right), "%d dBm", (int)d.best_rssi);
        out->tone = (d.kind >= PRV_KIND_DEAUTHER) ? PHAROS_TONE_BAD
                                                  : PHAROS_TONE_NEUTRAL;
    } else {
        snprintf(out->left, sizeof(out->left), "  %.23s", prv_kind_note(d.kind));
        snprintf(out->right, sizeof(out->right), "%s", d.ble ? "BLE" : "wifi");
        out->tone = PHAROS_TONE_DIM;
    }
    return true;
}

static const pharos_lens_t k_rival = {
    .id = "rf.rival",
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
