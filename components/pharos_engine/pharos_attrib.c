/* Pharos - attribution. See pharos_attrib.h for why this is possible at all.
 *
 * Pure C11, no allocation, no float: this runs on the analytics core and is
 * host-tested like every other judgement in this firmware.
 */
#include "pharos_attrib.h"

#include <string.h>

const char *pat_tool_name(pat_tool_t t)
{
    switch (t) {
    /* ELEVEN CHARACTERS. These are drawn in a row's value column, which is
     * char right[12] - "injector, no sequence" rendered as "injector, n",
     * which names nothing. The long form belongs in the report, not here. */
    case PAT_TOOL_TEMPLATED:   return "templated";
    case PAT_TOOL_MARAUDER:    return "Marauder";
    case PAT_TOOL_NO_SEQUENCE: return "no seq set";
    case PAT_TOOL_MICHAEL:     return "TKIP";
    case PAT_TOOL_UNKNOWN:
    default:                   return "unknown";
    }
}

void pat_reset(pat_engine_t *e)
{
    if (!e) {
        return;
    }
    memset(e, 0, sizeof(*e));
    e->forged_min = 0;
    e->forged_max = -128;
    e->duration_constant = true;
    e->seq_always_zero = true;
    e->seq_constant = true;
}

static bool mac_eq(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

void pat_observe(pat_engine_t *e, const uint8_t mac[6], int8_t rssi)
{
    if (!e || !mac) {
        return;
    }
    for (unsigned i = 0; i < e->n_radios; i++) {
        pat_radio_t *r = &e->radio[i];
        if (!r->used || !mac_eq(r->mac, mac)) {
            continue;
        }
        /* Saturate rather than wrap. A radio that has been talking for an
         * hour must not have its mean destroyed by an overflow. */
        if (r->frames < 0xFFFFu) {
            r->sum_rssi = (int16_t)(r->sum_rssi + rssi);
            r->frames++;
        }
        if (rssi < r->rssi_min) r->rssi_min = rssi;
        if (rssi > r->rssi_max) r->rssi_max = rssi;
        return;
    }
    if (e->n_radios >= PAT_MAX_RADIOS) {
        return; /* the room is already too crowded to attribute anything */
    }
    pat_radio_t *r = &e->radio[e->n_radios++];
    memcpy(r->mac, mac, 6);
    r->sum_rssi = rssi;
    r->frames = 1;
    r->rssi_min = rssi;
    r->rssi_max = rssi;
    r->used = true;
}

void pat_observe_forged(pat_engine_t *e, int8_t rssi, uint16_t seq,
                        uint16_t duration, uint16_t reason)
{
    if (!e) {
        return;
    }
    if (e->forged_n == 0) {
        e->first_duration = duration;
        e->first_seq = seq;
        e->forged_min = rssi;
        e->forged_max = rssi;
    } else {
        if (duration != e->first_duration) e->duration_constant = false;
        if (seq != e->first_seq) e->seq_constant = false;
        if (rssi < e->forged_min) e->forged_min = rssi;
        if (rssi > e->forged_max) e->forged_max = rssi;
    }
    if (seq != 0) {
        e->seq_always_zero = false;
    }
    /* The published Marauder / Evil-M5 pair. Recorded when seen on any single
     * frame rather than required of all of them, because a tool that mixes
     * templates still reveals itself once. */
    if (seq == 0xFFFu && duration == 0x013Au) {
        e->saw_marauder_pair = true;
    }
    e->reason = reason;
    if (e->forged_n < 0xFFFFu) {
        e->forged_sum += rssi;
        e->forged_n++;
    }
}

static pat_tool_t classify(const pat_engine_t *e)
{
    /* Ordered most specific first. A named tool beats the generic tell that
     * also fires for it. */
    if (e->saw_marauder_pair) {
        return PAT_TOOL_MARAUDER;
    }
    if (e->reason == 14u) {
        return PAT_TOOL_MICHAEL;
    }
    /* One frame proves nothing about a constant: everything is constant when
     * there is only one of it. */
    if (e->forged_n < 4u) {
        return PAT_TOOL_UNKNOWN;
    }
    if (e->seq_always_zero) {
        return PAT_TOOL_NO_SEQUENCE;
    }
    if (e->duration_constant) {
        /* A real AP computes this per frame. A constant across a burst means
         * a template - which is the honest limit of what it proves: a tool,
         * not which one. */
        return PAT_TOOL_TEMPLATED;
    }
    return PAT_TOOL_UNKNOWN;
}

void pat_evaluate(const pat_engine_t *e, const uint8_t spoofed[6],
                  pat_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!e || e->forged_n == 0u) {
        return;
    }

    const int32_t mean = e->forged_sum / (int32_t)e->forged_n;
    out->forged_rssi = (int8_t)mean;
    out->forged_n_seen = true;
    out->duration_constant = e->duration_constant && (e->forged_n >= 4u);
    out->first_duration = e->first_duration;
    out->forged_spread = (uint8_t)(e->forged_max - e->forged_min);
    out->tool = classify(e);

    /* Every radio whose mean level falls inside the window, EXCEPT the
     * address the frames claimed to be - matching that would simply be
     * rediscovering the forgery. */
    unsigned hits = 0;
    const pat_radio_t *best = 0;
    int32_t best_delta = 0;

    for (unsigned i = 0; i < e->n_radios; i++) {
        const pat_radio_t *r = &e->radio[i];
        if (!r->used || r->frames == 0u) {
            continue;
        }
        if (spoofed && mac_eq(r->mac, spoofed)) {
            continue;
        }
        /* A radio heard once has no mean worth comparing. */
        if (r->frames < 3u) {
            continue;
        }
        const int32_t rmean = (int32_t)r->sum_rssi / (int32_t)r->frames;
        int32_t d = rmean - mean;
        if (d < 0) d = -d;
        if (d > PAT_RSSI_WINDOW) {
            continue;
        }
        hits++;
        if (!best || d < best_delta) {
            best = r;
            best_delta = d;
        }
    }

    out->candidates = hits;

    /* UNIQUENESS OR NOTHING. Six devices at -45 dBm give six equally good
     * answers, which is the same as none - and naming the first would be an
     * accusation manufactured from a coincidence. */
    if (hits != 1u || !best) {
        out->ambiguous = (hits > 1u);
        return;
    }

    out->have_lead = true;
    memcpy(out->lead_mac, best->mac, 6);

    /* Confidence falls with the gap and with how much the forged level itself
     * wandered - a transmitter whose own readings spread 12 dB is not
     * something to match anybody against. Capped well short of certain. */
    int32_t c = PAT_MAX_CONFIDENCE;
    c -= best_delta * 8;
    c -= (int32_t)out->forged_spread * 2;
    if (e->forged_n < 8u) {
        c -= 15; /* a short burst is a thin sample */
    }
    if (c < 0) c = 0;
    if (c > PAT_MAX_CONFIDENCE) c = PAT_MAX_CONFIDENCE;
    out->confidence = (uint8_t)c;

    /* A lead nobody should act on is not a lead. */
    if (out->confidence < 25u) {
        out->have_lead = false;
    }
}
