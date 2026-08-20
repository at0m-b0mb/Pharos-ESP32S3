/* Pharos - Motion. See pharos_motion.h for what this may and may not conclude;
 * the short version is that it reports motion, never displacement. */
#include "pharos_motion.h"

#include <string.h>

/* Integer square root, so the hot path has no float and the host and the
 * device agree bit for bit. */
static uint32_t isqrt32(uint32_t v)
{
    uint32_t rem = 0, root = 0;
    for (int i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | (v >> 30);
        v <<= 2;
        if (root < rem) {
            rem -= ++root;
            root++;
        }
    }
    return root >> 1;
}

void pm_reset(pm_engine_t *e)
{
    if (!e) {
        return;
    }
    const bool present = e->present;
    memset(e, 0, sizeof(*e));
    e->present = present;
}

void pm_set_present(pm_engine_t *e, bool present)
{
    if (e) {
        e->present = present;
    }
}

void pm_observe(pm_engine_t *e, int32_t x_mg, int32_t y_mg, int32_t z_mg,
                uint64_t t_us)
{
    if (!e || !e->present) {
        return;
    }
    e->last_us = t_us;

    /* Magnitude, so orientation does not matter. A device face up, face down
     * or on its edge all read one g at rest, and the whole engine works the
     * same in a pocket as on a desk. */
    const int64_t sq = (int64_t)x_mg * x_mg + (int64_t)y_mg * y_mg +
                       (int64_t)z_mg * z_mg;
    const int32_t mag = (int32_t)isqrt32((uint32_t)(sq > 0xFFFFFFFFll
                                                        ? 0xFFFFFFFFll
                                                        : sq));

    /* GRAVITY IS ESTIMATED, NOT ASSUMED.
     *
     * Subtracting a hard-coded 1000 mg would work until the part's zero-g
     * offset, temperature or a slightly-off scale factor moved it - and then
     * a stationary device would read a permanent bias and never be STILL
     * again. A slow leak towards the current magnitude tracks all of that
     * while being far too slow to follow a step. */
    if (!e->gravity_seeded) {
        e->gravity_mg = mag;
        e->gravity_seeded = true;
    } else {
        e->gravity_mg += (mag - e->gravity_mg) / 64;
    }
    const int32_t filtered = mag - e->gravity_mg;

    /* When stillness ended, for still_for_s. Tracked here rather than derived
     * in evaluate() because the answer is "since when", which only the sample
     * stream knows. */
    const int32_t swing = (filtered < 0) ? -filtered : filtered;
    if (swing > PM_STILL_BAND / 2 || !e->last_move_us) {
        e->last_move_us = t_us;
    }

    e->recent[e->head] = filtered;
    e->head = (e->head + 1u) % PM_WINDOW;
    if (e->n < PM_WINDOW) {
        e->n++;
    }

    /* STEPS: a threshold crossing with a refractory period.
     *
     * Counting every peak would count the shock of setting the device down,
     * and counting zero crossings would count noise. Requiring the swing to
     * exceed a body-sized threshold AND arrive in the gait band rejects both -
     * a hand waving is too fast, a slow sway too slow. */
    if (!e->above && filtered > PM_STEP_THRESHOLD) {
        e->above = true;
        if (e->last_step_us) {
            const uint64_t dt_us = t_us - e->last_step_us;
            const uint32_t dt_ms = (uint32_t)(dt_us / 1000ull);
            if (dt_ms >= PM_STEP_MIN_MS && dt_ms <= PM_STEP_MAX_MS) {
                e->steps++;
                e->step_intervals_ms += dt_ms;
                e->step_interval_n++;
                e->recent_ms[e->recent_head] = (uint16_t)dt_ms;
                e->recent_head = (uint8_t)((e->recent_head + 1u) % PM_GAIT_RUN);
                if (e->recent_n < PM_GAIT_RUN) {
                    e->recent_n++;
                }
            } else {
                /* Out of band breaks the rhythm, and a broken rhythm is the
                 * whole difference between a walk and a series of jolts. */
                e->recent_n = 0;
                e->recent_head = 0;
            }
            /* Either way this is the new reference - two fast jolts must not
             * later combine into one plausible interval. */
            e->last_step_us = t_us;
        } else {
            e->last_step_us = t_us;
        }
    } else if (e->above && filtered < PM_STEP_THRESHOLD / 3) {
        /* Hysteresis on the way down, so one peak is one crossing. */
        e->above = false;
    }
}

void pm_evaluate(const pm_engine_t *e, uint64_t now_us, pm_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = PM_UNKNOWN;
    out->headline = "no motion sensor";
    if (!e) {
        return;
    }
    out->present = e->present;
    out->steps = e->steps;
    if (!e->present) {
        return;
    }
    if (e->n < PM_WINDOW / 4u) {
        out->headline = "settling";
        return;
    }

    /* Peak-to-peak over the window: the one number that separates a device on
     * a table from one being carried. */
    int32_t lo = e->recent[0], hi = e->recent[0];
    for (unsigned i = 1; i < e->n; i++) {
        if (e->recent[i] < lo) lo = e->recent[i];
        if (e->recent[i] > hi) hi = e->recent[i];
    }
    out->swing_mg = hi - lo;

    /* Cadence from the intervals actually accepted as steps. */
    if (e->step_interval_n) {
        const uint32_t mean_ms = e->step_intervals_ms / e->step_interval_n;
        if (mean_ms) {
            out->cadence_ppm = (uint16_t)(60000u / mean_ms);
        }
    }

    /* Recent steps are what make it a walk. A step counted a minute ago says
     * nothing about now. */
    const bool stepping =
        e->last_step_us && (now_us > e->last_step_us) &&
        ((now_us - e->last_step_us) < 3000000ull);

    /* AND THE RHYTHM HAS TO BE STEADY.
     *
     * This is what separates a walk from a car. Road vibration throws
     * threshold crossings that occasionally land 400-800 ms apart; average
     * them and you get a plausible cadence from a vehicle sitting on a
     * motorway. Gait is regular, so the test is on the SPREAD of consecutive
     * intervals, not on their mean. */
    bool rhythmic = false;
    if (e->recent_n >= PM_GAIT_MIN_RUN) {
        uint16_t lo_ms = e->recent_ms[0], hi_ms = e->recent_ms[0];
        for (unsigned i = 1; i < e->recent_n; i++) {
            if (e->recent_ms[i] < lo_ms) lo_ms = e->recent_ms[i];
            if (e->recent_ms[i] > hi_ms) hi_ms = e->recent_ms[i];
        }
        rhythmic = lo_ms &&
                   ((uint32_t)(hi_ms - lo_ms) * 100u <=
                    (uint32_t)lo_ms * PM_GAIT_SPREAD_PCT);
    }

    if (out->swing_mg < PM_STILL_BAND) {
        out->state = PM_STILL;
        out->headline = "not moving";
        if (e->last_move_us && now_us > e->last_move_us) {
            out->still_for_s = (uint32_t)((now_us - e->last_move_us) / 1000000ull);
        }
    } else if (stepping && rhythmic && out->cadence_ppm >= 72u &&
               out->cadence_ppm <= 158u) {
        out->state = PM_WALKING;
        out->headline = "walking";
    } else {
        /* Moving, with no gait in it. A vehicle, a lift, or somebody turning
         * the thing over in their hands - the engine does not pretend to tell
         * those apart, and says the honest thing. */
        out->state = PM_VEHICLE;
        out->headline = "moving, no gait";
    }
}

const char *pm_state_name(pm_state_t s)
{
    switch (s) {
    case PM_STILL:   return "still";
    case PM_WALKING: return "walking";
    case PM_VEHICLE: return "moving";
    case PM_UNKNOWN:
    default:         return "unknown";
    }
}

bool pm_has_travelled(const pm_engine_t *e, uint32_t since_steps)
{
    if (!e || !e->present) {
        /* NO SENSOR IS NOT NO MOVEMENT.
         *
         * A board without an IMU must not have every "did you move" question
         * answered "no" - that would silently disable the callers that use
         * this as a gate. Absent means unknown, and unknown must not block. */
        return true;
    }
    if (e->steps < since_steps) {
        return false;
    }
    /* At least twenty steps beyond the reference: enough that somebody has
     * genuinely gone somewhere rather than shifted in a chair. */
    return (e->steps - since_steps) >= 20u;
}
