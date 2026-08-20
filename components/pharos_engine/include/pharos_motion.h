/* Pharos - Motion: has the person holding this actually moved?
 *
 * Pure C, host-tested. The board carries a QMI8658 six-axis IMU that this
 * firmware has never touched, and there is one question it answers that
 * nothing else on the device can.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS, AND IT IS NOT "BECAUSE THERE IS A SENSOR"
 *
 * Vigil asks whether a tracker is travelling WITH YOU. Seeing a tracker means
 * nothing - a cafe at lunchtime has a dozen, all minding their own business in
 * other people's bags. The finding only exists if you moved and it came too.
 *
 * With no GPS, Vigil infers movement from Wi-Fi locale turnover: when the set
 * of audible access points changes over, you are somewhere else. That is a
 * good idea and it has a hole in it, in both directions:
 *
 *   - It reports movement you did not make. Access points switch off at night,
 *     a neighbour reboots a router, you turn around in a large building and
 *     the far half of the estate drops out. The locale turns over while you
 *     sat still, and a tracker that was simply nearby the whole time starts
 *     looking like one that followed you.
 *
 *   - It misses movement you did make. A car park, a rural lay-by, a stairwell
 *     - the places where a planted tracker matters most are frequently the
 *     places with no Wi-Fi to turn over at all. Vigil goes quiet exactly where
 *     it should be loudest.
 *
 * An accelerometer measures the second thing directly and is wrong in
 * completely different circumstances, which is what makes it worth having:
 * two families that fail independently. Same pattern as everywhere else here.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT MAY AND MAY NOT CONCLUDE
 *
 * An IMU measures MOTION, not DISPLACEMENT. Fidgeting in a chair, a phone
 * buzzing on a desk and being handed round a table are all motion and none of
 * them is travel. Double-integrating acceleration to get position is a
 * well-known way to produce confident nonsense - the error grows with the
 * square of time and there is nothing to correct it against.
 *
 * So this engine reports only what the signal can carry:
 *
 *   STILL    - it has not moved. This is the strongest thing here, because it
 *              is the one that can REFUSE a finding: if you did not move,
 *              "it followed you" cannot be concluded at all, whatever the
 *              Wi-Fi locales did.
 *   WALKING  - sustained periodic acceleration in the gait band. Steps are
 *              counted, and steps are the closest thing to displacement that
 *              can be had honestly.
 *   VEHICLE  - sustained motion without a gait rhythm.
 *   UNKNOWN  - not enough samples, or no IMU on this board.
 *
 * There is deliberately no "distance". A step count is offered instead, and
 * the caller is expected to treat it as ordinal - more steps means further -
 * rather than converting it to metres it cannot support.
 */
#ifndef PHAROS_MOTION_H
#define PHAROS_MOTION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PM_UNKNOWN = 0, /* too few samples, or no IMU fitted        */
    PM_STILL,       /* it has not moved                         */
    PM_WALKING,     /* periodic gait, steps countable           */
    PM_VEHICLE,     /* sustained motion with no gait rhythm     */
    PM_STATE_COUNT,
} pm_state_t;

/* Acceleration is taken in milli-g, which is what the QMI8658 reports at its
 * default range and avoids a float conversion in the hot path. One g is
 * PM_ONE_G; at rest the magnitude sits there whatever the orientation. */
#define PM_ONE_G 1000

/* A window of this many samples decides the state. At the 50 Hz the driver
 * runs, that is a little over two seconds - long enough for two or three
 * steps, short enough that stopping is noticed promptly. */
#define PM_WINDOW 128

/* Gait lives between roughly 1.2 and 2.6 steps a second. Anything faster is
 * a vibration, anything slower is not a walk. */
#define PM_STEP_MIN_MS 380
#define PM_STEP_MAX_MS 830

/* How far the filtered magnitude must swing to count as a step, in milli-g.
 * Below this is a hand moving, not a body. */
#define PM_STEP_THRESHOLD 180

/* Stillness: the band the filtered magnitude must stay inside. A device on a
 * desk sits well under this; one held in a hand does not. */
#define PM_STILL_BAND 60

/* How many consecutive in-band intervals make a walk, and how much they may
 * vary and still be one. Four steps at a steady rhythm is a walk; four spread
 * over a range of two is somebody being jostled. */
#define PM_GAIT_RUN 6
#define PM_GAIT_MIN_RUN 4
#define PM_GAIT_SPREAD_PCT 45

typedef struct {
    /* Running gravity estimate, so the engine works at any orientation and
     * survives the device being turned over. */
    int32_t gravity_mg;
    bool gravity_seeded;

    int32_t recent[PM_WINDOW]; /* filtered magnitude, most recent last */
    unsigned n;                /* samples in `recent`                 */
    unsigned head;

    /* Step detection state. */
    bool above;
    uint64_t last_step_us;
    uint32_t steps;
    uint32_t step_intervals_ms; /* running total, for cadence          */
    uint32_t step_interval_n;

    /* THE LAST FEW INTERVALS, BECAUSE A WALK IS PERIODIC.
     *
     * Accepting any interval that lands in the gait band counts road vibration
     * as walking: random crossings occasionally fall 400-800 ms apart, the mean
     * looks like a cadence, and a car ride reads as a stroll. Gait is regular,
     * and regularity is a property of a SEQUENCE of intervals, not of one. */
    uint16_t recent_ms[PM_GAIT_RUN];
    uint8_t recent_n;
    uint8_t recent_head;

    uint64_t last_move_us; /* when it was last anything but STILL   */
    uint64_t last_us;
    bool present; /* an IMU actually answered */
} pm_engine_t;

typedef struct {
    pm_state_t state;
    uint32_t steps;         /* since reset                            */
    uint16_t cadence_ppm;   /* steps per minute, 0 when not walking   */
    uint32_t still_for_s;   /* how long it has been STILL, 0 if moving*/
    int32_t swing_mg;       /* peak-to-peak in the window             */
    bool present;           /* false when there is no IMU to ask      */
    const char *headline;
} pm_verdict_t;

void pm_reset(pm_engine_t *e);

/* Tell the engine whether an IMU answered. Without this it reports UNKNOWN
 * forever rather than STILL, because "nothing is moving" and "nothing is
 * measuring" must never be the same answer. */
void pm_set_present(pm_engine_t *e, bool present);

/* One accelerometer sample, in milli-g on each axis. */
void pm_observe(pm_engine_t *e, int32_t x_mg, int32_t y_mg, int32_t z_mg,
                uint64_t t_us);

void pm_evaluate(const pm_engine_t *e, uint64_t now_us, pm_verdict_t *out);

const char *pm_state_name(pm_state_t s);

/* HAS THIS PERSON MOVED ENOUGH FOR "IT FOLLOWED ME" TO MEAN ANYTHING?
 *
 * The gate Vigil needs. True once enough steps have been taken, or once
 * vehicle motion has been sustained, since the reference point. A caller that
 * gets false here should not be concluding anybody is being followed - not
 * scoring it lower, refusing it. */
bool pm_has_travelled(const pm_engine_t *e, uint32_t since_steps);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_MOTION_H */
