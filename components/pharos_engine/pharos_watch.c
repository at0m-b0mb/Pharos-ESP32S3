#include "pharos_watch.h"

#include <string.h>

/* ---- small helpers -------------------------------------------------- */

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static bool mac_is_broadcast(const uint8_t *a)
{
    return a[0] == 0xFF && a[1] == 0xFF && a[2] == 0xFF &&
           a[3] == 0xFF && a[4] == 0xFF && a[5] == 0xFF;
}

static bool mac_is_zero(const uint8_t *a)
{
    for (int i = 0; i < 6; i++) {
        if (a[i]) {
            return false;
        }
    }
    return true;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Piecewise-linear ramp over an ascending xs[] table. */
static uint32_t interp(uint32_t x, const uint32_t *xs, const uint32_t *ys, unsigned n)
{
    if (x <= xs[0]) {
        return ys[0];
    }
    for (unsigned i = 1; i < n; i++) {
        if (x <= xs[i]) {
            const uint32_t span = xs[i] - xs[i - 1];
            const uint32_t rise = ys[i] - ys[i - 1];
            return ys[i - 1] + (span ? ((x - xs[i - 1]) * rise) / span : 0);
        }
    }
    return ys[n - 1];
}

/* ---- AP table ------------------------------------------------------- */

static pw_ap_t *ap_lookup(pw_engine_t *e, const uint8_t *bssid)
{
    for (unsigned i = 0; i < PW_MAX_AP; i++) {
        if (e->aps[i].in_use && mac_eq(e->aps[i].bssid, bssid)) {
            return &e->aps[i];
        }
    }
    return NULL;
}

static pw_ap_t *ap_admit(pw_engine_t *e, const uint8_t *bssid, uint64_t t_us)
{
    pw_ap_t *slot = ap_lookup(e, bssid);
    if (slot) {
        return slot;
    }
    for (unsigned i = 0; i < PW_MAX_AP; i++) {
        if (!e->aps[i].in_use) {
            slot = &e->aps[i];
            break;
        }
    }
    if (!slot) {
        /* Evict the least recently heard. Losing an AP weakens the identity
         * family for that BSSID, which is why the count is surfaced rather
         * than swallowed. */
        pw_ap_t *oldest = &e->aps[0];
        for (unsigned i = 1; i < PW_MAX_AP; i++) {
            if (e->aps[i].last_beacon_us < oldest->last_beacon_us) {
                oldest = &e->aps[i];
            }
        }
        slot = oldest;
        e->evicted_aps++;
    }
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->bssid, bssid, 6);
    slot->in_use = true;
    slot->last_beacon_us = t_us;
    return slot;
}

/* ---- ingest --------------------------------------------------------- */

void pw_reset(pw_engine_t *e)
{
    if (e) {
        memset(e, 0, sizeof(*e));
    }
}

void pw_observe(pw_engine_t *e, const pharos_ev_dot11_t *f, uint64_t t_us)
{
    if (!e || !f || f->type != PHAROS_FT_MGMT) {
        return;
    }
    if (e->total_frames == 0) {
        e->first_us = t_us;
    }
    e->total_frames++;
    e->last_us = t_us;

    if (f->subtype == PHAROS_ST_BEACON || f->subtype == PHAROS_ST_PROBE_RESP) {
        if (mac_is_zero(f->a2) || mac_is_broadcast(f->a2)) {
            return;
        }
        pw_ap_t *ap = ap_admit(e, f->a2, t_us);
        ap->channel = f->channel;
        ap->beacons++;
        ap->last_beacon_us = t_us;
        if (f->flags & PHAROS_DOT11_F_MFP_SEEN) {
            ap->mfp_capable = true;
        }
        /* EWMA at 1/4 weight, in quarter-dBm, so one frame cannot drag the
         * average far - the identity test depends on this being stable. */
        const int16_t sample_x4 = (int16_t)((int16_t)f->rssi * 4);
        ap->rssi_ewma_x4 = (ap->beacons == 1)
                               ? sample_x4
                               : (int16_t)(ap->rssi_ewma_x4 +
                                           (sample_x4 - ap->rssi_ewma_x4) / 4);
        return;
    }

    if (f->subtype != PHAROS_ST_DEAUTH && f->subtype != PHAROS_ST_DISASSOC) {
        return;
    }

    /* Rate is counted in per-second buckets rather than by walking the hit
     * ring, so a flood large enough to overflow the ring still produces a
     * truthful rate. The ring only ever answers "what shape". */
    const uint32_t sec = (uint32_t)(t_us / 1000000ull);
    pw_bucket_t *b = &e->rate[sec % PW_RATE_BUCKETS];
    if (b->sec != sec) {
        b->sec = sec;
        b->count = 0;
    }
    b->count++;
    e->hit_total++;

    pw_hit_t *h = &e->hits[e->hit_head];
    h->t_us = t_us;
    memcpy(h->src, f->a2, 6);
    memcpy(h->dst, f->a1, 6);
    h->reason = f->reason_or_status;
    h->rssi = f->rssi;
    h->channel = f->channel;

    e->hit_head = (uint16_t)((e->hit_head + 1) % PW_MAX_EVENTS);
    if (e->hit_count < PW_MAX_EVENTS) {
        e->hit_count++;
    }
}

/* ---- scoring -------------------------------------------------------- */

uint8_t pw_ceiling(const pw_context_t *ctx)
{
    const uint32_t dwell = clamp_u32(ctx ? ctx->dwell_permil : 1000, 1, 1000);
    const uint32_t yield = clamp_u32(ctx ? ctx->bus_yield_permil : 1000, 1, 1000);

    /* 58 when the channel is barely visited, 96 when camped. Nothing this
     * device observes earns more than 96: one antenna, one receiver, and no
     * way to rule out a transmitter it simply did not hear. */
    uint32_t c = 58u + (38u * dwell) / 1000u;

    uint32_t pen = (yield >= 900u) ? 0u : (900u - yield) / 40u;
    if (pen > 15u) {
        pen = 15u;
    }
    c = (c > pen) ? c - pen : 0u;
    return (uint8_t)clamp_u32(c, 45u, 96u);
}

static pw_band_t band_of(uint8_t score)
{
    if (score >= 75) return PW_BAND_LIKELY;
    if (score >= 60) return PW_BAND_SUSPICIOUS;
    if (score >= 40) return PW_BAND_ELEVATED;
    if (score >= 20) return PW_BAND_BACKGROUND;
    return PW_BAND_QUIET;
}

void pw_evaluate(const pw_engine_t *e, uint64_t now_us, const pw_context_t *ctx,
                 pw_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!e || !ctx) {
        return;
    }

    uint32_t window_ms = ctx->window_ms ? ctx->window_ms : 10000u;
    if (window_ms > PW_RATE_BUCKETS * 1000u) {
        window_ms = PW_RATE_BUCKETS * 1000u;
    }
    const uint64_t window_us = (uint64_t)window_ms * 1000ull;
    const uint64_t start_us = (now_us > window_us) ? (now_us - window_us) : 0ull;

    out->ceiling = pw_ceiling(ctx);

    const uint32_t dwell = clamp_u32(ctx->dwell_permil, 1, 1000);
    const uint32_t yield = clamp_u32(ctx->bus_yield_permil, 1, 1000);
    if (dwell < 500) out->notes |= PW_NOTE_THIN_DWELL;
    if (yield < 950) out->notes |= PW_NOTE_LOSSY;

    /* --- windowed count from the per-second buckets -------------------- */
    const uint32_t now_sec = (uint32_t)(now_us / 1000000ull);
    uint32_t win_s = (window_ms + 999u) / 1000u;
    if (win_s == 0) win_s = 1;
    if (win_s > PW_RATE_BUCKETS) win_s = PW_RATE_BUCKETS;

    uint32_t counted = 0;
    for (unsigned i = 0; i < PW_RATE_BUCKETS; i++) {
        const pw_bucket_t *b = &e->rate[i];
        if (b->count == 0) continue;
        if (b->sec > now_sec) continue;
        if ((now_sec - b->sec) < win_s) {
            counted += b->count;
        }
    }
    out->observed = counted;

    /* --- shape, from the most recent frames in the window --------------- */
    uint8_t victims[PW_MAX_VICTIMS][6];
    uint8_t sources[PW_MAX_VICTIMS][6];
    unsigned n_victims = 0, n_sources = 0;
    uint32_t sample = 0, broadcast = 0;
    uint16_t reasons[8];
    uint16_t reason_hits[8];
    unsigned n_reasons = 0;

    for (unsigned i = 0; i < e->hit_count; i++) {
        const unsigned idx =
            (unsigned)((e->hit_head + PW_MAX_EVENTS - 1 - i) % PW_MAX_EVENTS);
        const pw_hit_t *h = &e->hits[idx];
        if (h->t_us < start_us) {
            break; /* the ring is time-ordered; everything past here is older */
        }
        sample++;
        if (mac_is_broadcast(h->dst)) {
            broadcast++;
        } else {
            bool seen = false;
            for (unsigned v = 0; v < n_victims; v++) {
                if (mac_eq(victims[v], h->dst)) { seen = true; break; }
            }
            if (!seen && n_victims < PW_MAX_VICTIMS) {
                memcpy(victims[n_victims++], h->dst, 6);
            }
        }
        bool src_seen = false;
        for (unsigned s = 0; s < n_sources; s++) {
            if (mac_eq(sources[s], h->src)) { src_seen = true; break; }
        }
        if (!src_seen && n_sources < PW_MAX_VICTIMS) {
            memcpy(sources[n_sources++], h->src, 6);
        }
        bool r_seen = false;
        for (unsigned r = 0; r < n_reasons; r++) {
            if (reasons[r] == h->reason) { reason_hits[r]++; r_seen = true; break; }
        }
        if (!r_seen && n_reasons < 8) {
            reasons[n_reasons] = h->reason;
            reason_hits[n_reasons] = 1;
            n_reasons++;
        }
    }

    out->shape_sample = sample;
    out->distinct_victims = (uint8_t)n_victims;
    out->distinct_sources = (uint8_t)n_sources;
    out->broadcast_permil = sample ? (uint16_t)((broadcast * 1000u) / sample) : 0u;
    if (counted > sample) {
        out->notes |= PW_NOTE_SAMPLED;
    }

    if (counted == 0 && sample == 0) {
        out->band = PW_BAND_QUIET;
        return;
    }

    /* Dominant transmitter, and its mean received level. */
    uint32_t dom_hits = 0;
    int32_t dom_rssi_sum = 0;
    for (unsigned s = 0; s < n_sources; s++) {
        uint32_t hits = 0;
        int32_t rssi_sum = 0;
        for (unsigned i = 0; i < e->hit_count; i++) {
            const unsigned idx =
                (unsigned)((e->hit_head + PW_MAX_EVENTS - 1 - i) % PW_MAX_EVENTS);
            const pw_hit_t *h = &e->hits[idx];
            if (h->t_us < start_us) break;
            if (mac_eq(h->src, sources[s])) { hits++; rssi_sum += h->rssi; }
        }
        if (hits > dom_hits) {
            dom_hits = hits;
            dom_rssi_sum = rssi_sum;
            memcpy(out->src, sources[s], 6);
        }
    }
    const int32_t dom_rssi_avg = dom_hits ? (dom_rssi_sum / (int32_t)dom_hits) : 0;

    uint16_t best_reason_hits = 0;
    for (unsigned r = 0; r < n_reasons; r++) {
        if (reason_hits[r] > best_reason_hits) {
            best_reason_hits = reason_hits[r];
            out->dominant_reason = reasons[r];
        }
    }
    out->dominant_reason_pct =
        sample ? (uint8_t)clamp_u32((uint32_t)best_reason_hits * 100u / sample, 0, 100) : 0;

    /* --- elapsed and rate ---------------------------------------------- */
    uint32_t elapsed_ms = window_ms;
    if (e->last_us > e->first_us) {
        const uint64_t span_ms = (e->last_us - e->first_us) / 1000ull;
        if (span_ms < (uint64_t)elapsed_ms) {
            elapsed_ms = (uint32_t)(span_ms ? span_ms : 1u);
        }
    }
    if (elapsed_ms < 2000u) {
        out->notes |= PW_NOTE_SHORT_WINDOW;
    }

    /* Duty correction: scale what we heard by the share of the channel we
     * were listening to and the share of frames the bus kept. This is an
     * extrapolation; the ceiling above is what keeps it honest. */
    {
        const uint64_t num =
            (uint64_t)counted * 100ull * 1000ull * 1000ull * 1000ull;
        const uint64_t den =
            (uint64_t)elapsed_ms * (uint64_t)dwell * (uint64_t)yield;
        out->est_per_s_x100 = (uint32_t)(den ? (num / den) : 0ull);
    }

    static const uint32_t rate_x[] = { 20, 100, 300, 1000, 3000, 10000 };
    static const uint32_t rate_y[] = { 0, 8, 16, 26, 34, 40 };
    out->c_rate = (uint8_t)interp(out->est_per_s_x100, rate_x, rate_y, 6);

    /* --- targeting shape ------------------------------------------------ */
    uint32_t target = ((uint32_t)out->broadcast_permil * 14u) / 1000u;
    if (n_victims >= 5) {
        target += 8;
    } else if (n_victims >= 2) {
        target += 4;
    }
    out->c_target = (uint8_t)clamp_u32(target, 0, 22);

    /* --- identity -------------------------------------------------------- */
    const pw_ap_t *claimed = ap_lookup((pw_engine_t *)e, out->src);
    uint32_t identity = 0;
    if (!claimed) {
        /* Never heard this BSSID beacon: suggestive of a forged source. But
         * while hopping we very plausibly just missed the beacons, so the
         * points are scaled by how much of the channel we actually heard. */
        identity = (14u * dwell) / 1000u;
    } else if (claimed->beacons >= 3) {
        const int32_t beacon_rssi = claimed->rssi_ewma_x4 / 4;
        int32_t delta = beacon_rssi - dom_rssi_avg;
        if (delta < 0) delta = -delta;
        out->rssi_delta = (int8_t)clamp_u32((uint32_t)delta, 0, 127);
        if (delta >= 10) {
            /* The frames claim an AP we can hear, but arrive at a materially
             * different level: two transmitters wearing one address. */
            identity = 12u + clamp_u32((uint32_t)(delta - 10), 0, 10);
        }
        if (claimed->mfp_capable) {
            out->notes |= PW_NOTE_MFP_TARGET;
        }
    }
    out->c_identity = (uint8_t)clamp_u32(identity, 0, 22);

    /* --- reason-code monoculture (a modifier, never a family) ------------ */
    uint32_t reason_pts = 0;
    if (sample >= 8 && out->dominant_reason_pct >= 90) {
        reason_pts = 8;
        if (out->dominant_reason == 7 || out->dominant_reason == 1) {
            reason_pts += 4; /* class-3-frame / unspecified: the tool defaults */
        }
    }
    out->c_reason = (uint8_t)clamp_u32(reason_pts, 0, 12);

    /* --- families -------------------------------------------------------- */
    if (out->c_rate >= 12) out->families |= PW_FAM_RATE;
    if (out->c_target >= 8) out->families |= PW_FAM_TARGET;
    if (out->c_identity >= 10) out->families |= PW_FAM_IDENTITY;

    uint32_t raw = (uint32_t)out->c_rate + out->c_target + out->c_identity + out->c_reason;
    raw = clamp_u32(raw, 0, 100);
    out->raw_score = (uint8_t)raw;

    /* --- caps ------------------------------------------------------------ */
    uint32_t score = raw;

    unsigned family_count = 0;
    for (unsigned b = 0; b < 3; b++) {
        if (out->families & (1u << b)) family_count++;
    }
    /* One loud signal is not a case: a single family cannot leave ELEVATED,
     * however extreme the reading. (Two families cannot exceed 74 by
     * arithmetic, so the alarm band requires all three to agree.) */
    if (family_count < 2 && score > 49) {
        score = 49;
    }
    /* A blink is not a rate. */
    if ((out->notes & PW_NOTE_SHORT_WINDOW) && score > 49) {
        score = 49;
    }
    if (score > out->ceiling) {
        score = out->ceiling;
    }

    out->score = (uint8_t)score;
    out->band = band_of(out->score);
}

const char *pw_band_name(pw_band_t band)
{
    switch (band) {
    case PW_BAND_QUIET:      return "QUIET";
    case PW_BAND_BACKGROUND: return "BACKGROUND";
    case PW_BAND_ELEVATED:   return "ELEVATED";
    case PW_BAND_SUSPICIOUS: return "SUSPICIOUS";
    case PW_BAND_LIKELY:     return "FLOOD LIKELY";
    default:                 return "?";
    }
}

const char *pw_band_advice(pw_band_t band)
{
    switch (band) {
    case PW_BAND_QUIET:
        return "No disconnect traffic in view. This receiver hears one channel "
               "at a time - quiet here is not quiet everywhere.";
    case PW_BAND_BACKGROUND:
        return "Deauthentication at levels healthy networks produce. Roaming "
               "and idle timeouts look like this.";
    case PW_BAND_ELEVATED:
        return "More disconnect traffic than housekeeping explains. Camp on "
               "this channel to sharpen the reading.";
    case PW_BAND_SUSPICIOUS:
        return "The shape looks like an attack but the evidence is thin. Stop "
               "hopping and camp on this channel to raise the ceiling.";
    case PW_BAND_LIKELY:
        return "Sustained, broadly targeted deauthentication from a source "
               "that does not match the network it claims. Preserve the log.";
    default:
        return "";
    }
}
