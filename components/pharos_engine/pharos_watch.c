/* Pharos - the deauthentication watch engine (v2). See pharos_watch.h for
 * the design and for why v1 was replaced rather than tuned. */
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

static int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

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

/* ---- 802.11 sequence arithmetic -------------------------------------
 *
 * The counter is 12 bits and wraps. ahead(a, b) is how far b sits in front of
 * a going forwards, 0..4095; a value above 2048 is more sensibly read as b
 * sitting BEHIND a. Everything below works in this space so that a wrap is
 * never mistaken for a jump. */
static uint16_t seq_ahead(uint16_t a, uint16_t b)
{
    return (uint16_t)((b - a) & 0x0FFFu);
}

/* Could a transmitter whose counter was at `base` legitimately have reached
 * `value` after advancing at most `bound` steps?
 *
 * The test is ORDER, not gap size. Frames this receiver never heard widen the
 * gap between what we saw and what the transmitter actually sent - so a large
 * forward gap proves nothing and is not treated as evidence. What missed
 * frames can never do is make the counter go BACKWARDS. That asymmetry is the
 * whole test, and it is why this does not inherit the false-positive rate that
 * threshold-on-gap-size schemes are documented to have.
 *
 * `bound` must stay well clear of a full wrap or the question is meaningless:
 * once a transmitter could plausibly have advanced 4096 steps, every value is
 * reachable and the caller must not ask. */
static bool seq_plausible(uint16_t base, uint16_t value, uint32_t bound)
{
    if (bound >= 3000u) {
        return true; /* uninformative: refuse to draw a conclusion */
    }
    return (uint32_t)seq_ahead(base, value) <= bound;
}

/* ---- AP table ------------------------------------------------------- */

static pw_ap_t *ap_lookup(const pw_engine_t *e, const uint8_t *bssid)
{
    for (unsigned i = 0; i < PW_MAX_AP; i++) {
        if (e->aps[i].in_use && mac_eq(e->aps[i].bssid, bssid)) {
            return (pw_ap_t *)&e->aps[i];
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
        /* Evict the least recently heard. Losing an AP weakens every forgery
         * test for that BSSID, which is why the count is surfaced rather than
         * swallowed. */
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

/* ---- per-second slots ----------------------------------------------- */

static pw_slot_t *slot_for(pw_engine_t *e, uint32_t sec)
{
    pw_slot_t *s = &e->slots[sec % PW_WINDOW_SLOTS];
    if (s->sec != sec) {
        s->sec = sec;
        s->disconnects = 0;
        s->rejoins = 0;
    }
    return s;
}

/* ---- ingest --------------------------------------------------------- */

void pw_reset(pw_engine_t *e)
{
    if (e) {
        memset(e, 0, sizeof(*e));
    }
}

/* The AP's counter rate, in steps per second, held as a decaying peak. A peak
 * rather than a mean because it is used to build an upper bound: an access
 * point that was briefly busy can still be briefly busy again. */
static void ap_note_seq_rate(pw_ap_t *ap, uint16_t new_seq, uint64_t t_us)
{
    if (ap->beacons < 1 || ap->last_beacon_us == 0 || t_us <= ap->last_beacon_us) {
        return;
    }
    const uint32_t dt_ms = (uint32_t)((t_us - ap->last_beacon_us) / 1000ull);
    if (dt_ms < 10u || dt_ms > 5000u) {
        return; /* too short to measure, or too long to attribute */
    }
    const uint16_t adv = seq_ahead(ap->last_beacon_seq, new_seq);
    if (adv > 2048u) {
        return; /* the AP's own counter went backwards: not a rate sample */
    }
    uint32_t per_s = ((uint32_t)adv * 1000u) / dt_ms;
    if (per_s > 4000u) {
        per_s = 4000u;
    }
    /* Peak-hold with a slow decay. A peak rather than a mean because this feeds
     * an UPPER bound: an access point that was busy a moment ago can be busy
     * again, and a bound that tracked the mean would accuse it of forgery the
     * next time it had traffic to carry. */
    if (per_s > ap->seq_rate_peak) {
        ap->seq_rate_peak = (uint16_t)per_s;
    } else {
        ap->seq_rate_peak = (uint16_t)(ap->seq_rate_peak - ap->seq_rate_peak / 8u);
    }
}

static uint32_t ap_seq_bound(const pw_ap_t *ap, uint32_t elapsed_ms)
{
    /* Four times the observed peak, plus a fixed allowance so that an idle AP
     * (peak 0) still gets room for the handful of frames it can emit between a
     * beacon and a disconnect. */
    const uint32_t rate = ap->seq_rate_peak;
    return ((rate * elapsed_ms) / 1000u) * 4u + 96u;
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

    const uint32_t sec = (uint32_t)(t_us / 1000000ull);

    /* --- beacons and probe responses: this is how we learn who is real --- */
    if (f->subtype == PHAROS_ST_BEACON || f->subtype == PHAROS_ST_PROBE_RESP) {
        if (mac_is_zero(f->a2) || mac_is_broadcast(f->a2)) {
            return;
        }
        pw_ap_t *ap = ap_admit(e, f->a2, t_us);

        /* THE FORWARD HALF OF THE ORDER TEST.
         *
         * If disconnect frames claiming this BSSID arrived since its last
         * beacon, the AP's counter must by now have caught up with the
         * furthest of them - it is the same counter. A beacon that arrives
         * BEHIND a disconnect we already attributed to this AP means the
         * disconnect was never on this counter at all. */
        if (ap->pending_valid && ap->beacons > 0) {
            const uint32_t elapsed_ms =
                (uint32_t)((t_us - ap->last_beacon_us) / 1000ull);
            const uint32_t bound = ap_seq_bound(ap, elapsed_ms);
            if (!seq_plausible(ap->pending_seq, f->seq, bound) &&
                seq_ahead(ap->pending_seq, f->seq) > 2048u) {
                if (ap->seq_fwd < 0xFFFFu) {
                    ap->seq_fwd++;
                }
            }
        }
        ap->pending_valid = false;

        ap_note_seq_rate(ap, f->seq, t_us);

        ap->channel = f->channel;
        ap->last_beacon_us = t_us;
        ap->last_beacon_seq = f->seq;
        ap->rsn_flags |= f->rsn_flags;
        if (f->ssid_len && ap->ssid_len == 0) {
            const uint8_t n = f->ssid_len > PHAROS_EV_SSID_MAX
                                  ? PHAROS_EV_SSID_MAX : f->ssid_len;
            memcpy(ap->ssid, f->ssid, n);
            ap->ssid_len = n;
        }

        /* Level, and how much it normally moves. The deviation matters as much
         * as the mean: a flat threshold calls a fading distant AP a forgery
         * and lets a stable near one off. Both in quarter-dBm. */
        const int16_t sample_x4 = (int16_t)((int16_t)f->rssi * 4);
        if (ap->beacons == 0) {
            ap->rssi_ewma_x4 = sample_x4;
            ap->rssi_dev_x4 = 0;
        } else {
            const int16_t err = (int16_t)(sample_x4 - ap->rssi_ewma_x4);
            ap->rssi_ewma_x4 = (int16_t)(ap->rssi_ewma_x4 + err / 4);
            const int16_t aerr = (int16_t)(err < 0 ? -err : err);
            ap->rssi_dev_x4 = (int16_t)(ap->rssi_dev_x4 + (aerr - ap->rssi_dev_x4) / 4);
        }
        if (ap->beacons < 0xFFFFu) {
            ap->beacons++;
        }
        return;
    }

    /* --- the rejoin stampede: did anybody actually get knocked off? ---- */
    if (f->subtype == PHAROS_ST_AUTH || f->subtype == PHAROS_ST_ASSOC_REQ ||
        f->subtype == PHAROS_ST_REASSOC_REQ) {
        slot_for(e, sec)->rejoins++;
        return;
    }

    if (f->subtype != PHAROS_ST_DEAUTH && f->subtype != PHAROS_ST_DISASSOC) {
        return;
    }

    /* --- a disconnect frame -------------------------------------------- */

    /* Counted in per-second slots rather than by walking the hit ring, so a
     * flood large enough to overflow the ring still produces a truthful rate.
     * The ring only ever answers "what shape". */
    slot_for(e, sec)->disconnects++;
    e->hit_total++;

    /* THE BACKWARD HALF OF THE ORDER TEST. A frame claiming this BSSID whose
     * counter sits behind a beacon we already heard from it did not come from
     * that transmitter. Guarded by ap_seq_bound so a busy AP - whose counter
     * runs fast on data frames we never see - is never accused. */
    pw_ap_t *claimed = ap_lookup(e, f->a2);
    if (claimed && claimed->beacons >= 2 && t_us > claimed->last_beacon_us) {
        const uint32_t elapsed_ms =
            (uint32_t)((t_us - claimed->last_beacon_us) / 1000ull);
        const uint32_t bound = ap_seq_bound(claimed, elapsed_ms);
        if (!seq_plausible(claimed->last_beacon_seq, f->seq, bound)) {
            if (claimed->seq_back < 0xFFFFu) {
                claimed->seq_back++;
            }
        }
        /* Remember the furthest-ahead claim for the forward half above. */
        if (!claimed->pending_valid ||
            seq_ahead(claimed->pending_seq, f->seq) < 2048u) {
            claimed->pending_seq = f->seq;
            claimed->pending_valid = true;
        }
    }

    pw_hit_t *h = &e->hits[e->hit_head];
    h->t_us = t_us;
    memcpy(h->src, f->a2, 6);
    memcpy(h->dst, f->a1, 6);
    h->reason = f->reason_or_status;
    h->seq = f->seq;
    h->rssi = f->rssi;
    h->channel = f->channel;
    h->flags = f->flags;

    e->hit_head = (uint16_t)((e->hit_head + 1) % PW_MAX_HITS);
    if (e->hit_count < PW_MAX_HITS) {
        e->hit_count++;
    }
}

/* ---- ceiling -------------------------------------------------------- */

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
    return (uint8_t)clamp_u32(c, 45u, PW_CEILING_MAX);
}

static pw_band_t band_of(uint8_t score)
{
    if (score >= 75) return PW_BAND_LIKELY;
    if (score >= 60) return PW_BAND_SUSPICIOUS;
    if (score >= 40) return PW_BAND_ELEVATED;
    if (score >= 20) return PW_BAND_BACKGROUND;
    return PW_BAND_QUIET;
}

/* ---- pressure channel ------------------------------------------------ */

uint8_t pw_pressure_channel(const pw_engine_t *e, uint64_t now_us,
                            uint32_t window_ms)
{
    if (!e || e->hit_count == 0) {
        return 0;
    }
    const uint64_t window_us = (uint64_t)(window_ms ? window_ms : 10000u) * 1000ull;
    const uint64_t start_us = (now_us > window_us) ? (now_us - window_us) : 0ull;

    uint16_t per_chan[PW_MAX_AP > 15 ? 15 : 15] = { 0 };
    for (unsigned i = 0; i < e->hit_count; i++) {
        const unsigned idx =
            (unsigned)((e->hit_head + PW_MAX_HITS - 1 - i) % PW_MAX_HITS);
        const pw_hit_t *h = &e->hits[idx];
        if (h->t_us < start_us) {
            break;
        }
        if (h->channel >= 1 && h->channel <= 14) {
            per_chan[h->channel]++;
        }
    }
    uint8_t best = 0;
    uint16_t best_n = 0;
    for (uint8_t c = 1; c <= 14; c++) {
        if (per_chan[c] > best_n) {
            best_n = per_chan[c];
            best = c;
        }
    }
    return best_n ? best : 0;
}

void pw_history(const pw_engine_t *e, uint64_t now_us,
                uint16_t out[PW_WINDOW_SLOTS])
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(uint16_t) * PW_WINDOW_SLOTS);
    if (!e) {
        return;
    }
    const uint32_t now_sec = (uint32_t)(now_us / 1000000ull);
    for (unsigned i = 0; i < PW_WINDOW_SLOTS; i++) {
        /* out[0] is the oldest second in the window, out[n-1] is now. A slot
         * whose stamp does not match the second it would represent was never
         * written in this window and is genuinely zero, not missing. */
        const uint32_t want = now_sec + 1u + i - PW_WINDOW_SLOTS;
        if (want > now_sec) {
            continue; /* the device has not been up this long */
        }
        const pw_slot_t *s = &e->slots[want % PW_WINDOW_SLOTS];
        if (s->sec == want) {
            out[i] = (uint16_t)(s->disconnects > 0xFFFFu ? 0xFFFFu : s->disconnects);
        }
    }
}

/* ---- scoring -------------------------------------------------------- */

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
    if (window_ms > PW_WINDOW_SLOTS * 1000u) {
        window_ms = PW_WINDOW_SLOTS * 1000u;
    }
    const uint64_t window_us = (uint64_t)window_ms * 1000ull;
    const uint64_t start_us = (now_us > window_us) ? (now_us - window_us) : 0ull;

    out->ceiling = pw_ceiling(ctx);

    const uint32_t dwell = clamp_u32(ctx->dwell_permil, 1, 1000);
    const uint32_t yield = clamp_u32(ctx->bus_yield_permil, 1, 1000);
    if (dwell < 500) out->notes |= PW_NOTE_THIN_DWELL;
    if (yield < 950) out->notes |= PW_NOTE_LOSSY;

    /* --- windowed counts from the per-second slots --------------------- */
    const uint32_t now_sec = (uint32_t)(now_us / 1000000ull);
    uint32_t win_s = (window_ms + 999u) / 1000u;
    if (win_s == 0) win_s = 1;
    if (win_s > PW_WINDOW_SLOTS) win_s = PW_WINDOW_SLOTS;

    uint32_t counted = 0, rejoins = 0, peak = 0;
    for (unsigned i = 0; i < PW_WINDOW_SLOTS; i++) {
        const pw_slot_t *s = &e->slots[i];
        if (s->sec > now_sec) continue;
        if ((now_sec - s->sec) >= win_s) continue;
        counted += s->disconnects;
        rejoins += s->rejoins;
        if (s->disconnects > peak) {
            peak = s->disconnects;
        }
    }
    out->observed = counted;
    out->peak_second = peak;
    out->rejoins = rejoins;

    /* --- shape, from the most recent frames in the window --------------- */
    uint8_t victims[PW_MAX_VICTIMS][6];
    uint8_t sources[PW_MAX_SOURCES][6];
    unsigned n_victims = 0, n_sources = 0;
    uint32_t sample = 0, broadcast = 0, protectedf = 0;
    uint16_t reasons[8];
    uint16_t reason_hits[8];
    unsigned n_reasons = 0;

    /* Burst shape: the longest run of consecutive disconnects aimed at one
     * destination inside half a second. Every deauth tool emits in tight runs
     * (aireplay-ng defaults to 64 per burst); an access point disconnecting a
     * client that timed out sends one or two. */
    uint8_t run_dst[6];
    uint32_t run_len = 0, best_run = 0;
    uint64_t run_last_us = 0;
    bool run_open = false;

    for (unsigned i = 0; i < e->hit_count; i++) {
        const unsigned idx =
            (unsigned)((e->hit_head + PW_MAX_HITS - 1 - i) % PW_MAX_HITS);
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
        if (h->flags & PHAROS_DOT11_F_PROTECTED) {
            protectedf++;
        }

        /* Walking backwards in time, so a "run" is contiguous here too. */
        if (run_open && mac_eq(run_dst, h->dst) &&
            (run_last_us - h->t_us) <= 500000ull) {
            run_len++;
        } else {
            memcpy(run_dst, h->dst, 6);
            run_len = 1;
            run_open = true;
        }
        run_last_us = h->t_us;
        if (run_len > best_run) {
            best_run = run_len;
        }

        bool src_seen = false;
        for (unsigned s = 0; s < n_sources; s++) {
            if (mac_eq(sources[s], h->src)) { src_seen = true; break; }
        }
        if (!src_seen && n_sources < PW_MAX_SOURCES) {
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
    out->max_burst = (uint8_t)clamp_u32(best_run, 0, 255);
    out->broadcast_permil = sample ? (uint16_t)((broadcast * 1000u) / sample) : 0u;
    if (protectedf) {
        out->notes |= PW_NOTE_PROTECTED;
    }
    if (counted > sample) {
        out->notes |= PW_NOTE_SAMPLED;
    }

    if (counted == 0 && sample == 0) {
        out->band = PW_BAND_QUIET;
        return;
    }

    out->channel = pw_pressure_channel(e, now_us, window_ms);

    /* --- the dominant transmitter, and what it looks like --------------- */
    uint32_t dom_hits = 0;
    int32_t dom_rssi_sum = 0;
    uint32_t dom_unprotected = 0;
    uint32_t dom_nonretry = 0;
    uint16_t dom_seqs[16];
    unsigned dom_n_seqs = 0;

    for (unsigned s = 0; s < n_sources; s++) {
        uint32_t hits = 0;
        int32_t rssi_sum = 0;
        uint32_t unprot = 0;
        uint32_t nonretry = 0;
        uint16_t seqs[16];
        unsigned n_seqs = 0;
        for (unsigned i = 0; i < e->hit_count; i++) {
            const unsigned idx =
                (unsigned)((e->hit_head + PW_MAX_HITS - 1 - i) % PW_MAX_HITS);
            const pw_hit_t *h = &e->hits[idx];
            if (h->t_us < start_us) break;
            if (!mac_eq(h->src, sources[s])) continue;
            hits++;
            rssi_sum += h->rssi;
            if (!(h->flags & PHAROS_DOT11_F_PROTECTED)) {
                unprot++;
            }
            /* RETRANSMISSIONS REUSE THE SEQUENCE NUMBER. That is not a
             * forgery, it is 802.11 working: a frame the sender did not get
             * acked is sent again with the retry bit set and the SAME counter
             * value, precisely so the receiver can discard the duplicate.
             *
             * Counting them made an access point retrying one disconnect look
             * exactly like a tool blasting a hand-built frame - and it did,
             * on the first real air this engine ever saw: ambient traffic read
             * SUSPICIOUS with SEQ_FROZEN lit. Retries are excluded from the
             * distinct-value tally for that reason. */
            if (h->flags & PHAROS_DOT11_F_RETRY) {
                continue;
            }
            nonretry++;
            bool seen = false;
            for (unsigned q = 0; q < n_seqs; q++) {
                if (seqs[q] == h->seq) { seen = true; break; }
            }
            if (!seen && n_seqs < 16) {
                seqs[n_seqs++] = h->seq;
            }
        }
        if (hits > dom_hits) {
            dom_hits = hits;
            dom_rssi_sum = rssi_sum;
            dom_unprotected = unprot;
            dom_nonretry = nonretry;
            dom_n_seqs = n_seqs;
            memcpy(dom_seqs, seqs, sizeof(seqs));
            memcpy(out->src, sources[s], 6);
        }
    }
    (void)dom_seqs;
    const int32_t dom_rssi_avg = dom_hits ? (dom_rssi_sum / (int32_t)dom_hits) : 0;
    out->seq_distinct = (uint8_t)dom_n_seqs;

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

    /* Duty correction: scale what we heard by the share of the channel we were
     * listening to and the share of frames the bus kept. This is an
     * extrapolation, and it is the ONLY part of the score that hopping is
     * allowed to inflate - which is why the ceiling exists to hold it back.
     *
     * But it needs a denominator that was actually observed. See
     * PW_MIN_CHANNEL_MS: below that much time on the channel there is nothing
     * to divide by and the quotient is noise wearing a number's clothes. */
    const uint32_t heard_ms = (uint32_t)(((uint64_t)elapsed_ms * dwell) / 1000ull);
    if (heard_ms < PW_MIN_CHANNEL_MS) {
        out->notes |= PW_NOTE_NO_RATE;
        out->est_per_s_x100 = 0;
    } else {
        const uint64_t num =
            (uint64_t)counted * 100ull * 1000ull * 1000ull * 1000ull;
        const uint64_t den =
            (uint64_t)elapsed_ms * (uint64_t)dwell * (uint64_t)yield;
        out->est_per_s_x100 = (uint32_t)(den ? (num / den) : 0ull);
    }

    /* --- FAMILY 1: rate ------------------------------------------------ */
    /* Anchored on Kismet's DEAUTHFLOOD defaults so the words mean what a WIDS
     * operator expects: ~0.08/s (5/min) is where "notable" starts and 2/s is a
     * burst worth an alert. Above that we are into tool territory. */
    static const uint32_t rate_x[] = { 8, 50, 200, 800, 3000, 10000 };
    static const uint32_t rate_y[] = { 0,  7,  15,  23,   30,    34 };
    uint32_t c_rate = (out->notes & PW_NOTE_NO_RATE)
                          ? 0u
                          : interp(out->est_per_s_x100, rate_x, rate_y, 6);
    /* The peak second is a measurement, not an extrapolation - it needs no
     * duty correction and it catches short bursts a ten-second mean flattens
     * into nothing. Take whichever reading is stronger. */
    if (peak >= 2u) {
        static const uint32_t peak_x[] = { 2, 5, 15, 40, 120 };
        static const uint32_t peak_y[] = { 8, 15, 22, 28, 34 };
        const uint32_t c_peak = interp(peak, peak_x, peak_y, 5);
        if (c_peak > c_rate) {
            c_rate = c_peak;
        }
    }
    out->c_rate = (uint8_t)clamp_u32(c_rate, 0, 34);

    /* --- FAMILY 2: targeting shape ------------------------------------- */
    uint32_t shape = ((uint32_t)out->broadcast_permil * 10u) / 1000u; /* 0..10 */
    if (n_victims >= 5) {
        shape += 6;
    } else if (n_victims >= 2) {
        shape += 3;
    }
    /* The burst signature. A run of eight at one address inside half a second
     * is not something a healthy access point does. */
    if (best_run >= 16) {
        shape += 8;
    } else if (best_run >= 8) {
        shape += 5;
    } else if (best_run >= 4) {
        shape += 2;
    }
    out->c_shape = (uint8_t)clamp_u32(shape, 0, 22);

    /* --- FAMILY 3: forgery --------------------------------------------- */
    const pw_ap_t *claimed = ap_lookup(e, out->src);
    uint32_t forge = 0;
    bool hard = false;

    if (!claimed || claimed->beacons == 0) {
        /* Never heard this BSSID beacon. Suggestive of a forged source, but
         * while hopping we very plausibly just missed the beacons, so the
         * points are scaled by how much of the channel we actually heard. */
        const uint32_t ghost = (14u * dwell) / 1000u;
        if (ghost) {
            out->forgery |= PW_FORGE_GHOST;
            forge += ghost;
        }
    } else {
        /* 802.11w contradiction. This is the strongest thing a passive
         * receiver can find, and it is a contradiction rather than an
         * estimate: if the network REQUIRES protected management frames, an
         * unprotected disconnect claiming that BSSID cannot have come from
         * it. Hearing it during a short visit makes it no less impossible. */
        if (claimed->rsn_flags & PHAROS_RSN_F_MFP_REQUIRED) {
            out->notes |= PW_NOTE_MFP_TARGET;
            if (dom_unprotected >= 2) {
                out->forgery |= PW_FORGE_MFP_PROOF;
                forge += 24;
                hard = true;
            }
        } else if (claimed->rsn_flags & PHAROS_RSN_F_MFP_CAPABLE) {
            out->notes |= PW_NOTE_MFP_TARGET;
            /* Capable but not required: a client that never negotiated
             * protection can legitimately be disconnected in the clear, so
             * this is a hint and it is not hard evidence. */
            if (dom_unprotected >= 4) {
                out->forgery |= PW_FORGE_MFP_HINT;
                forge += 10;
            }
        }

        /* Sequence order. Needs beacons either side to mean anything, which is
         * why it wants two of them before it will speak. */
        const uint32_t viol = (uint32_t)claimed->seq_back + claimed->seq_fwd;
        out->seq_violations = (uint16_t)clamp_u32(viol, 0, 0xFFFF);
        if (claimed->beacons >= 3 && viol >= 3) {
            out->forgery |= PW_FORGE_SEQ_ORDER;
            forge += 12 + clamp_u32(viol - 3u, 0, 8);
            hard = true; /* order violations survive hopping: they are not rates */
        }

        /* Level. Compared against the beacon's own spread rather than a flat
         * threshold: three times the deviation, floored at 8 dB so a very
         * stable beacon does not make the test hair-trigger. */
        const int32_t beacon_rssi = claimed->rssi_ewma_x4 / 4;
        const int32_t spread = claimed->rssi_dev_x4 / 4;
        const int32_t delta = iabs32(beacon_rssi - dom_rssi_avg);
        out->rssi_delta = (int8_t)clamp_u32((uint32_t)delta, 0, 127);
        out->rssi_spread = (int8_t)clamp_u32((uint32_t)spread, 0, 127);
        int32_t threshold = spread * 3;
        if (threshold < 8) threshold = 8;
        if (claimed->beacons >= 4 && dom_hits >= 3 && delta >= threshold) {
            out->forgery |= PW_FORGE_RSSI_SPLIT;
            forge += 10u + clamp_u32((uint32_t)(delta - threshold), 0, 6);
        }
    }

    /* A frozen counter is a property of the sender, so it is tested whether or
     * not we ever heard the claimed AP beacon. Judged on NON-RETRY frames
     * only - see the note where they are counted. */
    if (dom_nonretry >= 8 && dom_n_seqs <= 2) {
        out->forgery |= PW_FORGE_SEQ_FROZEN;
        forge += 12;
    }

    out->c_forgery = (uint8_t)clamp_u32(forge, 0, 30);
    if (hard) {
        out->notes |= PW_NOTE_HARD;
    }

    /* --- FAMILY 4: aftermath ------------------------------------------- */
    /* Did it work? Clients that are knocked off come straight back, so a real
     * disconnection event is followed within a couple of seconds by a spike of
     * authentication and association requests. Correlating the stampede with
     * the burst that caused it is the one test that still works on the
     * low-volume TARGETED attacks that volume thresholds miss. */
    uint32_t rejoins_after = 0;
    if (peak >= 2u && rejoins > 0u) {
        const uint32_t burst_min = (peak / 4u) > 2u ? (peak / 4u) : 2u;
        for (unsigned i = 0; i < PW_WINDOW_SLOTS; i++) {
            const pw_slot_t *s = &e->slots[i];
            if (s->sec > now_sec || (now_sec - s->sec) >= win_s) continue;
            if (s->disconnects < burst_min) continue;
            /* the three seconds that follow this burst */
            for (uint32_t d = 1; d <= 3; d++) {
                const uint32_t sec = s->sec + d;
                if (sec > now_sec) break;
                const pw_slot_t *nx = &e->slots[sec % PW_WINDOW_SLOTS];
                if (nx->sec == sec) {
                    rejoins_after += nx->rejoins;
                }
            }
        }
    }
    out->rejoins_after = rejoins_after;

    uint32_t after = 0;
    if (rejoins_after >= 3u) {
        /* Both the size of the stampede and how much of ALL the rejoin traffic
         * in the window it accounts for. A network where clients associate all
         * day scores nothing here; one that went quiet and then all came back
         * at once scores the maximum. */
        static const uint32_t re_x[] = { 3, 6, 12, 30 };
        static const uint32_t re_y[] = { 5, 9, 13, 15 };
        after = interp(rejoins_after, re_x, re_y, 4);
        const uint32_t share = rejoins ? (rejoins_after * 100u) / rejoins : 0u;
        if (share >= 75u) {
            after += 3;
        }
    }
    out->c_aftermath = (uint8_t)clamp_u32(after, 0, 18);

    /* --- reason-code monoculture (a modifier, never a family) ----------- */
    /* Deliberately small and deliberately strict. Kismet retired its own
     * invalid-reason-code alert because modern access points emit odd codes
     * routinely; monoculture across a large sample is a hint that one hand
     * built every frame, and nothing more. */
    uint32_t reason_pts = 0;
    if (sample >= 12 && out->dominant_reason_pct >= 92) {
        reason_pts = 6;
        if (out->dominant_reason == 7 || out->dominant_reason == 1) {
            reason_pts += 4; /* class-3-frame / unspecified: the tool defaults */
        }
    }
    out->c_reason = (uint8_t)clamp_u32(reason_pts, 0, 10);

    /* --- which families are actually speaking -------------------------- */
    if (out->c_rate >= 12)      out->families |= PW_FAM_RATE;
    if (out->c_shape >= 8)      out->families |= PW_FAM_SHAPE;
    if (out->c_forgery >= 10)   out->families |= PW_FAM_FORGERY;
    if (out->c_aftermath >= 8)  out->families |= PW_FAM_AFTERMATH;

    uint32_t raw = (uint32_t)out->c_rate + out->c_shape + out->c_forgery +
                   out->c_aftermath + out->c_reason;
    raw = clamp_u32(raw, 0, 100);
    out->raw_score = (uint8_t)raw;

    /* --- caps ----------------------------------------------------------- */
    uint32_t score = raw;

    unsigned family_count = 0;
    for (unsigned b = 0; b < 4; b++) {
        if (out->families & (1u << b)) family_count++;
    }
    if (family_count < 2 && score > PW_CAP_ONE_FAMILY) {
        score = PW_CAP_ONE_FAMILY;   /* one loud signal is not a case */
    }
    if (family_count < 3 && score > PW_CAP_TWO_FAMILIES) {
        score = PW_CAP_TWO_FAMILIES; /* two is a lead, not a conclusion */
    }
    /* THE COROLLARY OF THE WHOLE DESIGN. Volume and targeting shape describe
     * how much traffic there is and where it is pointed - both of which a busy,
     * healthy, badly-behaved network can produce. Neither says anybody lied
     * about who they are, and neither says a single client was actually
     * knocked off. Without one of those two, this never alarms.
     *
     * With four families this is already implied by the three-family cap
     * above: only two families are volume-shaped, so any three of them
     * contain a corroborating one, and this branch never binds today. It is
     * written out anyway because it is the PROMISE, and the three-family rule
     * is only the current arithmetic that happens to deliver it. Add a fifth
     * volume-shaped family and the arithmetic stops being sufficient while
     * this line keeps the promise intact. */
    const bool corroborated =
        (out->families & (PW_FAM_FORGERY | PW_FAM_AFTERMATH)) != 0;
    if (!corroborated && score > PW_CAP_NO_CORROBORATION) {
        score = PW_CAP_NO_CORROBORATION;
    }
    if ((out->notes & PW_NOTE_SHORT_WINDOW) && score > PW_CAP_SHORT_WINDOW) {
        score = PW_CAP_SHORT_WINDOW;
    }

    /* The confidence ceiling. A dwell-independent contradiction - a frame that
     * could not have come from the device it names - raises it, because the
     * thing that made it true does not depend on how long we listened. It is
     * still capped below 100: there is no posture in which this firmware
     * claims certainty. */
    if (hard && out->ceiling < PW_CEILING_HARD_EVIDENCE) {
        out->ceiling = PW_CEILING_HARD_EVIDENCE;
    }
    if (score > out->ceiling) {
        score = out->ceiling;
    }

    out->score = (uint8_t)score;
    out->band = band_of(out->score);

    /* Name the network under pressure, if we ever heard it introduce itself. */
    if (claimed && claimed->ssid_len) {
        const uint8_t n = claimed->ssid_len > PHAROS_EV_SSID_MAX
                              ? PHAROS_EV_SSID_MAX : claimed->ssid_len;
        memcpy(out->ssid, claimed->ssid, n);
        out->ssid[n] = '\0';
    }
}

/* ---- words ----------------------------------------------------------- */

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
        return "No disconnect traffic in view. This receiver hears one "
               "channel at a time.";
    case PW_BAND_BACKGROUND:
        return "Deauthentication at levels healthy networks produce.";
    case PW_BAND_ELEVATED:
        return "More disconnect traffic than housekeeping explains.";
    case PW_BAND_SUSPICIOUS:
        return "The shape looks wrong but nothing proves a forgery yet.";
    case PW_BAND_LIKELY:
        return "Sustained disconnects that do not add up. Preserve the log.";
    default:
        return "";
    }
}

const char *pw_band_hint(pw_band_t band)
{
    /* Kept under PW_HINT_MAX_CHARS. Count before you edit. */
    switch (band) {
    case PW_BAND_QUIET:      return "Nothing in view yet.";
    case PW_BAND_BACKGROUND: return "Normal roaming and timeouts.";
    case PW_BAND_ELEVATED:   return "More than housekeeping.";
    case PW_BAND_SUSPICIOUS: return "Shape is wrong. Camp to confirm.";
    case PW_BAND_LIKELY:     return "Forged disconnects. Keep the log.";
    default:                 return "";
    }
}

const char *pw_forgery_name(uint8_t forgery_mask)
{
    /* Strongest first: the operator should be told the best reason, not the
     * first one that happened to be tested. */
    if (forgery_mask & PW_FORGE_MFP_PROOF)  return "unprotected on an 802.11w net";
    if (forgery_mask & PW_FORGE_SEQ_ORDER)  return "sequence counter went backwards";
    if (forgery_mask & PW_FORGE_SEQ_FROZEN) return "sequence counter never moves";
    if (forgery_mask & PW_FORGE_RSSI_SPLIT) return "wrong signal level for this AP";
    if (forgery_mask & PW_FORGE_MFP_HINT)   return "unprotected, 802.11w offered";
    if (forgery_mask & PW_FORGE_GHOST)      return "source has never beaconed";
    return (const char *)0;
}

const char *pw_reason_name(uint16_t reason)
{
    switch (reason) {
    case 1:  return "unspecified";
    case 2:  return "auth no longer valid";
    case 3:  return "leaving";
    case 4:  return "inactivity";
    case 5:  return "AP overloaded";
    case 6:  return "class 2 frame";
    case 7:  return "class 3 frame";
    case 8:  return "leaving BSS";
    case 9:  return "not authenticated";
    case 15: return "4-way timeout";
    default: return "other";
    }
}
