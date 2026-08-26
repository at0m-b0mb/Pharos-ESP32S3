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

    /* Rogue-AP appliances shipped with a default network name.
     *
     * The management network is the one the operator forgets. A Pineapple
     * brought up out of the box serves "Pineapple_XXXX" (Mark IV/V/VI/VII) or
     * "MK7_XXXX" alongside whatever it is impersonating, and the recovery AP
     * keeps the default whatever else is renamed. CONTAINS rather than starts,
     * because the serial suffix is sometimes a prefix on the clones. */
    static const char *k_rogue_ap[] = { "pineapple", "hak5", "mk7_", "mk7-",
                                        "wifipineapple" };
    for (unsigned i = 0; i < sizeof(k_rogue_ap) / sizeof(k_rogue_ap[0]); i++) {
        if (ci_contains(name, len, k_rogue_ap[i])) {
            return PRV_KIND_PINEAPPLE;
        }
    }

    /* Bare BLE serial bridges: a radio bolted to a UART. Reported as what they
     * are and never as a "skimmer" - the identical module is inside hobby
     * electronics, scoreboards and door locks, and the classic-Bluetooth
     * modules documented in card skimmers are invisible to this radio anyway.
     * See PRV_NOTE_BREDR_BLIND. */
    static const char *k_bridge[] = { "hc-05", "hc-06", "hc-08", "jdy-",
                                      "at-09", "mlt-bt05", "hm-10", "bt05",
                                      "cc41", "sh-hc-08", "rn487", "bolutek",
                                      "free2move", "linvor" };
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
 * Apple's other Continuity types are deliberately NOT here. Nearby Info (0x10)
 * and offline-finding (0x12) are broadcast continuously by every iPhone and
 * every AirTag in the building; counting them would make a busy foyer look
 * like an attack, and a detector that cries wolf in a foyer is one nobody
 * reads in a corridor.
 *
 * Google: service data for 0xFE2C carries a 3-byte model ID; the low byte is
 * enough to measure diversity.
 *
 * Microsoft Swift Pair: company 0x0006 with beacon ID 0x03. The beacon ID is
 * checked because 0x0006 alone is every Microsoft-adjacent device in range
 * announcing something unrelated, and folding those in would inflate the
 * diversity count with ordinary traffic.
 *
 * Samsung: company 0x0075. The EasySetup advertisement behind the Buds and
 * Watch dialogs, and the family the popular spam tools reach for on Android
 * hardware - the one this engine could not see at all. */
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
            /* Microsoft Swift Pair: beacon ID 0x03, not merely company
             * 0x0006 - see above. */
            if (company == 0x0006 && plen >= 6 && p[2] == 0x03) {
                *code = p[5];
                return true;
            }
            /* Samsung EasySetup. */
            if (company == 0x0075 && plen >= 5) {
                *code = p[4];
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
                /* THE SERIAL PROFILE, WHICH SURVIVES A RENAME.
                 *
                 * FFE0 (with its FFE1 characteristic) is the transparent-UART
                 * service that HM-10, CC41-A, AT-09, JDY and MLT-BT05 clones
                 * all expose; FFF0 is the same idea on the other common clone
                 * family. Matching it catches a module whose advertising name
                 * has been changed - which is a one-line AT command, and the
                 * first thing anybody deploying one quietly would do.
                 *
                 * Still only SERIAL_BRIDGE. The identical part is inside
                 * hobby electronics, scoreboards and door locks, and calling
                 * it a skimmer on this evidence would be inventing intent. */
                if (uuid == 0xFFE0u || uuid == 0xFFF0u) {
                    return PRV_KIND_SERIAL_BRIDGE;
                }
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

static prv_device_t *admit(prv_state_t *s, const uint8_t addr[6],
                           prv_kind_t kind, int8_t rssi, bool ble,
                           uint64_t t_us)
{
    /* A DEVICE IS A KIND, NOT AN ADDRESS - see the long note in the header.
     *
     * A Flipper running pairing spam broadcasts from a fresh random address
     * for every advertisement, so a table keyed on address showed twenty-nine
     * Flippers in a room containing one. Over Bluetooth the address is not an
     * identity, so it is not used as one; `addresses` counts the rotation
     * instead, which is a finding rather than a defect.
     *
     * Wi-Fi is different: BSSIDs do not rotate, two deauther boards really are
     * two, and keying on the address there is both safe and more precise. */
    if (ble) {
        for (unsigned i = 0; i < s->n; i++) {
            prv_device_t *d = &s->dev[i];
            if (!d->in_use || !d->ble || d->kind != kind) {
                continue;
            }
            if (memcmp(d->addr, addr, 6) != 0) {
                if (d->addresses < 0xFFFFu) {
                    d->addresses++;
                }
                memcpy(d->addr, addr, 6); /* the one it is using now */
            }
            return d;
        }
    } else {
        for (unsigned i = 0; i < s->n; i++) {
            if (s->dev[i].in_use && !s->dev[i].ble &&
                memcmp(s->dev[i].addr, addr, 6) == 0) {
                return &s->dev[i];
            }
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
    d->last_us = t_us;
    d->last_listen_us = s->listen_us;
    d->best_rssi = rssi;
    d->addresses = 1;
    return d;
}

/* A name is only worth keeping if it is readable. Spam payloads and truncated
 * elements produce byte soup, and "Flipper \xe2\x96" on a list of hardware in
 * the room is worse than no name at all. */
static bool name_is_sane(const char *n)
{
    if (!n || !n[0]) {
        return false;
    }
    for (unsigned i = 0; n[i]; i++) {
        const unsigned char c = (unsigned char)n[i];
        if (c < 0x20u || c > 0x7Eu) {
            return false;
        }
    }
    return true;
}

static void note_time(prv_state_t *s, uint64_t t_us)
{
    if (s->first_us == 0) {
        s->first_us = t_us;
    }
    s->last_us = t_us;
}

/* FNV-1a. Both spam tests answer only "how many DIFFERENT ones", never
 * "which", so addresses and names are stored as hashes - keeping the values
 * would be retaining identifying material for no purpose. Zero is reserved to
 * mark an empty slot. */
static uint32_t fnv_bytes(const uint8_t *p, unsigned n)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static uint32_t addr_hash(const uint8_t addr[6])
{
    return fnv_bytes(addr, 6);
}

static uint32_t str_hash(const char *s)
{
    unsigned n = 0;
    while (s[n] && n < 64u) n++;
    return fnv_bytes((const uint8_t *)s, n);
}

/* ---- payload-independent spam: one radio wearing many addresses -------
 *
 * See the long note in pharos_rival.h. The short version: an attacker writes
 * the address, the world writes the signal level, so a flood of addresses
 * from one transmitter arrives in a tight level band and a room full of real
 * accessories does not.
 */
static void cohere_expire(prv_state_t *s, uint64_t now_us)
{
    unsigned w = 0;
    for (unsigned i = 0; i < s->coh_n; i++) {
        if (now_us - s->coh_us[i] < PRV_COHERE_WINDOW_US) {
            s->coh_addr_h[w] = s->coh_addr_h[i];
            s->coh_rssi[w] = s->coh_rssi[i];
            s->coh_us[w] = s->coh_us[i];
            w++;
        }
    }
    s->coh_n = (uint8_t)w;

    w = 0;
    for (unsigned i = 0; i < s->coh_n_names; i++) {
        if (now_us - s->coh_name_us[i] < PRV_COHERE_WINDOW_US) {
            s->coh_name_h[w] = s->coh_name_h[i];
            s->coh_name_us[w] = s->coh_name_us[i];
            w++;
        }
    }
    s->coh_n_names = (uint8_t)w;
}

static unsigned cohere_peak(const prv_state_t *s, int8_t *at_rssi);

static void cohere_note(prv_state_t *s, const uint8_t addr[6], const char *name,
                        int8_t rssi, uint64_t t_us)
{
    cohere_expire(s, t_us);

    /* NO EARLY RETURNS PAST THIS POINT.
     *
     * Both the address and the name lookups below used to `return` on a hit,
     * which meant the latch at the bottom was reached only by the FIRST
     * sighting of each - so a steady flood, whose addresses and names are
     * quickly all known, stopped latching almost immediately. The verdict
     * then oscillated between IN USE and CAPABLE while nothing changed. */
    bool fresh_addr = true;
    const uint32_t h = addr_hash(addr);
    for (unsigned i = 0; i < s->coh_n; i++) {
        if (s->coh_addr_h[i] == h) {
            s->coh_us[i] = t_us; /* same address again: refresh, do not add */
            fresh_addr = false;
            break;
        }
    }
    if (fresh_addr && s->coh_n < PRV_COHERE_SLOTS) {
        s->coh_addr_h[s->coh_n] = h;
        s->coh_rssi[s->coh_n] = rssi;
        s->coh_us[s->coh_n] = t_us;
        s->coh_n++;
    }

    /* Names are tracked alongside, not per-address: the question is how many
     * DIFFERENT names the flood is wearing. An empty name is not one. */
    if (name && name[0]) {
        bool fresh_name = true;
        const uint32_t nh = str_hash(name);
        for (unsigned i = 0; i < s->coh_n_names; i++) {
            if (s->coh_name_h[i] == nh) {
                s->coh_name_us[i] = t_us;
                fresh_name = false;
                break;
            }
        }
        if (fresh_name && s->coh_n_names < PRV_NAME_SLOTS) {
            s->coh_name_h[s->coh_n_names] = nh;
            s->coh_name_us[s->coh_n_names] = t_us;
            s->coh_n_names++;
        }
    }

    /* Latch here, where the state is writable, rather than deciding afresh in
     * every evaluation. See PRV_FLOOD_HOLD_US. */
    if (cohere_peak(s, 0) >= PRV_COHERE_ADDRS) {
        s->coh_flood_us = t_us;
    }
}

/* The densest PRV_COHERE_DB-wide band of levels, and how many addresses sit
 * in it. A sliding window over a small array - forty entries, so the obvious
 * quadratic scan is cheaper than sorting and easier to be sure of. */
static unsigned cohere_peak(const prv_state_t *s, int8_t *at_rssi)
{
    unsigned best = 0;
    int8_t best_r = 0;
    for (unsigned i = 0; i < s->coh_n; i++) {
        const int lo = s->coh_rssi[i];
        unsigned n = 0;
        for (unsigned j = 0; j < s->coh_n; j++) {
            const int d = (int)s->coh_rssi[j] - lo;
            if (d >= 0 && d < PRV_COHERE_DB) {
                n++;
            }
        }
        if (n > best) {
            best = n;
            best_r = s->coh_rssi[i];
        }
    }
    if (at_rssi) {
        *at_rssi = best_r;
    }
    return best;
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

    /* Every advertisement feeds the coherence test, whatever it carries. */
    cohere_note(s, addr, name, rssi, t_us);

    /* Pairing-popup spam, into a window that slides. Each model carries its
     * own last-seen stamp and the advertisements go into per-second buckets;
     * nothing is reset wholesale, so a steady attack produces a steady
     * reading instead of a sawtooth. */
    {
        uint8_t code = 0;
        if (pairing_code(adv, adv_len, &code)) {
            const uint8_t slot = (uint8_t)(sec % 8u);
            if (s->pair_adv_sec[slot] != sec) {
                s->pair_adv_sec[slot] = sec;
                s->pair_adv_cnt[slot] = 0;
            }
            s->pair_adv_cnt[slot]++;

            /* And WHICH address it came from, because the spam that repeats a
             * single payload is invisible to the diversity test below and
             * obvious here: real accessories keep an address for minutes,
             * these tools draw a fresh one per advertisement. */
            {
                const uint32_t h = addr_hash(addr);
                bool known = false;
                for (uint8_t k = 0; k < s->pair_n_addrs; k++) {
                    if (s->pair_addr_h[k] == h) {
                        s->pair_addr_us[k] = t_us;
                        if (s->pair_addr_cnt[k] < 0xFFFFu) {
                            s->pair_addr_cnt[k]++;
                        }
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    if (s->pair_n_addrs < PRV_PAIR_ADDR_SLOTS) {
                        s->pair_addr_cnt[s->pair_n_addrs] = 1;
                        s->pair_addr_h[s->pair_n_addrs] = h;
                        s->pair_addr_us[s->pair_n_addrs] = t_us;
                        s->pair_n_addrs++;
                    } else {
                        /* Full is itself the finding - a spammer produces far
                         * more addresses than this holds - so evict the
                         * stalest and keep counting rather than stop. */
                        uint8_t oldest = 0;
                        for (uint8_t k = 1; k < s->pair_n_addrs; k++) {
                            if (s->pair_addr_us[k] < s->pair_addr_us[oldest]) {
                                oldest = k;
                            }
                        }
                        s->pair_addr_cnt[oldest] = 1;
                        s->pair_addr_h[oldest] = h;
                        s->pair_addr_us[oldest] = t_us;
                    }
                }
            }

            bool seen = false;
            for (uint8_t k = 0; k < s->pair_n_models; k++) {
                if (s->pair_models[k] == code) {
                    s->pair_model_us[k] = t_us;
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                if (s->pair_n_models < (uint8_t)(sizeof(s->pair_models))) {
                    s->pair_models[s->pair_n_models] = code;
                    s->pair_model_us[s->pair_n_models] = t_us;
                    s->pair_n_models++;
                } else {
                    /* Full: evict the stalest. A spammer cycles through more
                     * models than this table holds, and without eviction the
                     * table would freeze on the first sixteen it ever saw and
                     * stop tracking the attack that is actually running. */
                    uint8_t oldest = 0;
                    for (uint8_t k = 1; k < s->pair_n_models; k++) {
                        if (s->pair_model_us[k] < s->pair_model_us[oldest]) {
                            oldest = k;
                        }
                    }
                    s->pair_models[oldest] = code;
                    s->pair_model_us[oldest] = t_us;
                }
            }
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
    prv_device_t *d = admit(s, addr, kind, rssi, true, t_us);
    if (!d) {
        return;
    }
    d->ble = true;
    d->kind = kind;
    d->last_us = t_us;
    d->last_listen_us = s->listen_us;
    d->sightings++;
    if (rssi > d->best_rssi) {
        d->best_rssi = rssi;
    }
    if (d->name[0] == '\0') {
        if (name && name[0]) {
            /* Printable characters only. A spamming Flipper appends random
             * bytes to its advertised name, and rendering those straight to a
             * label puts replacement glyphs on the operator's screen and makes
             * two sightings of one device look like two devices. */
            uint8_t w = 0;
            for (uint8_t r = 0; name[r] && w < PR_NAME_MAX; r++) {
                const unsigned char c = (unsigned char)name[r];
                if (c >= 0x20 && c < 0x7F) {
                    d->name[w++] = (char)c;
                }
            }
            while (w && d->name[w - 1] == ' ') {
                w--;
            }
            d->name[w] = '\0';
        }
        if (d->name[0] == '\0' && colour && colour[0]) {
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
/* Hak5's registered OUI. An appliance can be renamed in a minute; the first
 * three bytes of its radio's address are assigned by the IEEE and are what the
 * vendor actually shipped. Sufficient on its own, unlike the name - which is
 * why a renamed Pineapple stopped being invisible.
 *
 * Not proof of an attack. A Pineapple on a shelf is a Pineapple on a shelf,
 * and the presence cap in this engine applies to it exactly as it does to a
 * Flipper. */
bool prv_is_hak5_oui(const uint8_t addr[6])
{
    /* 00:13:37 - assigned to Hak5 LLC, and no, that is not a coincidence. */
    return addr && addr[0] == 0x00 && addr[1] == 0x13 && addr[2] == 0x37;
}

/* ESPRESSIF'S OUIs - THE SIGNATURE OF A DEV BOARD PRETENDING TO BE A ROUTER.
 *
 * Nearly every cheap Wi-Fi attack tool in circulation is an ESP32 or ESP8266:
 * Marauder, the Deauther family, the evil-portal builds, the beacon spammers.
 * When any of them stands up an access point - which an evil portal must, to
 * serve its captive page - the beacon goes out from a radio whose address
 * Espressif was assigned by the IEEE. No manufactured router uses one.
 *
 * That makes an access point on one of these prefixes a genuinely strong
 * signal, and it is the one thing this engine could not see: a Marauder doing
 * a deauthentication flood is caught by Watch, doing beacon spam by Mirage -
 * but sitting there as a rogue access point it looked like any other network.
 *
 * WHAT IT IS NOT. It is not proof of an attack, and the honesty matters more
 * here than usual, because the false positives are somebody's furniture:
 * ESPHome sensors, Tasmota smart plugs, Shelly relays and a thousand hobby
 * projects are all ESP32s, and a good number of them run an access point for
 * setup. So this classifies as DEV_BOARD - "something programmable is here" -
 * and the presence cap applies exactly as it does to a Flipper. What raises
 * it is the SHAPE: a dev board running an OPEN network is the arrangement an
 * evil portal needs and a smart plug does not.
 *
 * The list is the common prefixes, not all of them - Espressif holds dozens
 * and buys more. A miss is a device this test does not fire on, which is the
 * safe direction: everything else in the engine still applies to it. */
bool prv_is_devboard_oui(const uint8_t addr[6])
{
    static const uint8_t k_espressif[][3] = {
        { 0x18, 0xFE, 0x34 }, { 0x24, 0x0A, 0xC4 }, { 0x24, 0x6F, 0x28 },
        { 0x24, 0xB2, 0xDE }, { 0x2C, 0x3A, 0xE8 }, { 0x2C, 0xF4, 0x32 },
        { 0x30, 0xAE, 0xA4 }, { 0x34, 0xAB, 0x95 }, { 0x3C, 0x61, 0x05 },
        { 0x3C, 0x71, 0xBF }, { 0x40, 0xF5, 0x20 }, { 0x48, 0x3F, 0xDA },
        { 0x4C, 0x11, 0xAE }, { 0x50, 0x02, 0x91 }, { 0x54, 0x5A, 0xA6 },
        { 0x58, 0xBF, 0x25 }, { 0x5C, 0xCF, 0x7F }, { 0x60, 0x01, 0x94 },
        { 0x68, 0x67, 0x25 }, { 0x68, 0xC6, 0x3A }, { 0x78, 0x21, 0x84 },
        { 0x7C, 0x9E, 0xBD }, { 0x7C, 0xDF, 0xA1 }, { 0x80, 0x7D, 0x3A },
        { 0x84, 0x0D, 0x8E }, { 0x84, 0xCC, 0xA8 }, { 0x8C, 0xAA, 0xB5 },
        { 0x90, 0x97, 0xD5 }, { 0x98, 0xF4, 0xAB }, { 0x9C, 0x9C, 0x1F },
        { 0xA0, 0x20, 0xA6 }, { 0xA4, 0x7B, 0x9D }, { 0xA4, 0xCF, 0x12 },
        { 0xAC, 0x67, 0xB2 }, { 0xB4, 0xE6, 0x2D }, { 0xB8, 0xF0, 0x09 },
        { 0xBC, 0xDD, 0xC2 }, { 0xC0, 0x49, 0xEF }, { 0xC4, 0x4F, 0x33 },
        { 0xC8, 0x2B, 0x96 }, { 0xC8, 0xC9, 0xA3 }, { 0xCC, 0x50, 0xE3 },
        { 0xCC, 0xDB, 0xA7 }, { 0xD4, 0xD4, 0xDA }, { 0xD8, 0xA0, 0x1D },
        { 0xDC, 0x4F, 0x22 }, { 0xE0, 0x98, 0x06 }, { 0xE8, 0x31, 0xCD },
        { 0xE8, 0xDB, 0x84 }, { 0xEC, 0xFA, 0xBC }, { 0xF0, 0x08, 0xD1 },
        { 0xF4, 0xCF, 0xA2 }, { 0xFC, 0xF5, 0xC4 },
    };
    if (!addr) {
        return false;
    }
    /* A locally-administered address is not a manufacturer's assignment at
     * all, so an OUI table says nothing about it - and every one of these
     * tools can set one. Checking anyway would produce confident matches from
     * a randomised address, which is worse than the miss. */
    if (addr[0] & 0x02) {
        return false;
    }
    for (unsigned i = 0; i < sizeof(k_espressif) / sizeof(k_espressif[0]); i++) {
        if (memcmp(addr, k_espressif[i], 3) == 0) {
            return true;
        }
    }
    return false;
}

bool prv_is_implant_oui(const uint8_t addr[6])
{
    /* DE:4F:22 - the locally-administered form of Espressif's DC:4F:22, and
     * the documented signature of the O.MG cable family. See the header for
     * why this is checked separately from the dev-board table. */
    return addr && addr[0] == 0xDE && addr[1] == 0x4F && addr[2] == 0x22;
}

uint64_t prv_expiry_us(const prv_device_t *d)
{
    /* Under three sightings there is no cadence to measure - one interval
     * between two adverts is a sample of one - so the patient ceiling applies.
     * That is the right way round: the less this engine knows about how
     * talkative something is, the longer it waits before calling it gone. */
    if (!d || d->sightings < 3u || d->last_us <= d->first_us) {
        return PRV_STALE_US;
    }
    const uint64_t mean = (d->last_us - d->first_us) / (uint64_t)(d->sightings - 1u);
    uint64_t win = mean * PRV_SILENCE_MULTIPLE;
    if (win < PRV_STALE_MIN_US) {
        win = PRV_STALE_MIN_US;
    }
    if (win > PRV_STALE_US) {
        win = PRV_STALE_US;
    }
    return win;
}

void prv_listen(prv_state_t *s, uint64_t dt_us)
{
    if (s) {
        s->listen_us += dt_us;
    }
}

/* `listen_now` is the engine's listening clock, not the wall clock. A device
 * is stale when we RECEIVED for longer than its own cadence allows without
 * hearing it - so a turn spent on another watch's band ages nothing. */
static bool stale_on_listen(const prv_device_t *d, uint64_t listen_now)
{
    if (!d || listen_now <= d->last_listen_us) {
        return false;
    }
    return (listen_now - d->last_listen_us) > prv_expiry_us(d);
}

bool prv_is_stale(const prv_device_t *d, uint64_t now_us)
{
    if (!d || !now_us || now_us <= d->last_us) {
        return false;
    }
    return (now_us - d->last_us) > prv_expiry_us(d);
}

bool prv_is_pwnagotchi_addr(const uint8_t addr[6])
{
    static const uint8_t k_pwn[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD };
    return addr && memcmp(addr, k_pwn, 6) == 0;
}

void prv_observe_beacon(prv_state_t *s, const uint8_t bssid[6], const char *ssid,
                        uint8_t ssid_len, bool whisper, int8_t rssi,
                        uint64_t t_us)
{
    prv_observe_beacon_ex(s, bssid, ssid, ssid_len, whisper, false, rssi, t_us);
}

void prv_observe_beacon_ex(prv_state_t *s, const uint8_t bssid[6],
                           const char *ssid, uint8_t ssid_len, bool whisper,
                           bool open_network, int8_t rssi, uint64_t t_us)
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
    } else if (prv_is_hak5_oui(bssid)) {
        /* Checked BEFORE the name, so renaming the network does not hide the
         * hardware. The name is still lifted below and shown. */
        kind = PRV_KIND_PINEAPPLE;
    } else if (ssid && ssid_len) {
        kind = prv_classify_name(ssid, ssid_len, false);
    }

    /* THE DEV BOARD RUNNING AN ACCESS POINT.
     *
     * Checked after the name, so a board that announces itself keeps the more
     * specific classification. An Espressif radio beaconing is a programmable
     * device, not a manufactured router - but it is also every ESPHome sensor
     * and Tasmota plug in the building, so on its own it is only DEV_BOARD.
     *
     * OPEN is what changes it. An evil portal must be joinable without a key,
     * because its whole purpose is to get a stranger's browser onto a page it
     * serves; a smart plug that has finished being set up has no reason to be.
     * A dev board offering an open network is the shape of a captive portal,
     * and that earns the classification the name-matching path gives a board
     * that admits what it is. */
    if (kind == PRV_KIND_NONE && prv_is_implant_oui(bssid)) {
        /* Checked before the dev-board table: it is the more specific claim,
         * and the address it matches is one the dev-board test refuses. */
        kind = PRV_KIND_IMPLANT;
    }

    if (kind == PRV_KIND_NONE && prv_is_devboard_oui(bssid)) {
        kind = open_network ? PRV_KIND_ROGUE_AP : PRV_KIND_DEV_BOARD;
    }

    if (kind == PRV_KIND_NONE) {
        return;
    }
    prv_device_t *d = admit(s, bssid, kind, rssi, false, t_us);
    if (!d) {
        return;
    }
    d->ble = false;
    d->kind = kind;
    d->last_us = t_us;
    d->last_listen_us = s->listen_us;
    d->sightings++;
    if (rssi > d->best_rssi) {
        d->best_rssi = rssi;
    }
    if (d->name[0] == '\0' && ssid && ssid_len) {
        const uint8_t n = ssid_len > PR_NAME_MAX ? PR_NAME_MAX : ssid_len;
        char tmp[PR_NAME_MAX + 1];
        memcpy(tmp, ssid, n);
        tmp[n] = '\0';
        if (name_is_sane(tmp)) {
            snprintf(d->name, sizeof(d->name), "%s", tmp);
        }
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
    /* Above the rogue access point and below the purpose-built appliances. A
     * cable with a radio in it is a serious thing to find, and the address it
     * is recognised by is one anything could have set - so it does not
     * outrank hardware that announced itself. */
    case PRV_KIND_IMPLANT:       return 36;
    case PRV_KIND_PINEAPPLE:     return 38;
    /* Between the deauther board and the plain dev board, deliberately. An
     * open network from an Espressif radio is the shape of a captive portal
     * and is a much stronger signal than "an ESP32 exists here" - but it is
     * still inference from a shape, not a name the firmware admitted to, so
     * it does not outrank the boards that say what they are. */
    case PRV_KIND_ROGUE_AP:      return 32;
    case PRV_KIND_PWNAGOTCHI:    return 36;
    case PRV_KIND_DEAUTHER:      return 34;
    case PRV_KIND_SERIAL_BRIDGE: return 18;
    case PRV_KIND_DEV_BOARD:     return 12;
    default:                     return 0;
    }
}

void prv_evaluate(const prv_state_t *s, uint64_t now_us, prv_verdict_t *out)
{
    /* MEASURED FIRST, BECAUSE IT CHANGES WHAT THE NAMES MEAN.
     *
     * The device loop below identifies hardware by announced name, and during
     * a rotation flood those names belong to the attacker. So whether a flood
     * is in view has to be known before any identification is trusted, not
     * after - which is where this test originally sat. */
    int8_t coh_at = 0;
    unsigned coh_tight = 0;
    bool flood_in_view = false;
    if (s) {
        coh_tight = cohere_peak(s, &coh_at);
        flood_in_view = s->coh_flood_us &&
                        (now_us >= s->coh_flood_us) &&
                        (now_us - s->coh_flood_us) < PRV_FLOOD_HOLD_US;
    }

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
    if (s->full) {
        out->notes |= PRV_NOTE_FULL;
    }

    uint8_t best = 0;
    /* Count KINDS, and separately how many addresses they were heard from.
     * Reporting the address count as a device count is what put twenty-four
     * Flippers on the screen. */
    bool kind_seen[PRV_KIND_COUNT];
    memset(kind_seen, 0, sizeof(kind_seen));
    for (unsigned i = 0; i < s->n; i++) {
        const prv_device_t *d = &s->dev[i];
        if (!d->in_use) {
            continue;
        }
        /* AND STILL HERE. The list below this one has always dropped hardware
         * that stopped transmitting; this loop did not, so switching a Flipper
         * off left the count saying 1 above a list showing nothing. A screen
         * that contradicts itself is worse than either half alone - the
         * operator has to decide which line to believe, and there is no way
         * for them to tell. One staleness rule, applied in both places. */
        if (stale_on_listen(d, s->listen_us)) {
            continue;
        }
        /* FLOOD DEBRIS IS NOT A DEVICE.
         *
         * A rotation flood advertises whatever names its author chose, and one
         * of the four measured here was "Flipper". Aggregated by name, that
         * became a confident "Flipper Zero present, 245 addresses" - an
         * identification manufactured by the attack itself, while the
         * operator's real Flipper was not advertising at all. A genuine
         * device does not wear two hundred addresses. */
        if (flood_in_view && d->addresses >= PRV_COHERE_NAMES &&
            (uint32_t)d->sightings * PRV_STEADY_DEN <
                (uint32_t)d->addresses * PRV_STEADY_NUM) {
            continue;
        }
        if (d->kind < PRV_KIND_COUNT && !kind_seen[d->kind]) {
            kind_seen[d->kind] = true;
            out->n_devices++;
            out->n_addresses = (uint16_t)(out->n_addresses + d->addresses);
            if (d->kind == PRV_KIND_FLIPPER) {
                out->n_flipper++;
            }
            if (d->kind == PRV_KIND_PWNAGOTCHI) {
                out->n_pwnagotchi++;
            }
            if (!d->ble) {
                out->n_wifi_tools++;
            }
        }
        const uint8_t w = kind_weight(d->kind);
        if (w > best) {
            best = w;
            out->worst_kind = d->kind;
            memcpy(out->worst_addr, d->addr, 6);
            snprintf(out->worst_name, sizeof(out->worst_name), "%s", d->name);
            out->worst_rssi = d->best_rssi;
            out->worst_addresses = d->addresses;
            out->worst_age_s = (uint32_t)((now_us > d->last_us)
                                              ? (now_us - d->last_us) / 1000000ull
                                              : 0ull);
        }
    }
    /* Give the headline the steadiest name for that kind rather than whichever
     * address happened to be loudest - see prv_device_at. */
    if (out->worst_kind != PRV_KIND_NONE) {
        prv_device_t top;
        if (prv_device_at(s, 0, &top) && top.kind == out->worst_kind) {
            snprintf(out->worst_name, sizeof(out->worst_name), "%s", top.name);
            memcpy(out->worst_addr, top.addr, 6);
            out->worst_rssi = top.best_rssi;
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
    /* Count only what is still INSIDE the window. Expiring per model rather
     * than clearing the table is what stops the reading oscillating. */
    {
        const uint32_t now_sec = (uint32_t)(now_us / 1000000ull);
        for (uint8_t k = 0; k < s->pair_n_models; k++) {
            if (now_us >= s->pair_model_us[k] &&
                now_us - s->pair_model_us[k] <= PRV_SPAM_WINDOW_US) {
                out->pair_models++;
            }
        }
        /* And how many different addresses carried those payloads, expiring
         * the same way. This is the half that sees single-payload spam. */
        for (uint8_t k = 0; k < s->pair_n_addrs; k++) {
            if (s->pair_addr_h[k] && now_us >= s->pair_addr_us[k] &&
                now_us - s->pair_addr_us[k] <= PRV_SPAM_WINDOW_US) {
                out->pair_addrs++;
            }
        }
        const uint32_t win_s =
            (uint32_t)(PRV_SPAM_WINDOW_US / 1000000ull);
        for (uint8_t k = 0; k < 8; k++) {
            if (s->pair_adv_cnt[k] == 0 || s->pair_adv_sec[k] > now_sec) {
                continue;
            }
            if (now_sec - s->pair_adv_sec[k] < win_s) {
                out->pair_advs += s->pair_adv_cnt[k];
            }
        }
    }

    /* Either shape counts. Diversity catches the tools that cycle payloads;
     * address churn catches the ones that hammer a single payload, which the
     * diversity test scores as one model and would otherwise miss entirely. */
    /* Three shapes now. Diversity of payload, diversity of address, or sheer
     * rate - because an iOS-targeted flood varies neither of the first two
     * and was therefore scored CLEAR while raising dialogs twenty-two times a
     * second. */
    /* The busiest single address inside the window. */
    for (uint8_t k = 0; k < s->pair_n_addrs; k++) {
        if (now_us >= s->pair_addr_us[k] &&
            now_us - s->pair_addr_us[k] <= PRV_SPAM_WINDOW_US &&
            s->pair_addr_cnt[k] > out->pair_worst_addr) {
            out->pair_worst_addr = s->pair_addr_cnt[k];
        }
    }

    /* Three shapes. Diversity of payload, diversity of address, or one
     * address hammering - because an iOS-targeted flood varies neither of the
     * first two and was scored CLEAR while raising dialogs 22 times a second. */
    const bool pair_spam = (out->pair_advs >= PRV_SPAM_ADVS) &&
                           (out->pair_models >= PRV_SPAM_MODELS ||
                            out->pair_addrs >= PRV_SPAM_ADDRS ||
                            out->pair_worst_addr >= PRV_SPAM_PAIR_RATE);

    /* THE EARLY EXIT THAT MADE THE LENS BLIND.
     *
     * Three ways to be interesting, and a flood that satisfies none of them
     * left through this door before anything else could look at it: no device
     * NAMED (the names are junk, or the flood's debris has just been
     * discarded), under sixty advertisements a second (a steady eight-a-second
     * rotation is not), and no pairing payload (a name-rotation flood carries
     * none). That is exactly the attack measured on hardware, and it was
     * returned as "nothing announced itself to this receiver" while 244
     * addresses were in view.
     *
     * The coherence test is a fourth way, and it has to be consulted HERE
     * rather than fifty lines further down where it used to sit. */
    if (out->n_devices == 0 && out->peak_adv_per_s < 60u && !pair_spam &&
        !flood_in_view) {
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

    /* ---- one radio wearing many addresses ----------------------------
     *
     * The payload-independent test. It is deliberately evaluated LAST and on
     * its own terms: it does not need a pairing payload, a known model code,
     * or a recognisable name, so it is the only one of these that can see a
     * flood nobody has catalogued yet.
     *
     * Measured against a real name-rotation flood the other two scored it
     * CAPABLE - "a tool is present" - while 244 addresses at one signal level
     * sat in plain view. */
    {
        const unsigned tight = coh_tight;
        out->cohere_addrs = (uint8_t)(tight > 255u ? 255u : tight);
        out->cohere_names = s->coh_n_names;
        out->cohere_rssi = coh_at;

        if (tight >= PRV_COHERE_ADDRS) {
            out->notes |= PRV_NOTE_COHERENT;
            /* Every name in view is now the attacker's to choose. */
            out->notes |= PRV_NOTE_NAMES_FORGED;
            out->families |= PRV_FAM_ONE_RADIO;
            out->families |= PRV_FAM_ACTIVE;
            if (score < 66u) {
                score = 66u;
            }
            /* Several names from one radio is the second half of the same
             * observation, and it is what separates a rotation flood from a
             * single device that merely re-randomises often. */
            if (s->coh_n_names >= PRV_COHERE_NAMES) {
                out->notes |= PRV_NOTE_MANY_NAMES;
                if (score < 74u) {
                    score = 74u;
                }
            }
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

/* One entry per KIND, aggregated across every address it was heard from.
 *
 * The table underneath is still per-address, because that is what arrives and
 * because counting distinct addresses is itself worth reporting. What the
 * operator sees is hardware.
 *
 * The name shown is the one from the MOST-HEARD address, which picks the right
 * one for free: a device's genuine advertisement repeats steadily while each
 * spoofed spam address appears once or twice. That is how "R3ghon" survives a
 * flood of "Flipper" plus random bytes. */
bool prv_device_at(const prv_state_t *s, unsigned index, prv_device_t *out)
{
    return prv_device_at_now(s, index, 0, out);
}

bool prv_device_at_now(const prv_state_t *s, unsigned index, uint64_t now_us,
                       prv_device_t *out)
{
    if (!s || !out) {
        return false;
    }
    /* Walk kinds in capability order so the list leads with the most capable
     * thing present, and pick out the index-th kind that is actually here. */
    unsigned seen_kinds = 0;
    for (int rank = PRV_KIND_COUNT - 1; rank > PRV_KIND_NONE; rank--) {
        bool present = false;
        prv_device_t agg;
        memset(&agg, 0, sizeof(agg));
        agg.best_rssi = -128;
        uint32_t best_sightings = 0;

        for (unsigned i = 0; i < s->n; i++) {
            const prv_device_t *d = &s->dev[i];
            if (!d->in_use || (int)d->kind != rank) {
                continue;
            }
            /* The list must agree with the count above it: a device the
             * verdict has dropped as stale cannot still have a row. */
            if (stale_on_listen(d, s->listen_us)) {
                continue;
            }
            /* ...and neither can one the verdict discarded as flood debris.
             *
             * Measured on hardware: "hardware identified 0" sat three rows
             * above "Flipper Zero  215 addr". A screen that contradicts
             * itself is worse than either half alone, because the operator
             * has to decide which line to believe and nothing tells them
             * which. One suppression rule, applied in both places - exactly
             * as the staleness rule above already is. */
            if (s->coh_flood_us && now_us >= s->coh_flood_us &&
                (now_us - s->coh_flood_us) < PRV_FLOOD_HOLD_US &&
                d->addresses >= PRV_COHERE_NAMES &&
                (uint32_t)d->sightings * PRV_STEADY_DEN <
                    (uint32_t)d->addresses * PRV_STEADY_NUM) {
                continue;
            }
            present = true;
            agg.kind = d->kind;
            /* SUM the counts rather than counting entries. admit() already
             * folds a kind's rotating addresses into one entry, so counting
             * entries here would report 1 for a device that has been heard
             * from thirty addresses - aggregating twice loses the very number
             * this field exists to carry. */
            agg.addresses = (uint16_t)(agg.addresses + d->addresses);
            agg.sightings += d->sightings;
            agg.ble = d->ble;
            if (d->best_rssi > agg.best_rssi) {
                agg.best_rssi = d->best_rssi;
            }
            if (agg.first_us == 0 || d->first_us < agg.first_us) {
                agg.first_us = d->first_us;
            }
            if (d->last_us > agg.last_us) {
                agg.last_us = d->last_us;
            }
            /* The steadiest address supplies the name and the address shown. */
            if (d->sightings > best_sightings) {
                best_sightings = d->sightings;
                memcpy(agg.addr, d->addr, 6);
                if (d->name[0]) {
                    memcpy(agg.name, d->name, sizeof(agg.name));
                }
            }
        }
        if (!present) {
            continue;
        }
        if (seen_kinds == index) {
            agg.in_use = true;
            *out = agg;
            return true;
        }
        seen_kinds++;
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
    case PRV_KIND_IMPLANT:       return "cable/plug implant";
    case PRV_KIND_ROGUE_AP:      return "open dev-board AP";
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
    case PRV_KIND_IMPLANT:
        return "A charging cable with a radio and a keyboard in it";
    case PRV_KIND_ROGUE_AP:
        return "Open network from a dev board - a portal's shape";
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
