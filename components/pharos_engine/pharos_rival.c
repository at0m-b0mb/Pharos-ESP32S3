/* Pharos - Rival. See pharos_rival.h for what this lens refuses to claim. */
#include "pharos_rival.h"

#include <stdio.h>
#include <string.h>

/* ---- helpers --------------------------------------------------------- */

static bool ci_starts(const char *hay, uint8_t hlen, const char *needle)
{
    unsigned i = 0;
    for (; needle[i]; i++) {
        if (i >= hlen) {
            return false;
        }
        char a = hay[i];
        char b = needle[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool ci_contains(const char *hay, uint8_t hlen, const char *needle)
{
    const unsigned nl = (unsigned)strlen(needle);
    if (nl == 0 || hlen < nl) {
        return false;
    }
    for (unsigned i = 0; i + nl <= hlen; i++) {
        if (ci_starts(&hay[i], (uint8_t)(hlen - i), needle)) {
            return true;
        }
    }
    return false;
}

/* ---- classification --------------------------------------------------
 *
 * Names only. This is deliberately a NAME test rather than an address-prefix
 * one: OUI prefixes for this hardware are widely cloned and randomised, and a
 * false positive here is worse than a miss - accusing somebody's headphones of
 * being an attack tool is how an operator learns to stop reading the screen.
 *
 * Every entry below is something that announces itself in plain words. If it
 * has been renamed, Rival will not see it, and that limitation is honest: this
 * lens finds hardware that is not hiding. */
prv_kind_t prv_classify_name(const char *name, uint8_t len, bool ble)
{
    if (!name || len == 0) {
        return PRV_KIND_NONE;
    }

    /* A Flipper Zero ships advertising "Flipper <name>" over Bluetooth. It is
     * the most capable piece of hobbyist RF hardware most defenders will ever
     * meet - and note that Bluetooth is the ONLY part of it this device can
     * hear. Its Sub-GHz, NFC and infrared radios are all invisible here. */
    if (ble && ci_starts(name, len, "flipper")) {
        return PRV_KIND_FLIPPER;
    }

    /* Boards whose NAME announces the attack. These are firmware defaults from
     * well-known deauthentication projects; somebody running one has gone out
     * of their way to install it. */
    /* "pwned" is the ESP8266 deauther's default access-point name, straight
     * from its own documentation; the rest are firmware names people install
     * deliberately. All matched as CONTAINS because forks append serials. */
    static const char *k_deauth[] = { "pwned", "deauth", "marauder", "evilportal" };
    for (unsigned i = 0; i < sizeof(k_deauth) / sizeof(k_deauth[0]); i++) {
        if (ci_contains(name, len, k_deauth[i])) {
            return PRV_KIND_DEAUTHER;
        }
    }

    /* A Pwnagotchi announces itself in a Wi-Fi beacon while it collects
     * handshakes - it is talking to other Pwnagotchi, not to you. */
    if (ci_contains(name, len, "pwnagotchi")) {
        return PRV_KIND_PWNAGOTCHI;
    }

    /* Rogue-AP appliances shipped with a default network name. */
    if (ci_starts(name, len, "pineapple") || ci_contains(name, len, "hak5")) {
        return PRV_KIND_PINEAPPLE;
    }

    /* Bare BLE serial bridges: a radio bolted to a UART. Reported as what they
     * are and never as a "skimmer" - the identical module is inside hobby
     * electronics, scoreboards and door locks, and the classic-Bluetooth
     * modules documented in card skimmers are invisible to this radio anyway.
     * See PRV_NOTE_BREDR_BLIND. */
    static const char *k_bridge[] = { "hc-05", "hc-06", "hc-08", "jdy-",
                                      "at-09", "mlt-bt05", "hm-10", "bt05" };
    for (unsigned i = 0; i < sizeof(k_bridge) / sizeof(k_bridge[0]); i++) {
        if (ci_starts(name, len, k_bridge[i])) {
            return PRV_KIND_SERIAL_BRIDGE;
        }
    }

    /* Development boards left on their factory advertising name. Weak
     * evidence of anything - an ESP32 is in a thousand innocent products -
     * so it sits at the bottom of the capability order and is scored as
     * presence only. */
    static const char *k_dev[] = { "esp32", "esp-", "nodemcu", "m5stack",
                                   "wemos", "nrf connect" };
    for (unsigned i = 0; i < sizeof(k_dev) / sizeof(k_dev[0]); i++) {
        if (ci_starts(name, len, k_dev[i])) {
            return PRV_KIND_DEV_BOARD;
        }
    }
    return PRV_KIND_NONE;
}

/* THE FLIPPER SIGNATURE, TAKEN OFF THE AIR RATHER THAN OFF A FORUM.
 *
 * A Flipper Zero advertises a 16-BIT service UUID in the 0x3081..0x3083 range,
 * the low byte selecting the shell colour. Captured from a real unit sitting on
 * a desk:
 *
 *   0201 06  07 09 52 33 67 68 6f 6e  03 02 82 30  02 0a 00
 *   flags    complete name "R3ghon"   ^^^^^^^^^^^  tx power
 *                                     len 3, AD type 0x02
 *                                     (16-bit service UUID list), UUID 0x3082
 *
 * Two things that capture settled, both of which I had wrong:
 *
 *   - The pair lives in a SIXTEEN-bit UUID list (AD type 0x02/0x03). I had
 *     required it inside a 128-bit list (0x06/0x07) and would have matched
 *     nothing, ever.
 *   - The name is right there in the advertisement - and it is "R3ghon",
 *     because people rename these. Matching on a name beginning "Flipper" is
 *     therefore useless on any unit whose owner has touched the settings, which
 *     is most of them.
 *
 * Matching a whole UUID rather than scanning the payload for two loose bytes is
 * deliberate: 0x8N 0x30 would collide by chance across a room of advertisers,
 * and every false positive on this lens is an accusation pointed at a person.
 */
static bool flipper_uuid(uint16_t uuid, const char **colour)
{
    switch (uuid) {
    case 0x3081: if (colour) *colour = "black";       return true;
    case 0x3082: if (colour) *colour = "white";       return true;
    case 0x3083: if (colour) *colour = "transparent"; return true;
    default: return false;
    }
}

/* Pull the pairing "model" or "action" code out of an advertisement, if it is
 * one of the popup-triggering kinds. Returns false when it is not.
 *
 * Apple: company 0x004C, then a type byte. 0x0F is Nearby Action and 0x07 is
 * Proximity Pairing - the two a phone will raise a dialog for. The byte after
 * the length is the action or model, and that is the one that varies wildly
 * under spam and not at all under normal use.
 *
 * Google: service data for 0xFE2C carries a 3-byte model ID; the low byte is
 * enough to measure diversity. */
static bool pairing_code(const uint8_t *data, uint8_t len, uint8_t *code)
{
    if (!data || len < 4) {
        return false;
    }
    uint8_t i = 0;
    while (i + 1u < len) {
        const uint8_t l = data[i];
        if (l == 0 || (uint16_t)i + 1u + l > (uint16_t)len) {
            break;
        }
        const uint8_t type = data[i + 1];
        const uint8_t *p = &data[i + 2];
        const uint8_t plen = (uint8_t)(l - 1);

        if (type == 0xFF && plen >= 5) {
            const uint16_t company = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
            if (company == 0x004C && (p[2] == 0x0F || p[2] == 0x07)) {
                *code = p[4];
                return true;
            }
            /* Microsoft Swift Pair. */
            if (company == 0x0006 && plen >= 6) {
                *code = p[5];
                return true;
            }
        }
        if (type == 0x16 && plen >= 5) {
            const uint16_t uuid = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
            if (uuid == 0xFE2C) { /* Google Fast Pair */
                *code = p[4];
                return true;
            }
        }
        i = (uint8_t)(i + 1u + l);
    }
    return false;
}

prv_kind_t prv_classify_adv(const uint8_t *data, uint8_t len,
                            const char **colour)
{
    if (colour) {
        *colour = "";
    }
    if (!data || len < 3) {
        return PRV_KIND_NONE;
    }
    uint8_t i = 0;
    while (i + 1u < len) {
        const uint8_t l = data[i];
        if (l == 0 || (uint16_t)i + 1u + l > (uint16_t)len) {
            break; /* malformed or truncated - stop rather than over-read */
        }
        const uint8_t type = data[i + 1];
        const uint8_t *p = &data[i + 2];
        const uint8_t plen = (uint8_t)(l - 1);

        /* 0x02 / 0x03: incomplete and complete lists of 16-bit service UUIDs,
         * little-endian, two bytes each. This is where a Flipper's is. */
        if ((type == 0x02 || type == 0x03) && plen >= 2) {
            for (uint8_t k = 0; k + 1u < plen; k += 2u) {
                const uint16_t uuid = (uint16_t)(p[k] | ((uint16_t)p[k + 1] << 8));
                if (flipper_uuid(uuid, colour)) {
                    return PRV_KIND_FLIPPER;
                }
            }
        }
        /* A local name in the advertisement is still worth reading when a
         * device volunteers one - though a renamed Flipper will not match. */
        if ((type == 0x08 || type == 0x09) && plen >= 3) {
            char nm[PR_NAME_MAX + 1];
            uint8_t n = plen > PR_NAME_MAX ? PR_NAME_MAX : plen;
            memcpy(nm, p, n);
            nm[n] = 0;
            const prv_kind_t k = prv_classify_name(nm, n, true);
            if (k != PRV_KIND_NONE) {
                return k;
            }
        }
        i = (uint8_t)(i + 1u + l);
    }
    return PRV_KIND_NONE;
}

/* ---- ingest ---------------------------------------------------------- */

void prv_reset(prv_state_t *s)
{
    if (s) {
        memset(s, 0, sizeof(*s));
    }
}

static prv_device_t *admit(prv_state_t *s, const uint8_t addr[6], uint64_t t_us)
{
    for (unsigned i = 0; i < s->n; i++) {
        if (s->dev[i].in_use && memcmp(s->dev[i].addr, addr, 6) == 0) {
            return &s->dev[i];
        }
    }
    if (s->n >= PR_MAX_SIGHTINGS) {
        s->full = true;
        return NULL;
    }
    prv_device_t *d = &s->dev[s->n++];
    memset(d, 0, sizeof(*d));
    memcpy(d->addr, addr, 6);
    d->in_use = true;
    d->first_us = t_us;
    d->best_rssi = -128;
    return d;
}

static void note_time(prv_state_t *s, uint64_t t_us)
{
    if (s->first_us == 0) {
        s->first_us = t_us;
    }
    s->last_us = t_us;
}

void prv_observe_ble_adv(prv_state_t *s, const uint8_t addr[6], const char *name,
                         const uint8_t *adv, uint8_t adv_len, int8_t rssi,
                         uint64_t t_us)
{
    if (!s || !addr) {
        return;
    }
    note_time(s, t_us);

    /* Advertisement-rate bookkeeping, for the spam test. Counted per wall
     * second across ALL advertisers, because BLE spam is characterised by a
     * flood of distinct short-lived addresses rather than by one loud one. */
    const uint32_t sec = (uint32_t)(t_us / 1000000ull);
    if (sec != s->adv_sec) {
        s->adv_sec = sec;
        s->adv_slot = (uint8_t)((s->adv_slot + 1u) % 8u);
        s->adv_distinct[s->adv_slot] = 0;
    }
    s->adv_distinct[s->adv_slot]++;

    /* Pairing-popup spam. The window slides rather than resetting on a timer
     * so a burst straddling a boundary is not split into two harmless halves. */
    if (s->pair_window_us == 0 || t_us < s->pair_window_us ||
        t_us - s->pair_window_us > PRV_SPAM_WINDOW_US) {
        s->pair_window_us = t_us;
        s->pair_n_models = 0;
        s->pair_advs = 0;
    }
    uint8_t code = 0;
    if (pairing_code(adv, adv_len, &code)) {
        s->pair_advs++;
        bool seen = false;
        for (uint8_t k = 0; k < s->pair_n_models; k++) {
            if (s->pair_models[k] == code) {
                seen = true;
                break;
            }
        }
        if (!seen && s->pair_n_models < (uint8_t)sizeof(s->pair_models)) {
            s->pair_models[s->pair_n_models++] = code;
        }
    }

    /* The raw advertisement first: it is the only thing a passive listener is
     * guaranteed to see, and it is where the Flipper signature lives. The name
     * is a bonus when a device volunteers one in the advertisement itself. */
    const char *colour = "";
    prv_kind_t kind = prv_classify_adv(adv, adv_len, &colour);
    if (kind == PRV_KIND_NONE) {
        const uint8_t nlen = name ? (uint8_t)strlen(name) : 0u;
        kind = prv_classify_name(name, nlen, true);
    }
    if (kind == PRV_KIND_NONE) {
        return; /* an ordinary Bluetooth device is not this lens' business */
    }
    prv_device_t *d = admit(s, addr, t_us);
    if (!d) {
        return;
    }
    d->ble = true;
    d->kind = kind;
    d->last_us = t_us;
    d->sightings++;
    if (rssi > d->best_rssi) {
        d->best_rssi = rssi;
    }
    if (d->name[0] == '\0') {
        if (name && name[0]) {
            snprintf(d->name, sizeof(d->name), "%s", name);
        } else if (colour && colour[0]) {
            /* No name to be had passively, but the UUID told us the shell
             * colour - which is a far more useful label to hand somebody
             * looking around a room than a random address. */
            snprintf(d->name, sizeof(d->name), "%s", colour);
        }
    }
}

void prv_observe_ble(prv_state_t *s, const uint8_t addr[6], const char *name,
                     int8_t rssi, uint64_t t_us)
{
    prv_observe_ble_adv(s, addr, name, NULL, 0, rssi, t_us);
}

/* The address a Pwnagotchi transmits its advertisement from. Hardcoded in the
 * project and unchanged across the common forks - but checked as ONE of two
 * signals, never as the only one, because a constant in somebody else's source
 * is exactly the kind of thing that changes without warning. */
bool prv_is_pwnagotchi_addr(const uint8_t addr[6])
{
    static const uint8_t k_pwn[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD };
    return addr && memcmp(addr, k_pwn, 6) == 0;
}

void prv_observe_beacon(prv_state_t *s, const uint8_t bssid[6], const char *ssid,
                        uint8_t ssid_len, bool whisper, int8_t rssi,
                        uint64_t t_us)
{
    if (!s || !bssid) {
        return;
    }
    note_time(s, t_us);

    prv_kind_t kind = PRV_KIND_NONE;
    /* THE PWNAGOTCHI TEST, AND WHY IT IS NOT A NAME MATCH.
     *
     * A Pwnagotchi advertisement is a beacon with NO SSID carrying a chunked
     * JSON payload in information elements 222 and 224-226, sent from
     * de:ad:be:ef:de:ad. Matching on the word "pwnagotchi" in an SSID - which
     * is what this lens did first - could never have worked, because there is
     * no SSID in the frame at all. Either signal is sufficient; the radio
     * lifts the unit's own name out of the payload so it can be shown. */
    if (whisper || prv_is_pwnagotchi_addr(bssid)) {
        kind = PRV_KIND_PWNAGOTCHI;
    } else if (ssid && ssid_len) {
        kind = prv_classify_name(ssid, ssid_len, false);
    }
    if (kind == PRV_KIND_NONE) {
        return;
    }
    prv_device_t *d = admit(s, bssid, t_us);
    if (!d) {
        return;
    }
    d->ble = false;
    d->kind = kind;
    d->last_us = t_us;
    d->sightings++;
    if (rssi > d->best_rssi) {
        d->best_rssi = rssi;
    }
    if (d->name[0] == '\0' && ssid && ssid_len) {
        const uint8_t n = ssid_len > PR_NAME_MAX ? PR_NAME_MAX : ssid_len;
        memcpy(d->name, ssid, n);
        d->name[n] = '\0';
    }
}

void prv_observe_ssid(prv_state_t *s, const uint8_t bssid[6], const char *ssid,
                      uint8_t ssid_len, int8_t rssi, uint64_t t_us)
{
    prv_observe_beacon(s, bssid, ssid, ssid_len, false, rssi, t_us);
}

/* ---- scoring --------------------------------------------------------- */

static uint8_t kind_weight(prv_kind_t k)
{
    switch (k) {
    case PRV_KIND_FLIPPER:       return 40;
    case PRV_KIND_PINEAPPLE:     return 38;
    case PRV_KIND_PWNAGOTCHI:    return 36;
    case PRV_KIND_DEAUTHER:      return 34;
    case PRV_KIND_SERIAL_BRIDGE: return 18;
    case PRV_KIND_DEV_BOARD:     return 12;
    default:                     return 0;
    }
}

void prv_evaluate(const prv_state_t *s, uint64_t now_us, prv_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    /* Two things this receiver is structurally deaf to, stated on every single
     * verdict rather than in a footnote. */
    out->notes = PRV_NOTE_BREDR_BLIND | PRV_NOTE_SUBGHZ_BLIND;
    out->band = PRV_BAND_CLEAR;
    out->headline = "Nothing announced itself to this receiver.";
    if (!s) {
        return;
    }
    (void)now_us;
    if (s->full) {
        out->notes |= PRV_NOTE_FULL;
    }

    uint8_t best = 0;
    for (unsigned i = 0; i < s->n; i++) {
        const prv_device_t *d = &s->dev[i];
        if (!d->in_use) {
            continue;
        }
        out->n_devices++;
        if (d->kind == PRV_KIND_FLIPPER) {
            out->n_flipper++;
        }
        if (d->kind == PRV_KIND_PWNAGOTCHI) {
            out->n_pwnagotchi++;
        }
        if (!d->ble) {
            out->n_wifi_tools++;
        }
        const uint8_t w = kind_weight(d->kind);
        if (w > best) {
            best = w;
            out->worst_kind = d->kind;
            memcpy(out->worst_addr, d->addr, 6);
            snprintf(out->worst_name, sizeof(out->worst_name), "%s", d->name);
            out->worst_rssi = d->best_rssi;
        }
    }

    /* The advertisement flood. A Flipper running BLE spam produces a burst of
     * distinct advertisers no ordinary room does; a busy cafe sits far below
     * this. Taken as the peak second so a short burst is not averaged away. */
    for (unsigned i = 0; i < 8; i++) {
        if (s->adv_distinct[i] > out->peak_adv_per_s) {
            out->peak_adv_per_s = s->adv_distinct[i];
        }
    }
    out->pair_models = s->pair_n_models;
    out->pair_advs = s->pair_advs;

    const bool pair_spam = (out->pair_models >= PRV_SPAM_MODELS &&
                            out->pair_advs >= PRV_SPAM_ADVS);

    if (out->n_devices == 0 && out->peak_adv_per_s < 60u && !pair_spam) {
        return;
    }

    uint32_t score = best;
    if (out->n_devices > 1) {
        score += 6u * (out->n_devices > 3u ? 3u : (out->n_devices - 1u));
    }
    if (best) {
        out->families |= PRV_FAM_PRESENT;
    }
    if (best >= 34u) {
        out->families |= PRV_FAM_CAPABLE;
    }

    /* ACTIVE is the only family that justifies raising a voice, because it is
     * the only one that is about something being DONE. */
    if (pair_spam) {
        /* The specific one, and the better evidence: a room does not contain
         * six different models of headphone all announcing themselves twenty
         * times in four seconds. */
        out->notes |= PRV_NOTE_PAIR_SPAM;
        out->families |= PRV_FAM_ACTIVE;
        if (score < 70u) {
            score = 70u;
        }
    }
    if (out->peak_adv_per_s >= 60u) {
        out->notes |= PRV_NOTE_SPAM;
        out->families |= PRV_FAM_ACTIVE;
        score += 30u;
        /* AN ADVERTISEMENT FLOOD IS THE BAND, NOT A CONTRIBUTION TO IT.
         *
         * The first version simply added points, and that was wrong for the
         * exact case this family exists to catch: spam advertisers carry junk
         * names, so nothing is IDENTIFIED, so `best` is zero and a genuine
         * flood scored 30 - the same as noticing a dev board on a desk.
         *
         * ACTIVE means something is being run. That is the finding, whether
         * or not the source could be named, so it sets the floor of its own
         * band rather than nudging a total that had nothing else in it. */
        if (score < 60u) {
            score = 60u;
        }
    }

    if (score > 100u) {
        score = 100u;
    }
    out->raw_score = (uint8_t)score;

    /* PRESENCE ALONE IS CAPPED. Owning a tool is not an offence, and a lens
     * that alarms at the sight of one teaches its operator to ignore it. */
    if (!(out->families & PRV_FAM_ACTIVE) && score > PRV_CAP_PRESENCE_ONLY) {
        score = PRV_CAP_PRESENCE_ONLY;
    }
    out->score = (uint8_t)score;

    if (out->score >= 60)      out->band = PRV_BAND_ACTIVE;
    else if (out->score >= 40) out->band = PRV_BAND_CAPABLE;
    else if (out->score >= 20) out->band = PRV_BAND_NOTED;
    else                       out->band = PRV_BAND_CLEAR;

    out->headline = prv_band_advice(out->band);
}

bool prv_device_at(const prv_state_t *s, unsigned index, prv_device_t *out)
{
    if (!s || !out) {
        return false;
    }
    /* Most capable first, then strongest, then table order for stability. */
    for (unsigned i = 0; i < s->n; i++) {
        if (!s->dev[i].in_use) {
            continue;
        }
        unsigned above = 0;
        for (unsigned j = 0; j < s->n; j++) {
            if (j == i || !s->dev[j].in_use) {
                continue;
            }
            const uint8_t wi = kind_weight(s->dev[i].kind);
            const uint8_t wj = kind_weight(s->dev[j].kind);
            if (wj > wi || (wj == wi && s->dev[j].best_rssi > s->dev[i].best_rssi) ||
                (wj == wi && s->dev[j].best_rssi == s->dev[i].best_rssi && j < i)) {
                above++;
            }
        }
        if (above == index) {
            *out = s->dev[i];
            return true;
        }
    }
    return false;
}

/* ---- words ----------------------------------------------------------- */

const char *prv_kind_name(prv_kind_t k)
{
    switch (k) {
    case PRV_KIND_FLIPPER:       return "Flipper Zero";
    case PRV_KIND_PINEAPPLE:     return "rogue-AP box";
    case PRV_KIND_PWNAGOTCHI:    return "Pwnagotchi";
    case PRV_KIND_DEAUTHER:      return "deauther board";
    case PRV_KIND_SERIAL_BRIDGE: return "BLE serial bridge";
    case PRV_KIND_DEV_BOARD:     return "dev board";
    case PRV_KIND_NONE:
    default:                     return "--";
    }
}

const char *prv_kind_note(prv_kind_t k)
{
    switch (k) {
    case PRV_KIND_FLIPPER:
        return "Sub-GHz, NFC, IR - none of which this can hear";
    case PRV_KIND_PINEAPPLE:
        return "Purpose-built rogue access point";
    case PRV_KIND_PWNAGOTCHI:
        return "Collects handshakes to crack later";
    case PRV_KIND_DEAUTHER:
        return "Firmware named for the attack it performs";
    case PRV_KIND_SERIAL_BRIDGE:
        return "A radio on a serial port. Also in doorbells";
    case PRV_KIND_DEV_BOARD:
        return "A dev board. In a thousand innocent products";
    case PRV_KIND_NONE:
    default:
        return "";
    }
}

const char *prv_band_name(prv_band_t b)
{
    switch (b) {
    case PRV_BAND_CLEAR:   return "CLEAR";
    case PRV_BAND_NOTED:   return "NOTED";
    case PRV_BAND_CAPABLE: return "CAPABLE";
    case PRV_BAND_ACTIVE:  return "IN USE";
    default:               return "?";
    }
}

const char *prv_band_advice(prv_band_t b)
{
    switch (b) {
    case PRV_BAND_CLEAR:
        return "Nothing announced itself to this receiver.";
    case PRV_BAND_NOTED:
        return "Ordinary hardware in view. Owning a tool is not an offence.";
    case PRV_BAND_CAPABLE:
        return "Capable hardware is present. Nothing says it is being used.";
    case PRV_BAND_ACTIVE:
        return "Advertisement flooding in progress. Something is being run.";
    default:
        return "";
    }
}
