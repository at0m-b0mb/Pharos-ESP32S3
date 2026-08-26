/* Pharos - clock skew. See pharos_skew.h for why an oscillator cannot be
 * forged the way a BSSID can, and for what this deliberately refuses to claim.
 *
 * Integer only, like every other engine here: the fit is a two-point slope in
 * parts per million over int64 microseconds, which needs no float and cannot
 * drift with one.
 */
#include "pharos_skew.h"

#include <string.h>

void psk_reset(psk_engine_t *e)
{
    if (e) {
        memset(e, 0, sizeof(*e));
    }
}

static psk_ap_t *find(psk_engine_t *e, const uint8_t bssid[6])
{
    for (unsigned i = 0; i < e->n; i++) {
        if (e->ap[i].used && memcmp(e->ap[i].bssid, bssid, 6) == 0) {
            return &e->ap[i];
        }
    }
    if (e->n >= PSK_MAX_APS) {
        return 0;
    }
    psk_ap_t *a = &e->ap[e->n++];
    memset(a, 0, sizeof(*a));
    memcpy(a->bssid, bssid, 6);
    a->used = true;
    return a;
}

static const psk_ap_t *find_const(const psk_engine_t *e, const uint8_t bssid[6])
{
    for (unsigned i = 0; i < e->n; i++) {
        if (e->ap[i].used && memcmp(e->ap[i].bssid, bssid, 6) == 0) {
            return &e->ap[i];
        }
    }
    return 0;
}

/* Slope of (tsf - local) against local, in parts per million.
 *
 * Uses the OLDEST and NEWEST buckets rather than a least-squares fit over all
 * of them. That is not laziness: each bucket already holds the minimum of its
 * second, so the endpoints are the two most reliable numbers available, and a
 * regression would pull them back toward the jittery middle. The span is what
 * buys the resolution, so the two furthest-apart points are exactly the ones
 * worth using.
 */
static bool fit_ppm(const psk_ap_t *a, int32_t *ppm, uint64_t *span_us,
                    unsigned *samples)
{
    int oldest = -1, newest = -1;
    unsigned n = 0;
    for (unsigned i = 0; i < PSK_SLOTS; i++) {
        if (!a->slot_used[i]) {
            continue;
        }
        n++;
        if (oldest < 0 || a->slot_local[i] < a->slot_local[oldest]) {
            oldest = (int)i;
        }
        if (newest < 0 || a->slot_local[i] > a->slot_local[newest]) {
            newest = (int)i;
        }
    }
    if (samples) {
        *samples = n;
    }
    if (oldest < 0 || newest < 0 || oldest == newest) {
        return false;
    }

    const uint64_t span = a->slot_local[newest] - a->slot_local[oldest];
    if (span_us) {
        *span_us = span;
    }
    if (n < PSK_MIN_SAMPLES || span < PSK_MIN_SPAN_US) {
        return false;
    }

    const int64_t d_off = a->min_off[newest] - a->min_off[oldest];
    /* ppm = drift / elapsed * 1e6. Both are microseconds and span is at least
     * twenty seconds, so this cannot divide by zero and cannot overflow: the
     * drift over any plausible window is far inside int64 even multiplied by
     * a million. */
    const int64_t p = (d_off * 1000000) / (int64_t)span;
    if (ppm) {
        /* A number outside this range is not a crystal, it is a clock that
         * was reset or a frame from a different network entirely. Clamped so
         * one bad sample cannot masquerade as a dramatic finding. */
        int64_t c = p;
        if (c > 100000) c = 100000;
        if (c < -100000) c = -100000;
        *ppm = (int32_t)c;
    }
    return true;
}

void psk_observe(psk_engine_t *e, const uint8_t bssid[6], uint64_t tsf,
                 uint64_t local_us)
{
    if (!e || !bssid || tsf == 0u || local_us == 0u) {
        return;
    }
    psk_ap_t *a = find(e, bssid);
    if (!a) {
        return;
    }

    const uint64_t bucket = local_us / (1000000ull * PSK_BUCKET_S);
    const unsigned idx = (unsigned)(bucket % PSK_SLOTS);
    const int64_t off = (int64_t)tsf - (int64_t)local_us;

    /* A new bucket takes the slot over; within one bucket the smallest offset
     * wins. See the note in the header on why the minimum. */
    const uint64_t held = a->slot_local[idx] / (1000000ull * PSK_BUCKET_S);
    if (!a->slot_used[idx] || held != bucket) {
        a->slot_used[idx] = true;
        a->slot_local[idx] = local_us;
        a->min_off[idx] = off;
        if (a->slot_n < PSK_SLOTS) {
            a->slot_n++;
        }
    } else if (off < a->min_off[idx]) {
        a->min_off[idx] = off;
        a->slot_local[idx] = local_us;
    }

    int32_t ppm = 0;
    uint64_t span = 0;
    if (!fit_ppm(a, &ppm, &span, 0)) {
        return;
    }
    a->last_ppm = ppm;
    a->have_last = true;

    if (!a->have_base) {
        a->base_ppm = ppm;
        a->have_base = true;
        return;
    }

    int32_t d = ppm - a->base_ppm;
    if (d < 0) {
        d = -d;
    }
    if (d >= PSK_CHANGE_PPM && !a->changed) {
        /* THE FINDING. A crystal does not change rate; a radio does. */
        a->changed = true;
        a->change_from = a->base_ppm;
        a->change_to = ppm;
    }
}

void psk_evaluate(const psk_engine_t *e, const uint8_t bssid[6],
                  psk_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!e || !bssid) {
        return;
    }
    const psk_ap_t *a = find_const(e, bssid);
    if (!a) {
        return;
    }

    int32_t ppm = 0;
    uint64_t span = 0;
    unsigned n = 0;
    const bool ok = fit_ppm(a, &ppm, &span, &n);

    out->samples = n;
    out->span_s = (uint32_t)(span / 1000000ull);
    /* Seen, but not for long enough to say anything. Reported as measuring
     * rather than as "no finding", because those are different and only one
     * of them invites you to wait. */
    out->measuring = (n > 0) && !ok;
    out->have_skew = ok;
    if (ok) {
        out->ppm = ppm;
    }
    out->changed = a->changed;
    out->from_ppm = a->change_from;
    out->to_ppm = a->change_to;
}

bool psk_any_changed(const psk_engine_t *e)
{
    if (!e) {
        return false;
    }
    for (unsigned i = 0; i < e->n; i++) {
        if (e->ap[i].used && e->ap[i].changed) {
            return true;
        }
    }
    return false;
}

bool psk_first_changed(const psk_engine_t *e, psk_verdict_t *out,
                       uint8_t bssid[6])
{
    if (!e) {
        return false;
    }
    for (unsigned i = 0; i < e->n; i++) {
        if (!e->ap[i].used || !e->ap[i].changed) {
            continue;
        }
        if (bssid) {
            memcpy(bssid, e->ap[i].bssid, 6);
        }
        if (out) {
            psk_evaluate(e, e->ap[i].bssid, out);
        }
        return true;
    }
    return false;
}

void psk_progress(const psk_engine_t *e, unsigned *ready, unsigned *measuring)
{
    unsigned r = 0, m = 0;
    if (e) {
        for (unsigned i = 0; i < e->n; i++) {
            if (!e->ap[i].used) {
                continue;
            }
            psk_verdict_t v;
            psk_evaluate(e, e->ap[i].bssid, &v);
            if (v.have_skew) r++;
            else if (v.measuring) m++;
        }
    }
    if (ready) *ready = r;
    if (measuring) *measuring = m;
}
