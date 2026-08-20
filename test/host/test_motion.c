/* Host tests for the motion engine.
 *
 * The failure that matters here is not a miscounted step. It is the engine
 * being confidently wrong in a way that changes a stalking verdict: saying
 * somebody walked when they sat still would let Vigil conclude a tracker
 * followed them across a room they never left.
 *
 * So the tests are built from synthesised accelerometer traces - a desk, a
 * walk, a car, a hand fidgeting - and assert what the engine may conclude from
 * each, including the cases where the honest answer is "I cannot tell".
 */
#include <math.h>
#include <string.h>

#include "pharos_motion.h"
#include "test_support.h"

#define T0 1000000000ull
#define HZ 50u
#define DT_US (1000000ull / HZ)

/* Feed `secs` seconds of a trace. `step_hz` of 0 means no gait. */
static uint64_t feed(pm_engine_t *e, uint64_t t, double secs, double step_hz,
                     double amp_mg, double noise_mg)
{
    const unsigned n = (unsigned)(secs * (double)HZ);
    for (unsigned i = 0; i < n; i++) {
        const double s = (double)i / (double)HZ;
        /* A step is a sharp vertical impulse, not a sine - but a sine at the
         * gait frequency exercises the same threshold crossings and is
         * reproducible, which matters more for a regression test. */
        double a = 0.0;
        if (step_hz > 0.0) {
            a = amp_mg * sin(2.0 * 3.14159265 * step_hz * s);
        }
        /* Deterministic pseudo-noise, so a failure is reproducible. */
        const double nz = noise_mg * sin(s * 137.0);
        pm_observe(e, 0, 0, (int32_t)((double)PM_ONE_G + a + nz), t);
        t += DT_US;
    }
    return t;
}

/* NOTHING IS MOVING AND NOTHING IS MEASURING ARE DIFFERENT ANSWERS. */
static void test_motion_absent_is_not_still(void)
{
    banner("motion: no sensor reads UNKNOWN, never STILL");
    pm_engine_t e;
    pm_reset(&e);
    pm_set_present(&e, false);

    uint64_t t = T0;
    t = feed(&e, t, 10.0, 1.8, 300.0, 5.0); /* a vigorous walk, ignored */

    pm_verdict_t v;
    pm_evaluate(&e, t, &v);
    CHECK(v.state == PM_UNKNOWN, "an absent IMU has no opinion");
    CHECK(!v.present, "and says so");
    CHECK_EQ(v.steps, 0);

    /* And critically: absence must not block the callers that gate on it.
     * A board with no IMU should behave as it did before there was one. */
    CHECK(pm_has_travelled(&e, 0), "absence does not veto a following verdict");
}

static void test_motion_desk_is_still(void)
{
    banner("motion: a device on a desk is STILL, and stays STILL");
    pm_engine_t e;
    pm_reset(&e);
    pm_set_present(&e, true);

    uint64_t t = T0;
    t = feed(&e, t, 20.0, 0.0, 0.0, 8.0); /* only sensor noise */

    pm_verdict_t v;
    pm_evaluate(&e, t, &v);
    CHECK(v.state == PM_STILL, "a desk reads still (swing %d mg)",
          (int)v.swing_mg);
    CHECK(v.swing_mg < PM_STILL_BAND, "the swing is inside the still band");
    CHECK_EQ(v.steps, 0);
    CHECK(v.still_for_s > 0, "and it knows how long for");

    /* THE ONE THAT MATTERS: sitting still is not travelling, so a following
     * verdict cannot be drawn no matter what the Wi-Fi locales did. */
    CHECK(!pm_has_travelled(&e, 0), "sitting still is not travelling");
}

static void test_motion_walking_counts_steps(void)
{
    banner("motion: a walk is recognised and its steps counted");
    pm_engine_t e;
    pm_reset(&e);
    pm_set_present(&e, true);

    /* 1.8 Hz is an ordinary walking cadence - about 108 steps a minute. */
    uint64_t t = T0;
    t = feed(&e, t, 30.0, 1.8, 320.0, 10.0);

    pm_verdict_t v;
    pm_evaluate(&e, t, &v);
    CHECK(v.state == PM_WALKING, "thirty seconds of gait reads as walking");
    CHECK(v.steps >= 40u, "and counts roughly the right number (%u)",
          (unsigned)v.steps);
    CHECK(v.steps <= 60u, "without inventing extras (%u)", (unsigned)v.steps);
    CHECK(v.cadence_ppm >= 90u && v.cadence_ppm <= 130u,
          "cadence is about right (%u ppm)", (unsigned)v.cadence_ppm);
    CHECK(pm_has_travelled(&e, 0), "and that counts as having travelled");
}

/* A HAND WAVING IS NOT A WALK, and this is the failure that would matter:
 * somebody fidgeting with the device while sitting still must not accumulate
 * steps that later license a stalking verdict. */
static void test_motion_fidget_is_not_a_walk(void)
{
    banner("motion: fast shaking is not gait");
    pm_engine_t e;
    pm_reset(&e);
    pm_set_present(&e, true);

    /* 6 Hz - far above any gait, which is what a hand does. */
    uint64_t t = T0;
    t = feed(&e, t, 20.0, 6.0, 400.0, 10.0);

    pm_verdict_t v;
    pm_evaluate(&e, t, &v);
    CHECK(v.state != PM_WALKING, "6 Hz is not walking (got %s)",
          pm_state_name(v.state));
    CHECK(v.steps < 5u, "and barely any steps are counted (%u)",
          (unsigned)v.steps);
    CHECK(!pm_has_travelled(&e, 0), "so it does not count as travel");

    /* And the slow end: a gentle sway is not a walk either. */
    pm_engine_t slow;
    pm_reset(&slow);
    pm_set_present(&slow, true);
    uint64_t ts = T0;
    ts = feed(&slow, ts, 30.0, 0.5, 400.0, 10.0);
    pm_verdict_t sv;
    pm_evaluate(&slow, ts, &sv);
    CHECK(sv.state != PM_WALKING, "0.5 Hz is not walking either (got %s)",
          pm_state_name(sv.state));
    CHECK(!pm_has_travelled(&slow, 0), "and is not travel");
}

static void test_motion_vehicle(void)
{
    banner("motion: sustained motion with no gait reads as moving, not walking");
    pm_engine_t e;
    pm_reset(&e);
    pm_set_present(&e, true);

    /* Road vibration: well above the still band, no gait rhythm. */
    uint64_t t = T0;
    t = feed(&e, t, 20.0, 9.0, 150.0, 40.0);

    pm_verdict_t v;
    pm_evaluate(&e, t, &v);
    CHECK(v.state == PM_VEHICLE, "reads as moving (got %s)",
          pm_state_name(v.state));
    CHECK(v.swing_mg >= PM_STILL_BAND, "the swing says something is happening");
    CHECK(strstr(v.headline, "no gait") != NULL,
          "and it is honest that it cannot name the cause");
}

/* Stopping has to be noticed promptly, or the gate stays open after somebody
 * sits down and a stationary tracker starts looking like a follower. */
static void test_motion_stopping_is_noticed(void)
{
    banner("motion: it notices when the walking stops");
    pm_engine_t e;
    pm_reset(&e);
    pm_set_present(&e, true);

    uint64_t t = T0;
    t = feed(&e, t, 20.0, 1.8, 320.0, 10.0);
    pm_verdict_t v;
    pm_evaluate(&e, t, &v);
    CHECK(v.state == PM_WALKING, "walking first");

    t = feed(&e, t, 10.0, 0.0, 0.0, 8.0); /* sits down */
    pm_evaluate(&e, t, &v);
    CHECK(v.state == PM_STILL, "then still within ten seconds");
    CHECK(v.steps >= 25u, "the steps already taken are not forgotten");
}

/* The device works in any orientation, and being turned over must not be
 * mistaken for having gone somewhere. */
static void test_motion_orientation_independent(void)
{
    banner("motion: gravity is estimated, so orientation does not matter");
    pm_engine_t e;
    pm_reset(&e);
    pm_set_present(&e, true);

    uint64_t t = T0;
    /* Lying on its side: gravity on X instead of Z, still one g total. */
    for (unsigned i = 0; i < 20u * HZ; i++) {
        pm_observe(&e, PM_ONE_G, 0, 0, t);
        t += DT_US;
    }
    pm_verdict_t v;
    pm_evaluate(&e, t, &v);
    CHECK(v.state == PM_STILL, "on its side and still (swing %d)",
          (int)v.swing_mg);
    CHECK(!pm_has_travelled(&e, 0), "and has not travelled");
}

static void test_motion_travel_gate(void)
{
    banner("motion: the gate Vigil needs opens only after real travel");
    pm_engine_t e;
    pm_reset(&e);
    pm_set_present(&e, true);

    uint64_t t = T0;
    t = feed(&e, t, 6.0, 1.8, 320.0, 10.0); /* a few steps only */
    CHECK(!pm_has_travelled(&e, 0), "a few paces is not travel (%u steps)",
          (unsigned)e.steps);

    t = feed(&e, t, 30.0, 1.8, 320.0, 10.0);
    CHECK(pm_has_travelled(&e, 0), "half a minute of walking is");

    /* And relative to a mark: twenty more steps than when we started looking. */
    const uint32_t mark = e.steps;
    CHECK(!pm_has_travelled(&e, mark), "no further travel since the mark");
    t = feed(&e, t, 30.0, 1.8, 320.0, 10.0);
    CHECK(pm_has_travelled(&e, mark), "and then there is");
    (void)t;
}

static void test_motion_degenerate(void)
{
    banner("motion: NULLs and empty state are survivable");
    pm_verdict_t v;
    pm_evaluate(NULL, T0, &v);
    CHECK(v.state == PM_UNKNOWN, "a NULL engine has no opinion");
    pm_reset(NULL);
    pm_set_present(NULL, true);
    pm_observe(NULL, 0, 0, 0, T0);
    CHECK(pm_has_travelled(NULL, 0), "and a NULL engine does not veto");

    pm_engine_t e;
    pm_reset(&e);
    pm_set_present(&e, true);
    pm_evaluate(&e, T0, &v);
    CHECK(v.state == PM_UNKNOWN, "no samples yet is not STILL");
    CHECK(strstr(v.headline, "settling") != NULL, "and it says it is settling");
}

void test_motion(void)
{
    test_motion_absent_is_not_still();
    test_motion_desk_is_still();
    test_motion_walking_counts_steps();
    test_motion_fidget_is_not_a_walk();
    test_motion_vehicle();
    test_motion_stopping_is_noticed();
    test_motion_orientation_independent();
    test_motion_travel_gate();
    test_motion_degenerate();
}
