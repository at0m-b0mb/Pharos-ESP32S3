/* Pharos - the acoustic engine. See pharos_acoustic.h for what it refuses to
 * do, and why a microphone on a security device is added this way or not at
 * all. */
#include "pharos_acoustic.h"

#include <string.h>

/* ---- probes ---------------------------------------------------------- */

static const uint32_t k_hz[PAC_BAND_COUNT] = {
    1000, 18000, 19000, 20000, 21000,
};

uint32_t pac_probe_hz(pac_band_t b)
{
    return (b < PAC_BAND_COUNT) ? k_hz[b] : 0;
}

const char *pac_probe_name(pac_band_t b)
{
    switch (b) {
    case PAC_BAND_AUDIBLE: return "1 kHz";
    case PAC_BAND_18K:     return "18 kHz";
    case PAC_BAND_19K:     return "19 kHz";
    case PAC_BAND_20K:     return "20 kHz";
    case PAC_BAND_21K:     return "21 kHz";
    default:               return "?";
    }
}

/* ---- the Goertzel probe ----------------------------------------------
 *
 * One bin of a DFT, computed without the DFT. For a handful of known
 * frequencies this is far cheaper than an FFT and - the part that matters
 * here - it needs no buffer: samples are consumed as they arrive and only two
 * running state variables survive each one. There is structurally nowhere for
 * audio to accumulate, which is the privacy promise expressed as an
 * algorithm choice rather than as a policy.
 *
 * Integer throughout. The coefficient is held in Q14 so the whole thing runs
 * in 64-bit integers on a chip whose FPU we would rather leave for LVGL. */
static uint32_t goertzel_q(const int16_t *x, unsigned n, uint32_t hz,
                           uint32_t rate)
{
    if (!x || n == 0 || rate == 0) {
        return 0;
    }
    /* coeff = 2*cos(2*pi*hz/rate), in Q14. cos is evaluated from a small
     * table by linear interpolation rather than libm: this file must build
     * and behave identically on the host and on target, and the target build
     * is deliberately float-free. */
    static const int32_t cos_q14[65] = { /* cos(pi*i/64), i = 0..64, Q14 */
        16384, 16364, 16305, 16207, 16069, 15893, 15679, 15426,
        15137, 14811, 14449, 14053, 13623, 13160, 12665, 12140,
        11585, 11003, 10394,  9760,  9102,  8423,  7723,  7005,
         6270,  5520,  4756,  3981,  3196,  2404,  1606,   804,
            0,  -804, -1606, -2404, -3196, -3981, -4756, -5520,
        -6270, -7005, -7723, -8423, -9102, -9760,-10394,-11003,
       -11585,-12140,-12665,-13160,-13623,-14053,-14449,-14811,
       -15137,-15426,-15679,-15893,-16069,-16207,-16305,-16364,
       -16384,
    };
    /* theta = 2*pi*hz/rate, expressed as a position in [0,64] over [0,pi]. */
    const uint64_t pos_x256 = ((uint64_t)hz * 2u * 64u * 256u) / rate;
    if (pos_x256 >= 64u * 256u) {
        return 0; /* at or beyond Nyquist: this probe cannot mean anything */
    }
    const unsigned i = (unsigned)(pos_x256 / 256u);
    const uint32_t frac = (uint32_t)(pos_x256 % 256u);
    const int32_t c = cos_q14[i] + (int32_t)(((int64_t)(cos_q14[i + 1] - cos_q14[i]) * frac) / 256);
    const int32_t coeff = 2 * c; /* Q14 */

    int64_t s1 = 0, s2 = 0;
    for (unsigned k = 0; k < n; k++) {
        const int64_t s0 = (int64_t)x[k] + (((int64_t)coeff * s1) >> 14) - s2;
        s2 = s1;
        s1 = s0;
    }
    /* |X|^2 = s1^2 + s2^2 - coeff*s1*s2, scaled down to stay in range. */
    int64_t mag2 = (s1 * s1) + (s2 * s2) - (((int64_t)coeff * s1 * s2) >> 14);
    if (mag2 < 0) {
        mag2 = 0;
    }
    /* Normalise by the window length so different n are comparable. */
    mag2 /= (int64_t)n;
    mag2 /= (int64_t)n;
    return (mag2 > 0xFFFFFFFFll) ? 0xFFFFFFFFu : (uint32_t)mag2;
}

/* Energy -> a 0..120 "how many dB below full scale" figure, small first.
 * Integer log2, then scaled: 6 dB per doubling is close enough for a level
 * meter and exact enough to compare bands against each other. */
static uint8_t to_dbfs(uint32_t mag2)
{
    if (mag2 == 0) {
        return 120;
    }
    unsigned bits = 0;
    uint32_t v = mag2;
    while (v > 1u) {
        v >>= 1;
        bits++;
    }
    /* mag2 is a squared amplitude, so one bit is 3 dB. Full scale for an
     * int16 squared and normalised lands near bit 30. */
    const int db_below = (int)(30 - (int)bits) * 3;
    if (db_below < 0)   return 0;
    if (db_below > 120) return 120;
    return (uint8_t)db_below;
}

/* ---- ingest ---------------------------------------------------------- */

void pac_reset(pac_engine_t *e)
{
    if (e) {
        memset(e, 0, sizeof(*e));
        for (unsigned b = 0; b < PAC_BAND_COUNT; b++) {
            e->level[b] = 120;
            e->floor[b] = 120;
            e->peak[b] = 0;
        }
        e->audible_best = 120;
    }
}

void pac_observe(pac_engine_t *e, const int16_t *samples, unsigned n,
                 uint32_t sample_rate)
{
    if (!e || !samples || n == 0) {
        return;
    }
    for (unsigned b = 0; b < PAC_BAND_COUNT; b++) {
        const uint32_t mag2 = goertzel_q(samples, n, k_hz[b], sample_rate);
        const uint8_t lvl = to_dbfs(mag2);
        e->level[b] = lvl;

        /* The noise floor is a slow MAXIMUM of the quiet level - remember,
         * smaller means louder - so a band that is usually silent has a floor
         * near 120 and a room with a constant hum settles wherever that hum
         * is. Rising fast and falling slowly means a beacon cannot raise its
         * own floor and hide inside it. */
        if (!e->floor_valid) {
            e->floor[b] = lvl;
        } else if (lvl > e->floor[b]) {
            e->floor[b] = (uint8_t)(e->floor[b] + 1); /* quieter: drift up slowly */
        } else if (lvl < e->floor[b]) {
            /* louder than the floor: do not chase it, that is the signal */
        }

        /* Present = meaningfully above this band's own floor. 9 dB is three
         * doublings of power; a room does not do that to a single narrow
         * band by accident. */
        const bool present = (e->floor[b] >= 9u) && (lvl + 9u <= e->floor[b]);
        e->seen[b] = (e->seen[b] << 1) | (present ? 1u : 0u);

        /* Peak-hold with a slow decay, for the reason in the header: the
         * verdict is about what this band has done, not about whichever
         * window happened to be last. */
        const uint32_t margin = (e->floor[b] > lvl) ? (uint32_t)(e->floor[b] - lvl) : 0u;
        if (margin > e->peak[b]) {
            e->peak[b] = (uint8_t)((margin > 120u) ? 120u : margin);
        } else if (e->peak[b] > 0u) {
            e->peak[b]--;
        }
    }
    /* Same treatment for the liveness check. */
    if (e->level[PAC_BAND_AUDIBLE] < e->audible_best) {
        e->audible_best = e->level[PAC_BAND_AUDIBLE];
    } else if (e->audible_best < 120u) {
        e->audible_best++;
    }
    e->floor_valid = true;
    if (e->windows < PAC_SLOTS) {
        e->windows++;
    }
}

/* ---- scoring --------------------------------------------------------- */

static unsigned popcount32(uint32_t v)
{
    unsigned c = 0;
    while (v) {
        v &= (v - 1u);
        c++;
    }
    return c;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void pac_evaluate(const pac_engine_t *e, pac_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!e) {
        return;
    }
    for (unsigned b = 0; b < PAC_BAND_COUNT; b++) {
        out->level[b] = e->level[b];
    }
    out->windows = e->windows;

    /* Not enough history to say anything about persistence, which is most of
     * what separates a beacon from a key being put down on a desk. */
    if (e->windows < 8u) {
        out->notes |= PAC_NOTE_SHORT;
    }

    /* Is the microphone alive? A silent audible band means an absent beacon
     * and a dead capture path look identical, and the honest answer is to say
     * so rather than to report QUIET as if it were a finding. */
    if (e->audible_best >= 110u) {
        out->notes |= PAC_NOTE_DEAF;
    } else if (e->audible_best <= 30u) {
        /* A loud room throws harmonics and intermodulation upward; a genuine
         * 19 kHz tone and the third harmonic of a shriek are not the same
         * finding, and only one of them is worth telling somebody about. */
        out->notes |= PAC_NOTE_LOUD_ROOM;
    }

    /* The strongest ultrasonic band over the HISTORY.
     *
     * Ranked by how often the band was present first and by how far it got
     * second, because a beacon is defined by recurrence: a band that was
     * briefly louder but appeared once is a clatter, and a band that shows up
     * again and again is the thing worth naming. */
    const unsigned hist_n = (e->windows < PAC_SLOTS) ? e->windows : PAC_SLOTS;
    const uint32_t hist_mask = (hist_n >= 32u) ? 0xFFFFFFFFu : ((1u << hist_n) - 1u);
    /* Ranked by MARGIN among the bands that recur at all.
     *
     * Ranking by recurrence first was the obvious reading of "a beacon is a
     * thing that happens again" and it picked the wrong band: a strong tone
     * leaks into its neighbouring probes - a 19 kHz tone 45 dB down at 20 kHz
     * is still well above that band's floor - so the neighbour is "present"
     * in exactly the same windows and wins any tie on hit count. Recurrence
     * decides ELIGIBILITY; how far the band actually got decides which one it
     * is. */
    pac_band_t best = PAC_BAND_18K;
    uint32_t best_margin = 0;
    bool have_eligible = false;
    for (unsigned pass = 0; pass < 2 && !have_eligible; pass++) {
        for (unsigned b = PAC_BAND_18K; b < PAC_BAND_COUNT; b++) {
            /* First pass: only bands that appeared more than once. Second
             * pass, if none did: whatever got furthest, so the verdict still
             * names something rather than defaulting to 18 kHz. */
            if (pass == 0 && popcount32(e->seen[b] & hist_mask) < 2u) {
                continue;
            }
            if (pass == 0) {
                have_eligible = true;
            }
            if (e->peak[b] > best_margin) {
                best_margin = e->peak[b];
                best = (pac_band_t)b;
            }
        }
    }
    out->strongest = best;
    if (best == PAC_BAND_21K) {
        out->notes |= PAC_NOTE_EDGE_OF_HEARING;
    }

    /* --- LEVEL: is it loud enough to be deliberate? -------------------- */
    static const uint32_t lv_x[] = { 9, 15, 24, 40 };
    static const uint32_t lv_y[] = { 0, 12, 22, 30 };
    uint32_t level = 0;
    for (unsigned i = 1; i < 4; i++) {
        if (best_margin <= lv_x[i]) {
            const uint32_t span = lv_x[i] - lv_x[i - 1];
            level = lv_y[i - 1] + (span ? ((best_margin - lv_x[i - 1]) *
                                           (lv_y[i] - lv_y[i - 1])) / span : 0);
            break;
        }
        level = lv_y[3];
    }
    if (best_margin < lv_x[0]) {
        level = 0;
    }
    out->c_level = (uint8_t)clamp_u32(level, 0, 30);

    /* --- NARROW: a tone, not noise ------------------------------------
     *
     * A beacon lives in one band. Broadband noise - a fan, a hiss, a hand
     * across a table - lifts every probe together, and lifting every probe
     * together is exactly what this must NOT call a beacon. Score the gap
     * between the strongest ultrasonic band and the next strongest. */
    uint32_t second = 0;
    for (unsigned b = PAC_BAND_18K; b < PAC_BAND_COUNT; b++) {
        if ((pac_band_t)b == best) {
            continue;
        }
        if (e->peak[b] > second) {
            second = e->peak[b];
        }
    }
    const uint32_t sharpness = (best_margin > second) ? (best_margin - second) : 0u;
    out->c_narrow = (uint8_t)clamp_u32(sharpness * 2u, 0, 24);

    /* --- PERSISTENT: it keeps coming back ------------------------------ */
    const unsigned hist = (e->windows < PAC_SLOTS) ? e->windows : PAC_SLOTS;
    const uint32_t mask = (hist >= 32u) ? 0xFFFFFFFFu : ((1u << hist) - 1u);
    const unsigned hits = popcount32(e->seen[best] & mask);
    out->hits = (uint8_t)hits;
    out->duty_pct = hist ? (uint8_t)((hits * 100u) / hist) : 0u;

    uint32_t persist = 0;
    if (hist >= 8u && hits >= 3u) {
        persist = clamp_u32((uint32_t)out->duty_pct / 3u, 0, 26);
    }
    out->c_persist = (uint8_t)persist;

    if (out->c_level >= 12)   out->families |= PAC_FAM_LEVEL;
    if (out->c_narrow >= 8)   out->families |= PAC_FAM_NARROW;
    if (out->c_persist >= 10) out->families |= PAC_FAM_PERSISTENT;

    uint32_t score = (uint32_t)out->c_level + out->c_narrow + out->c_persist;

    /* --- ceilings ------------------------------------------------------
     *
     * The microphone path is falling away by 21 kHz, so a finding that rests
     * on the top probe cannot be told from a receiver that is going deaf.
     * A loud room can throw harmonics upward. And one family is never a case,
     * exactly as everywhere else in this firmware. */
    uint8_t ceiling = 92; /* nothing acoustic here is ever certainty */
    if (out->notes & PAC_NOTE_EDGE_OF_HEARING) ceiling = 62;
    if (out->notes & PAC_NOTE_LOUD_ROOM)       ceiling = (ceiling > 66) ? 66 : ceiling;
    if (out->notes & PAC_NOTE_SHORT)           ceiling = (ceiling > 45) ? 45 : ceiling;
    if (out->notes & PAC_NOTE_DEAF)            ceiling = 0;

    unsigned fams = 0;
    for (unsigned b = 0; b < 3; b++) {
        if (out->families & (1u << b)) fams++;
    }
    if (fams < 2 && score > 45) {
        score = 45;
    }
    /* A tone that does not repeat is a noise. Reaching the top band requires
     * persistence specifically - a beacon is a thing that happens again. */
    if (!(out->families & PAC_FAM_PERSISTENT) && score > 66) {
        score = 66;
    }
    if (score > ceiling) {
        score = ceiling;
    }
    out->ceiling = ceiling;
    out->score = (uint8_t)score;

    if (score >= 75)      out->band = PAC_BAND_BEACON;
    else if (score >= 60) out->band = PAC_BAND_PERSISTENT;
    else if (score >= 40) out->band = PAC_BAND_PRESENT;
    else if (score >= 20) out->band = PAC_BAND_TRACE;
    else                  out->band = PAC_BAND_QUIET;
}

const char *pac_band_name(pac_verdict_band_t b)
{
    switch (b) {
    case PAC_BAND_QUIET:      return "QUIET";
    case PAC_BAND_TRACE:      return "TRACE";
    case PAC_BAND_PRESENT:    return "TONE PRESENT";
    case PAC_BAND_PERSISTENT: return "PERSISTENT";
    case PAC_BAND_BEACON:     return "BEACON LIKELY";
    default:                  return "?";
    }
}

const char *pac_band_hint(pac_verdict_band_t b)
{
    /* Under 34 characters, like every other on-screen hint. */
    switch (b) {
    case PAC_BAND_QUIET:      return "Nothing above the room.";
    case PAC_BAND_TRACE:      return "Something, but it comes and goes.";
    case PAC_BAND_PRESENT:    return "An inaudible tone is here.";
    case PAC_BAND_PERSISTENT: return "It keeps coming back.";
    case PAC_BAND_BEACON:     return "Inaudible beacon. Move and retest.";
    default:                  return "";
    }
}

/* ---- holding the verdict still --------------------------------------- */

void pac_hold_reset(pac_hold_t *h)
{
    if (h) {
        memset(h, 0, sizeof(*h));
    }
}

/* THE BAND EDGES, WITH A KERB.
 *
 * The confirm counter stops a verdict that flickers randomly. It cannot help
 * a score that genuinely SITS on a boundary: 38, 42, 38, 42 crosses 40 every
 * time, agrees with itself for four evaluations, and is dutifully adopted -
 * so TRACE and TONE PRESENT still traded places every few seconds on real
 * hardware.
 *
 * That is what hysteresis is for. Leaving the band you are in costs a few
 * points more than entering it did, so a reading parked on a threshold stays
 * where it is instead of rattling between two names for the same room. */
#define PAC_HYST 5u

static pac_verdict_band_t band_with_kerb(uint8_t score, pac_verdict_band_t shown)
{
    /* Lower edge of each band; index by pac_verdict_band_t. */
    static const uint8_t lo[] = { 0u, 20u, 40u, 60u, 75u };
    const unsigned n = (unsigned)(sizeof(lo) / sizeof(lo[0]));

    pac_verdict_band_t nat = PAC_BAND_QUIET;
    for (unsigned i = 0; i < n; i++) {
        if (score >= lo[i]) {
            nat = (pac_verdict_band_t)i;
        }
    }
    if (nat == shown || (unsigned)shown >= n) {
        return shown;
    }

    /* Climbing: clear the band's own floor by the margin. Falling: drop
     * clear of the floor of the band being left. */
    if ((unsigned)nat > (unsigned)shown) {
        return (score >= (unsigned)lo[nat] + PAC_HYST) ? nat : shown;
    }
    return (score + PAC_HYST <= lo[shown]) ? nat : shown;
}

bool pac_hold_apply(pac_hold_t *h, pac_verdict_t *v)
{
    if (!h || !v) {
        return false;
    }

    /* Apply the kerb before anything else, so the confirm counter is only
     * ever asked about changes that already cleared the margin. */
    if (h->primed) {
        v->band = band_with_kerb(v->score, h->shown);
    }

    /* The first verdict is shown immediately. Making somebody wait four
     * evaluations to see anything at all would look like a lens that had not
     * started. */
    if (!h->primed) {
        h->primed = true;
        h->shown = v->band;
        h->shown_band = v->strongest;
        h->candidate = v->band;
        h->cand_band = v->strongest;
        h->agree = 0;
        return true;
    }

    /* Agreeing with what is already displayed cancels any pending change:
     * a challenger has to be sustained, not merely frequent. */
    if (v->band == h->shown) {
        h->agree = 0;
        h->candidate = h->shown;
        /* The named frequency may still drift while the severity holds, so
         * it is only adopted once it too has settled. */
        if (v->strongest == h->shown_band) {
            h->cand_band = h->shown_band;
        }
        v->strongest = h->shown_band;
        return false;
    }

    if (v->band == h->candidate && v->strongest == h->cand_band) {
        h->agree++;
    } else {
        h->candidate = v->band;
        h->cand_band = v->strongest;
        h->agree = 1;
    }

    if (h->agree >= PAC_CONFIRM) {
        h->shown = h->candidate;
        h->shown_band = h->cand_band;
        h->agree = 0;
        v->band = h->shown;
        v->strongest = h->shown_band;
        return true;
    }

    /* Not yet convinced: keep showing what is on the glass. */
    v->band = h->shown;
    v->strongest = h->shown_band;
    return false;
}
