/* Pharos - clock skew: which physical radio is behind a name
 *
 * Twin decides whether an access point is an impostor from what it SAYS: the
 * security posture it advertises, how many radios claim one name, whether it
 * behaves like a roaming cell. Those are good heuristics and they are all
 * heuristics, because every field an attacker has to fill in is a field an
 * attacker can fill in correctly.
 *
 * This is not that. Every 802.11 beacon carries a 64-bit TSF timestamp - the
 * microseconds the access point has been running, counted by its own crystal.
 * No two crystals run at exactly the same rate: manufacturing tolerance puts
 * them tens of parts per million apart, and that drift is a property of the
 * silicon rather than of the frame. An attacker writes the BSSID, the SSID and
 * the capability bits. They do not write their oscillator.
 *
 * So: measure how fast a BSSID's clock runs against ours. If the answer
 * CHANGES, the radio behind that name changed - and a real access point's
 * crystal does not suddenly start running at a different rate. That is the
 * closest thing to proof this device can obtain about an evil twin, and it is
 * the same argument used against a spoofed source MAC in pharos_attrib.h.
 *
 * ---------------------------------------------------------------------------
 * WHY THE MINIMUM AND NOT THE MEAN
 *
 * The measurement is (their clock) minus (our clock) over time, and the slope
 * of that is the skew. Both numbers are microseconds, and the difference of
 * two of them tens of seconds apart is what carries the signal - so the noise
 * in each one matters enormously.
 *
 * Our timestamp is taken when the driver hands us the frame, which is after
 * reception, after interrupt latency, after whatever else the CPU was doing.
 * That delay is always POSITIVE and highly variable: hundreds of microseconds
 * on a busy channel. Averaging it does not remove it, because it is not
 * symmetric noise - it is a one-sided tax.
 *
 * The MINIMUM offset seen in a window is therefore much closer to the truth
 * than the mean, because the smallest delay is the one where least got in the
 * way. This is the standard estimator for clock offset over a jittery path,
 * and using it is the difference between measuring a crystal and measuring
 * how busy the CPU was.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT CANNOT DO, STATED UP FRONT
 *
 * A determined attacker can read the real AP's TSF and synchronise to it; the
 * literature documents exactly this as the way around clock-skew detection.
 * So a match is NOT proof of innocence, and this engine never reports one as
 * such. It reports only the positive finding - a skew that changed - and says
 * nothing at all when the skew is stable.
 *
 * It also needs TIME. Ten parts per million over ten seconds is a hundred
 * microseconds, which is inside the reception jitter; over a minute it is six
 * hundred, which is not. So a verdict before PSK_MIN_SPAN_US is refused
 * rather than guessed, and the caller is told it is still measuring.
 */
#ifndef PHAROS_SKEW_H
#define PHAROS_SKEW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSK_MAX_APS 16

/* Beacons are bucketed and the minimum offset in each bucket is what the fit
 * uses.
 *
 * THE BUCKET IS FOUR SECONDS WIDE, NOT ONE. Sixteen one-second buckets hold
 * fifteen seconds of history, and the header note above says plainly that ten
 * ppm over ten seconds is inside the reception jitter - so a ring that could
 * only ever offer a fifteen-second span could not satisfy its own minimum. A
 * first version shipped exactly that contradiction and the tests caught it.
 *
 * Four-second buckets give a full minute of memory in the same sixteen slots.
 * Ten ppm across that is six hundred microseconds, which is comfortably
 * outside the jitter - which is the whole reason the span matters. */
#define PSK_SLOTS 16
#define PSK_BUCKET_S 4

/* How long a baseline must be before any skew is reported. See the note
 * above: shorter than this and the answer is reception jitter. */
#define PSK_MIN_SPAN_US 20000000ull

/* How many bucketed samples the fit needs. */
#define PSK_MIN_SAMPLES 6

/* A change bigger than this means a different crystal, in parts per million.
 *
 * Crystals used in consumer access points are specified at +/-20 ppm or so
 * and drift by a couple of ppm with temperature over minutes. Twenty-five is
 * comfortably outside thermal drift and comfortably inside the spread between
 * two independent parts. */
#define PSK_CHANGE_PPM 25

typedef struct {
    uint8_t bssid[6];
    bool used;

    /* Per-second buckets: the smallest (tsf - local) seen in each. */
    int64_t min_off[PSK_SLOTS];
    uint64_t slot_local[PSK_SLOTS];
    bool slot_used[PSK_SLOTS];
    uint8_t slot_n;

    /* The skew established the first time there was enough evidence, kept so
     * a later measurement has something to disagree with. */
    int32_t base_ppm;
    bool have_base;

    int32_t last_ppm;
    bool have_last;

    /* Set once and never cleared: a radio swap is a fact about the session,
     * not a condition that goes away when the impostor stops beaconing. */
    bool changed;
    int32_t change_from, change_to;
} psk_ap_t;

typedef struct {
    psk_ap_t ap[PSK_MAX_APS];
    unsigned n;
} psk_engine_t;

typedef struct {
    bool measuring;    /* seen beacons, not yet a long enough baseline */
    bool have_skew;
    int32_t ppm;

    bool changed;      /* the crystal behind this name changed          */
    int32_t from_ppm, to_ppm;
    uint32_t span_s;   /* how long the baseline actually is             */
    unsigned samples;
} psk_verdict_t;

void psk_reset(psk_engine_t *e);

/* One beacon. `tsf` is the 64-bit timestamp from the frame body, `local_us`
 * the receiver's own clock when it was handed over. */
void psk_observe(psk_engine_t *e, const uint8_t bssid[6], uint64_t tsf,
                 uint64_t local_us);

/* What is known about one BSSID. Never claims a match means anything; see the
 * note on synchronisation above. */
void psk_evaluate(const psk_engine_t *e, const uint8_t bssid[6],
                  psk_verdict_t *out);

/* Has ANY access point in view changed its crystal? The cheap question for a
 * lens that wants one bit for its evidence family. */
bool psk_any_changed(const psk_engine_t *e);

/* The first access point whose crystal changed, for a caller that wants to
 * report the detail rather than the bit. Returns false when none has. */
bool psk_first_changed(const psk_engine_t *e, psk_verdict_t *out,
                       uint8_t bssid[6]);

/* How many BSSIDs currently have a usable baseline, and how many are still
 * measuring - so a screen can say "measuring" instead of "nothing found",
 * which are different answers. */
void psk_progress(const psk_engine_t *e, unsigned *ready, unsigned *measuring);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_SKEW_H */
