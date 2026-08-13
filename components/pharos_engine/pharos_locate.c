#include "pharos_locate.h"

#include <string.h>

/* Fixed-point EWMA weights. fast reacts in a few samples; slow lags it, and
 * the sign of (fast - slow) is the trend. Kept in 1/256 units so integer-only
 * smoothing does not quantise away small, real movements. */
#define PL_FAST_SHIFT 2 /* alpha = 1/4  */
#define PL_SLOW_SHIFT 4 /* alpha = 1/16 */

/* How many consecutive samples must agree before the committed trend flips.
 * This is the anti-flicker: the needle does not chase jitter. */
#define PL_CONFIRM 4

/* dB the fast/slow gap must exceed to count as movement (in whole dB). */
#define PL_MOVE_DB 2

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

void pl_reset(pl_engine_t *e, const uint8_t target[6])
{
    if (!e) {
        return;
    }
    memset(e, 0, sizeof(*e));
    if (target) {
        memcpy(e->target, target, 6);
        e->has_target = true;
    }
    e->peak = -128;
    e->trend = PL_TREND_STEADY;
    e->pending = PL_TREND_STEADY;
}

void pl_observe(pl_engine_t *e, const uint8_t src[6], int8_t rssi, uint64_t t_us)
{
    if (!e || !e->has_target || !src || !mac_eq(src, e->target)) {
        return;
    }
    const int32_t r256 = (int32_t)rssi * 256;

    if (e->samples == 0) {
        e->fast_x256 = r256;
        e->slow_x256 = r256;
    } else {
        e->fast_x256 += (r256 - e->fast_x256) >> PL_FAST_SHIFT;
        e->slow_x256 += (r256 - e->slow_x256) >> PL_SLOW_SHIFT;
    }

    e->last_rssi = rssi;
    e->last_us = t_us;
    if (rssi > e->peak) {
        e->peak = rssi;
    }
    if (e->samples < 0xFFFF) {
        e->samples++;
    }

    /* Decide the candidate trend from the fast/slow gap. */
    const int gap_db = (int)((e->fast_x256 - e->slow_x256) / 256);
    const int smoothed = (int)(e->fast_x256 / 256);

    int candidate;
    if (smoothed >= (PL_RSSI_NEAR - 6) && gap_db > -PL_MOVE_DB) {
        /* Strong signal and not actively cooling: you are on top of it. This
         * takes priority over HOTTER, because coming off a walk-in the slow
         * average lags and would otherwise keep reporting "warmer" long after
         * you have arrived. A clear cooling (gap below -MOVE) still wins, so a
         * strong-but-receding source reads COLDER as it should. */
        candidate = PL_TREND_HERE;
    } else if (gap_db >= PL_MOVE_DB) {
        candidate = PL_TREND_HOTTER;
    } else if (gap_db <= -PL_MOVE_DB) {
        candidate = PL_TREND_COLDER;
    } else {
        candidate = PL_TREND_STEADY;
    }

    /* Hysteresis: a new candidate must persist for PL_CONFIRM samples before
     * it replaces the committed trend. STEADY commits immediately, because
     * "stop, I have lost the movement" should never lag. */
    if (candidate == e->trend) {
        e->pending = candidate;
        e->pending_run = 0;
    } else if (candidate == e->pending) {
        if (e->pending_run < 255) {
            e->pending_run++;
        }
        if (candidate == PL_TREND_STEADY || e->pending_run >= PL_CONFIRM) {
            e->trend = (int8_t)candidate;
            e->pending_run = 0;
        }
    } else {
        e->pending = (int8_t)candidate;
        e->pending_run = 1;
        if (candidate == PL_TREND_STEADY) {
            e->trend = PL_TREND_STEADY;
        }
    }
}

void pl_evaluate(const pl_engine_t *e, pl_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "";
    if (!e || e->samples == 0) {
        out->trend = PL_TREND_STEADY;
        out->headline = e && e->has_target ? "Listening for the target..."
                                           : "No target selected";
        return;
    }

    const int smoothed = (int)(e->fast_x256 / 256);
    out->rssi_now = e->last_rssi;
    out->rssi_smoothed = (int8_t)clampi(smoothed, -128, 0);
    out->rssi_peak = e->peak;
    out->samples = e->samples;
    out->trend = (pl_trend_t)e->trend;

    /* Closeness: the smoothed rssi mapped onto the fixed FAR..NEAR scale. Not
     * distance - relative signal. The gauge label says so. */
    const int span = PL_RSSI_NEAR - PL_RSSI_FAR; /* 60 dB */
    out->closeness = (uint8_t)clampi(((smoothed - PL_RSSI_FAR) * 100) / span, 0, 100);

    /* Confidence grows with samples and is docked when the last reading sits
     * far below the peak (a body or wall just cut the signal - the trend is
     * briefly unreliable). */
    int conf = (int)(e->samples < 40 ? (e->samples * 100 / 40) : 100);
    const int below_peak = (int)e->peak - smoothed;
    if (below_peak > 12) {
        conf -= clampi((below_peak - 12) * 3, 0, 40);
    }
    out->confidence = (uint8_t)clampi(conf, 0, 100);
    out->locked = e->samples >= 8;

    if (!out->locked) {
        out->headline = "Getting a fix...";
        return;
    }
    switch (out->trend) {
    case PL_TREND_HERE:
        out->headline = "On top of it";
        break;
    case PL_TREND_HOTTER:
        out->headline = "Warmer - keep going";
        break;
    case PL_TREND_COLDER:
        out->headline = "Colder - turn around";
        break;
    case PL_TREND_STEADY:
    default:
        out->headline = "Steady - take a few steps";
        break;
    }
}

const char *pl_trend_name(pl_trend_t t)
{
    switch (t) {
    case PL_TREND_COLDER: return "COLDER";
    case PL_TREND_STEADY: return "STEADY";
    case PL_TREND_HOTTER: return "WARMER";
    case PL_TREND_HERE:   return "HERE";
    default:              return "?";
    }
}

const char *pl_trend_advice(pl_trend_t t)
{
    switch (t) {
    case PL_TREND_COLDER:
        return "Signal is falling. You are walking away from it - turn back.";
    case PL_TREND_STEADY:
        return "No clear change. Take a few steps in one direction to see "
               "which way the signal moves.";
    case PL_TREND_HOTTER:
        return "Signal is rising. Keep going, slowly - and remember walls and "
               "bodies bend it, so trust the trend over any single reading.";
    case PL_TREND_HERE:
        return "Signal is strong and steady. The transmitter is close. RSSI is "
               "not distance, so sweep slowly to find the peak.";
    default:
        return "";
    }
}
