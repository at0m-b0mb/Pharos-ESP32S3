/* Pharos - RSSI direction finding: walk toward a flagged transmitter
 *
 * Pure C. Once Watch, Karma or Mirage has named a suspect BSSID, the next
 * question a defender asks is physical: *where is it?* Locate answers that the
 * only honest way a single antenna can - not with a bearing, but with a
 * hotter/colder game. Point the device, walk, and it tells you whether the
 * signal is rising or falling. Warmer means closer. It is a metal detector for
 * a radio.
 *
 * Two honesty constraints shape the whole design:
 *
 *   1. RSSI is not distance. Multipath, body-shadowing and antenna
 *      orientation move it by 20 dB without you taking a step. So Locate never
 *      reports metres. It reports a smoothed *closeness* (relative signal, on a
 *      fixed -90..-30 dBm scale) and a *trend*, and it says plainly that
 *      walls and bodies lie.
 *
 *   2. A trend that flickers is worse than no trend - it sends you back and
 *      forth. So the trend only flips after several consistent samples
 *      (hysteresis), and it is driven by the gap between a fast and a slow
 *      exponential average rather than raw jitter. The needle is calm on
 *      purpose.
 *
 * Host-tested: a simulated walk-in must read HOTTER then HERE, a walk-out
 * COLDER, and a noisy stationary hold must stay STEADY rather than twitch.
 */
#ifndef PHAROS_LOCATE_H
#define PHAROS_LOCATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed reference scale for closeness. Nothing about the environment is
 * assumed; these are just the ends of a sensible dBm range for 2.4 GHz. */
#define PL_RSSI_FAR   (-90)
#define PL_RSSI_NEAR  (-30)

typedef enum {
    PL_TREND_COLDER = 0, /* signal falling - walking away    */
    PL_TREND_STEADY,     /* holding - move to learn more     */
    PL_TREND_HOTTER,     /* signal rising - getting closer   */
    PL_TREND_HERE,       /* very strong and steady - on top  */
    PL_TREND_LOST,       /* the target has stopped transmitting */
} pl_trend_t;

typedef struct {
    uint8_t target[6];
    bool has_target;

    int32_t fast_x256;   /* fast EWMA of rssi, 8.8-ish fixed point */
    int32_t slow_x256;   /* slow EWMA, for the slope               */
    int8_t peak;         /* hottest rssi seen                      */
    int8_t last_rssi;
    uint16_t samples;
    uint64_t last_us;

    int8_t trend;        /* current committed pl_trend_t           */
    int8_t pending;      /* candidate trend awaiting confirmation   */
    uint8_t pending_run; /* consecutive samples supporting pending  */
} pl_engine_t;

typedef struct {
    int8_t rssi_now;
    int8_t rssi_smoothed;
    int8_t rssi_peak;
    uint8_t closeness;   /* 0..100 on the fixed scale         */
    uint8_t confidence;  /* 0..100 from sample count + spread */
    pl_trend_t trend;
    uint16_t samples;
    bool locked;         /* enough samples to trust the trend */
    const char *headline;
    uint64_t silent_us; /* how long since the target last spoke */
} pl_verdict_t;

void pl_reset(pl_engine_t *e, const uint8_t target[6]);

/* Feed one frame's RSSI. Frames whose source is not the target are ignored,
 * so the caller may pass the whole stream. */
void pl_observe(pl_engine_t *e, const uint8_t src[6], int8_t rssi, uint64_t t_us);

/* HOW LONG A SILENT TARGET STAYS BELIEVABLE.
 *
 * The needle is driven by exponential averages of RSSI. If the target stops
 * transmitting, nothing new arrives, the averages keep their last values and
 * the trend keeps reporting whatever it last said - forever.
 *
 * On any other lens that would be a stale number on a screen. On this one it
 * is a person walking across a building following a needle that is pointing
 * at a memory, because Locate is the lens you follow WITH YOUR FEET. A frozen
 * HOTTER is worse than no reading at all: it is confidently wrong in a
 * direction that costs shoe leather and trust.
 *
 * Trackers and access points beacon several times a second, so a few seconds
 * of silence is already unusual. Three is long enough to survive ordinary
 * gaps and short enough that nobody walks far on a dead reading. */
#define PL_STALE_US 3000000ull

void pl_evaluate(const pl_engine_t *e, pl_verdict_t *out);

/* The same, told what time it is, so a target that has gone quiet can be
 * reported as quiet instead of as its last opinion. pl_evaluate() forwards
 * here with a `now` of zero, which means "do not age" - preserved for callers
 * that genuinely have no clock, never as a default worth choosing. */
void pl_evaluate_at(const pl_engine_t *e, uint64_t now_us, pl_verdict_t *out);

const char *pl_trend_name(pl_trend_t t);
const char *pl_trend_advice(pl_trend_t t);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_LOCATE_H */
