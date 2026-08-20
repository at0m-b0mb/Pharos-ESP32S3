#include "pharos_vigil.h"

#include <string.h>

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static unsigned popcount8(uint8_t v)
{
    unsigned n = 0;
    while (v) { n += (v & 1u); v >>= 1; }
    return n;
}

void pv_reset(pv_state_t *s)
{
    if (s) {
        memset(s, 0, sizeof(*s));
        /* Movement is UNKNOWN until an IMU says otherwise, and unknown must
         * never become a veto - a board with no motion sensor has to behave
         * exactly as it did before there was one. */
        s->moved = true;
        s->moved_known = false;
    }
}

bool pv_mark_known(pv_state_t *s, const uint8_t addr[6])
{
    if (!s || !addr || s->n_known >= PV_MAX_KNOWN) {
        return false;
    }
    for (unsigned i = 0; i < s->n_known; i++) {
        if (memcmp(s->known[i], addr, 6) == 0) {
            return true; /* already yours */
        }
    }
    memcpy(s->known[s->n_known++], addr, 6);
    return true;
}

static bool is_known(const pv_state_t *s, const uint8_t addr[6])
{
    for (unsigned i = 0; i < s->n_known; i++) {
        if (memcmp(s->known[i], addr, 6) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- payload classification ------------------------------------------ */

pv_kind_t pv_classify(const uint8_t *data, uint8_t len)
{
    if (!data || len < 3) {
        return PV_KIND_UNKNOWN;
    }
    /* Walk the AD structures: [len][type][payload...] */
    uint8_t i = 0;
    while (i + 1u < len) {
        const uint8_t l = data[i];
        if (l == 0 || (uint16_t)i + 1u + l > (uint16_t)len) {
            break; /* malformed or truncated - stop rather than over-read */
        }
        const uint8_t type = data[i + 1];
        const uint8_t *p = &data[i + 2];
        const uint8_t plen = (uint8_t)(l - 1);

        if (type == 0xFF && plen >= 3) {
            /* Manufacturer specific data; company ID is little-endian. */
            const uint16_t company = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
            if (company == 0x004C) { /* Apple */
                /* Find My advertises type 0x12. The byte after the length
                 * carries a status field whose high bit distinguishes a tag
                 * whose owner is nearby from one that is SEPARATED - and the
                 * separated case is the one that matters, because a tracker
                 * planted on somebody is by definition away from its owner. */
                if (plen >= 4 && p[2] == 0x12) {
                    const uint8_t status = p[4 < plen ? 4 : plen - 1];
                    return (status & 0x04) ? PV_KIND_FINDMY_LOST : PV_KIND_FINDMY;
                }
                return PV_KIND_UNKNOWN; /* other Apple traffic: phones, buds */
            }
            if (company == 0x0075 && plen >= 4) { /* Samsung */
                return PV_KIND_SMARTTAG;
            }
        } else if ((type == 0x08 || type == 0x09) && plen >= 3) {
            /* Shortened or complete local name. Two things announce
             * themselves here by name, and both are worth knowing about.
             *
             * A Flipper Zero ships advertising "Flipper <name>". It is not an
             * attack by itself - it is a tool, and plenty of people carry one
             * legitimately - but a defender walking a site should know one is
             * in the room, because it is the single most capable piece of
             * hobbyist RF hardware they are likely to meet.
             *
             * The serial bridges are the other. See PV_BREDR_BLIND in the
             * header for why this is reported as a BRIDGE and never as a
             * "skimmer": the same module is in doorbells and hobby projects,
             * and calling one a card skimmer would be the most damaging false
             * positive this project could produce. */
            char nm[24];
            uint8_t n = plen < (uint8_t)(sizeof(nm) - 1u) ? plen : (uint8_t)(sizeof(nm) - 1u);
            for (uint8_t k = 0; k < n; k++) {
                nm[k] = (char)p[k];
            }
            nm[n] = '\0';
            if (n >= 7 && nm[0] == 'F' && nm[1] == 'l' && nm[2] == 'i' &&
                nm[3] == 'p' && nm[4] == 'p' && nm[5] == 'e' && nm[6] == 'r') {
                return PV_KIND_FLIPPER;
            }
            static const char *k_bridges[] = {
                "HC-05", "HC-06", "HC-08", "JDY-", "AT-09", "MLT-BT05",
                "FCD", "FREE2MOVE", "BT05", "HM-10",
            };
            for (unsigned b = 0; b < sizeof(k_bridges) / sizeof(k_bridges[0]); b++) {
                const char *needle = k_bridges[b];
                unsigned m = 0;
                while (needle[m] && nm[m] && nm[m] == needle[m]) {
                    m++;
                }
                if (!needle[m]) {
                    return PV_KIND_SERIAL;
                }
            }
        } else if ((type == 0x16 || type == 0x14) && plen >= 2) {
            /* Service data / 16-bit service UUID, little-endian. */
            const uint16_t uuid = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
            if (uuid == 0xFEED || uuid == 0xFEEC) {
                return PV_KIND_TILE;
            }
            if (uuid == 0xFD5A) {
                return PV_KIND_SMARTTAG;
            }
            if (uuid == 0xFD44 || uuid == 0xFD43) {
                return PV_KIND_FINDMY; /* Apple service data */
            }
            if (uuid == 0xFE33) {
                return PV_KIND_CHIPOLO;
            }
        }
        i = (uint8_t)(i + 1u + l);
    }
    return PV_KIND_UNKNOWN;
}

/* ---- locales ---------------------------------------------------------- */

/* How different are two landscape signatures, per mille? The signature is a
 * set of bits, so the symmetric difference over the union is a serviceable
 * distance and needs no floating point. */
static uint32_t sig_distance_permil(uint32_t a, uint32_t b)
{
    if (a == 0 && b == 0) {
        return 0;
    }
    const uint32_t diff = a ^ b;
    const uint32_t uni = a | b;
    unsigned nd = 0, nu = 0;
    for (unsigned i = 0; i < 32; i++) {
        if (diff & (1u << i)) nd++;
        if (uni & (1u << i)) nu++;
    }
    return nu ? (uint32_t)((nd * 1000u) / nu) : 0;
}

void pv_set_moved(pv_state_t *s, bool moved)
{
    if (!s) {
        return;
    }
    s->moved = moved;
    s->moved_known = true;
}

void pv_observe_locale(pv_state_t *s, uint32_t sig, uint64_t t_us)
{
    if (!s || sig == 0) {
        return;
    }
    if (s->first_us == 0) {
        s->first_us = t_us;
    }
    s->last_us = t_us;

    if (s->n_locales == 0) {
        s->locale_sig[0] = sig;
        s->n_locales = 1;
        s->cur_locale = 0;
        s->cur_sig = sig;
        return;
    }
    if (sig_distance_permil(s->cur_sig, sig) < PV_LOCALE_CHANGE_PERMIL) {
        /* Same place; let the signature drift with it. */
        s->cur_sig = sig;
        return;
    }

    /* Somewhere else. Have we been here before? */
    for (unsigned i = 0; i < s->n_locales; i++) {
        if (sig_distance_permil(s->locale_sig[i], sig) < PV_LOCALE_CHANGE_PERMIL) {
            s->cur_locale = i;
            s->cur_sig = sig;
            return;
        }
    }
    if (s->n_locales < PV_MAX_LOCALES) {
        s->locale_sig[s->n_locales] = sig;
        s->cur_locale = s->n_locales;
        s->n_locales++;
    } else {
        /* Ring over the oldest rather than stop noticing movement. */
        s->cur_locale = (s->cur_locale + 1u) % PV_MAX_LOCALES;
        s->locale_sig[s->cur_locale] = sig;
    }
    s->cur_sig = sig;
}

/* ---- advertisements --------------------------------------------------- */

void pv_observe_adv(pv_state_t *s, const uint8_t addr[6], uint8_t addr_type,
                    int8_t rssi, const uint8_t *data, uint8_t len,
                    uint64_t t_us)
{
    if (!s || !addr) {
        return;
    }
    const pv_kind_t kind = pv_classify(data, len);
    if (kind == PV_KIND_UNKNOWN) {
        return; /* not a tracker we can name; phones and laptops are not this */
    }
    if (s->first_us == 0) {
        s->first_us = t_us;
    }
    s->last_us = t_us;

    pv_tag_t *tag = NULL;
    for (unsigned i = 0; i < s->n; i++) {
        if (s->tags[i].in_use && memcmp(s->tags[i].addr, addr, 6) == 0) {
            tag = &s->tags[i];
            break;
        }
    }
    if (!tag) {
        if (s->n >= PV_MAX_TAGS) {
            s->full = true;
            return;
        }
        tag = &s->tags[s->n++];
        memset(tag, 0, sizeof(*tag));
        memcpy(tag->addr, addr, 6);
        tag->addr_type = addr_type;
        tag->in_use = true;
        tag->first_us = t_us;
        tag->best_rssi = rssi;
    }
    /* A tag that ever advertises as separated is remembered that way: that is
     * the state we care about, and it should not be masked by a later frame. */
    if (tag->kind != PV_KIND_FINDMY_LOST) {
        tag->kind = kind;
    }
    tag->sightings++;
    tag->last_us = t_us;
    if (rssi > tag->best_rssi) {
        tag->best_rssi = rssi;
    }
    if (s->n_locales) {
        const uint8_t bit = (uint8_t)(1u << (s->cur_locale & 7u));
        if (!(tag->locale_mask & bit)) {
            tag->locale_mask |= bit;
            tag->n_locales = (uint8_t)popcount8(tag->locale_mask);
        }
    }
}

/* ---- verdict ---------------------------------------------------------- */

static pv_band_t band_of(uint8_t score)
{
    if (score >= 70) return PV_BAND_FOLLOWING;
    if (score >= 45) return PV_BAND_PERSISTENT;
    if (score >= 20) return PV_BAND_SEEN;
    return PV_BAND_CLEAR;
}

void pv_evaluate(const pv_state_t *s, uint64_t now_us, pv_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "";
    out->worst_kind = PV_KIND_UNKNOWN;
    if (!s) {
        return;
    }
    if (now_us < s->last_us) {
        now_us = s->last_us;
    }
    out->n_locales = (uint8_t)s->n_locales;

    /* Confidence. Bluetooth advertising is cheap to hear - no channel hopping
     * problem here - so the ceiling is set by TIME and MOVEMENT instead: the
     * question is whether we have had the chance to see a tag fail to follow.
     * Rotation is the permanent discount. */
    uint32_t ceiling = 70;
    if (s->n_locales >= 3) ceiling = 92;
    else if (s->n_locales == 2) ceiling = 84;

    /* THE MOVEMENT GATE.
     *
     * Locale turnover is evidence of movement, not proof of it - a router
     * rebooting or half an estate dropping out of earshot looks identical to
     * having walked somewhere. The accelerometer is wrong in entirely
     * different circumstances, so when it says confidently that nobody went
     * anywhere, FOLLOWING is withheld regardless of how many locales were
     * counted.
     *
     * This is a CEILING rather than a subtraction, for the same reason every
     * other ceiling in this project is: the evidence that was seen is still
     * reported honestly, and what changes is how much can be concluded from
     * it. The band drops to PERSISTENT - "this has been near you a while",
     * which is true and is not an accusation of stalking.
     *
     * Only applied when the IMU actually answered. On a board without one,
     * movement is unknown and unknown must never become a veto. */
    if (s->moved_known && !s->moved && ceiling > 69u) {
        ceiling = 69;
    }
    const uint64_t span_us = (s->first_us && now_us > s->first_us)
                                 ? (now_us - s->first_us) : 0;
    const uint32_t span_min = (uint32_t)(span_us / 60000000ull);
    if (span_min < 5) {
        ceiling = ceiling > 20 ? ceiling - 20 : 10;
        out->notes |= PV_NOTE_SHORT;
    }
    out->ceiling = (uint8_t)ceiling;

    out->notes |= PV_NOTE_ROTATION; /* always true, always disclosed */
    if (s->n_locales <= 1) {
        out->notes |= PV_NOTE_ONE_PLACE;
    }
    if (s->full) {
        out->notes |= PV_NOTE_FULL;
    }
    if (s->n_known) {
        out->notes |= PV_NOTE_KNOWN;
    }

    uint32_t best = 0;
    for (unsigned i = 0; i < s->n; i++) {
        const pv_tag_t *t = &s->tags[i];
        if (!t->in_use || is_known(s, t->addr)) {
            continue;
        }
        out->n_tags++;
        if (t->n_locales >= 2) {
            out->n_following++;
        }

        /* Score this one. Presence in several locales is the whole argument;
         * duration is corroboration, and the separated state is what makes a
         * Find My tag interesting rather than somebody's nearby phone. */
        uint32_t sc = 0;
        if (t->n_locales >= 4)      sc = 88;
        else if (t->n_locales == 3) sc = 80;
        else if (t->n_locales == 2) sc = 66;
        else                        sc = 24; /* seen, but you have not moved */

        const uint32_t mins = (uint32_t)((t->last_us > t->first_us)
                                             ? (t->last_us - t->first_us) / 60000000ull
                                             : 0);
        if (mins >= 30)      sc += 8;
        else if (mins >= 10) sc += 4;

        if (t->kind == PV_KIND_FINDMY_LOST) {
            sc += 6; /* away from its owner: the state that matters */
        }
        sc = clamp_u32(sc, 0, 100);

        if (sc > best) {
            best = sc;
            memcpy(out->worst_addr, t->addr, 6);
            out->worst_kind = t->kind;
            out->worst_locales = t->n_locales;
            out->worst_minutes = mins;
        }
    }

    uint32_t score = best;

    /* THE cap that keeps this from frightening people in cafés. Without a
     * second locale there is no evidence of following at all - only of
     * existing, which every tracker in every bag in the room also does. */
    if (s->n_locales <= 1 && score > 44) {
        score = 44;
    }
    /* And a single move is a coincidence waiting to happen: you and a stranger
     * can walk the same way once. */
    if (out->n_following && out->worst_locales < 3 && score > 69) {
        score = 69;
    }

    score = clamp_u32(score, 0, 100);
    out->raw_score = (uint8_t)score;
    if (score > out->ceiling) {
        score = out->ceiling;
    }
    out->score = (uint8_t)score;
    out->band = band_of(out->score);

    switch (out->band) {
    case PV_BAND_FOLLOWING:
        out->headline = "A tracker has been with you across several places";
        break;
    case PV_BAND_PERSISTENT:
        out->headline = "A tracker has stayed with you - keep watching as you move";
        break;
    case PV_BAND_SEEN:
        out->headline = "Trackers nearby, which is ordinary in a public place";
        break;
    case PV_BAND_CLEAR:
    default:
        out->headline = s->n_locales <= 1
                            ? "Nothing following yet - but you have not moved"
                            : "No tracker has followed you between places so far";
        break;
    }
}

/* Rank key for one tag, highest first. Locales dominate because presence in
 * several PLACES is what separates a tracker following you from one that was
 * simply in the cafe; then how long it has been around; then sightings. */
static uint64_t tag_rank(const pv_tag_t *t)
{
    const uint64_t span = (t->last_us > t->first_us) ? (t->last_us - t->first_us) : 0;
    const uint64_t mins = span / 60000000ull;
    return ((uint64_t)t->n_locales << 40) |
           ((mins > 0xFFFFFull ? 0xFFFFFull : mins) << 20) |
           (t->sightings > 0xFFFFFu ? 0xFFFFFu : t->sightings);
}

bool pv_tag_at(const pv_state_t *s, unsigned index, uint64_t now_us,
               pv_tag_t *out, uint32_t *minutes)
{
    (void)now_us;
    if (!s || !out) {
        return false;
    }
    /* Select the index-th best by counting how many rank strictly above each
     * candidate. O(n^2) over 32 entries on a UI repaint, and it needs no
     * scratch array and cannot get its bookkeeping wrong the way a
     * mark-as-consumed scan can - which the first version of this did. Ties
     * break on table position so the order is stable between frames. */
    for (unsigned i = 0; i < PV_MAX_TAGS; i++) {
        if (!s->tags[i].in_use) {
            continue;
        }
        const uint64_t ri = tag_rank(&s->tags[i]);
        unsigned above = 0;
        for (unsigned j = 0; j < PV_MAX_TAGS; j++) {
            if (j == i || !s->tags[j].in_use) {
                continue;
            }
            const uint64_t rj = tag_rank(&s->tags[j]);
            if (rj > ri || (rj == ri && j < i)) {
                above++;
            }
        }
        if (above == index) {
            *out = s->tags[i];
            if (minutes) {
                const uint64_t span = (s->tags[i].last_us > s->tags[i].first_us)
                                          ? (s->tags[i].last_us - s->tags[i].first_us)
                                          : 0;
                *minutes = (uint32_t)(span / 60000000ull);
            }
            return true;
        }
    }
    return false;
}

const char *pv_kind_name(pv_kind_t k)
{
    switch (k) {
    case PV_KIND_FINDMY:      return "Find My";
    case PV_KIND_FINDMY_LOST: return "Find My (separated)";
    case PV_KIND_TILE:        return "Tile";
    case PV_KIND_SMARTTAG:    return "SmartTag";
    case PV_KIND_CHIPOLO:     return "Chipolo";
    case PV_KIND_FLIPPER:     return "Flipper Zero";
    case PV_KIND_SERIAL:      return "BLE serial bridge";
    case PV_KIND_GENERIC:     return "unnamed tracker";
    case PV_KIND_UNKNOWN:
    default:                  return "unknown";
    }
}

const char *pv_band_name(pv_band_t b)
{
    switch (b) {
    case PV_BAND_CLEAR:      return "CLEAR";
    case PV_BAND_SEEN:       return "SEEN";
    case PV_BAND_PERSISTENT: return "PERSISTENT";
    case PV_BAND_FOLLOWING:  return "FOLLOWING";
    default:                 return "?";
    }
}

const char *pv_band_advice(pv_band_t b)
{
    switch (b) {
    case PV_BAND_CLEAR:
        return "No tracker has followed you between places in what has been "
               "heard so far. This is not a clean bill of health: tags that "
               "rotate their address while their owner is nearby are missed, "
               "and a few minutes in one place proves nothing. Keep it running "
               "while you travel if you have a reason to be concerned.";
    case PV_BAND_SEEN:
        return "Trackers are nearby. That is completely ordinary - a busy room "
               "contains several, all in other people's bags. It only becomes "
               "meaningful if one is still here after you have moved.";
    case PV_BAND_PERSISTENT:
        return "Something has stayed with you. Keep moving and keep this "
               "running: another change of place is what separates a tag that "
               "is travelling with you from one that happens to be nearby. If "
               "it is your own, mark it known so it stops being counted.";
    case PV_BAND_FOLLOWING:
        return "A tracker has been present across several distinct places. "
               "That is a fact about radio, not a statement about anybody's "
               "intent - it may be yours, a friend's, or in a parcel you are "
               "carrying. Check those first. If it is none of them: your phone "
               "can usually make an unknown tag play a sound, most trackers "
               "carry a serial number the police can act on, and a report from "
               "this device records what was seen and when. If you believe you "
               "are being followed, contact local police or a support service "
               "rather than confronting the owner.";
    default:
        return "";
    }
}
