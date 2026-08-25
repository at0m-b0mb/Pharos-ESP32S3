/* Pharos - the acoustic engine: inaudible beacons, and nothing else
 *
 * Pure C. No ESP-IDF, no allocation, no floating point in the decision path,
 * so the whole thing is exercised on a laptop by test/host/test_acoustic.c
 * before it ever meets a microphone.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT LOOKS FOR
 *
 * Ultrasonic beacons. There is a real, deployed advertising technique in which
 * a shop, a television advert or a web page emits a short tone just above the
 * top of human hearing - typically 18 to 20 kHz - carrying an identifier. An
 * app on a phone in the room hears it through the microphone and reports that
 * this device was in that place, or saw that advert. It is cross-device
 * tracking that travels through the air, it is completely inaudible, and the
 * person being tracked has no way to notice it.
 *
 * That is exactly the shape of thing this project exists to make visible: a
 * signal aimed at you, that you cannot perceive, that you never agreed to. It
 * belongs next to the Wi-Fi lenses for the same reason they belong to each
 * other.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT REFUSES TO DO, STRUCTURALLY
 *
 * It never records, stores, buffers or reconstructs audio. Not as a policy -
 * as a matter of what this code physically contains. Samples are consumed by
 * pac_observe() and converted immediately into a handful of band ENERGIES;
 * there is no sample buffer in pac_engine_t to inspect, exfiltrate or subpoena,
 * and there is no API that returns one. Speech cannot be recovered from four
 * energy figures per 20 ms window any more than a photograph can be recovered
 * from its average brightness.
 *
 * The audible band is measured for exactly one reason - to know whether the
 * microphone is working and the room is not silent, so that "no beacon" can be
 * distinguished from "no signal at all". Its LEVEL is used. Its content is not
 * examined, and there is nothing here that could examine it.
 *
 * A microphone on a security device is a serious thing to add. It is added
 * this way, or not at all.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT CANNOT DO
 *
 * The ES7210 and the codec path roll off near the top of their range, and this
 * runs at 48 kHz, so anything above about 22 kHz is beyond Nyquist and simply
 * does not exist here. Between roughly 20 and 22 kHz the microphone's own
 * response is falling away and an absent beacon and a deaf receiver look the
 * same. The engine therefore reports its confidence and refuses to treat the
 * top band as proof - see PAC_NOTE_EDGE_OF_HEARING.
 */
#ifndef PHAROS_ACOUSTIC_H
#define PHAROS_ACOUSTIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bands. One audible reference plus four ultrasonic probes across the
 * range beacons are actually deployed in. Narrow on purpose: a beacon is a
 * TONE, and a narrow probe rejects the broadband noise that a room, a fan or
 * a hand on a desk produces. */
typedef enum {
    PAC_BAND_AUDIBLE = 0, /*  1 kHz - is the microphone alive at all?  */
    PAC_BAND_18K,         /* 18 kHz - the common beacon band           */
    PAC_BAND_19K,
    PAC_BAND_20K,
    PAC_BAND_21K,         /* near the edge of what this path can hear  */
    PAC_BAND_COUNT,
} pac_band_t;

#define PAC_SLOTS 32 /* ~one per observation window; the persistence history */

typedef enum {
    PAC_BAND_QUIET = 0,  /*  0-19  nothing above the room             */
    PAC_BAND_TRACE,      /* 20-39  something, but it comes and goes   */
    PAC_BAND_PRESENT,    /* 40-59  a tone is there                    */
    PAC_BAND_PERSISTENT, /* 60-74  it keeps coming back               */
    PAC_BAND_BEACON,     /* 75-100 inaudible beacon likely            */
} pac_verdict_band_t;

/* Evidence families, same shape as every other engine here. */
#define PAC_FAM_LEVEL      (1u << 0) /* it is loud enough to be deliberate */
#define PAC_FAM_NARROW     (1u << 1) /* it is a tone, not noise            */
#define PAC_FAM_PERSISTENT (1u << 2) /* it repeats over time               */

#define PAC_NOTE_DEAF            (1u << 0) /* the audible band is silent too   */
#define PAC_NOTE_EDGE_OF_HEARING (1u << 1) /* strongest band is the 21k one    */
#define PAC_NOTE_SHORT           (1u << 2) /* not enough windows yet           */
#define PAC_NOTE_LOUD_ROOM       (1u << 3) /* audible band is high; harmonics  */

typedef struct {
    uint8_t score;
    uint8_t ceiling;
    uint8_t families;
    uint8_t notes;
    pac_verdict_band_t band;

    uint8_t c_level, c_narrow, c_persist;

    /* Per-band level in dBFS, negated and clamped to 0..120 so it stays an
     * integer: 0 means full scale, 120 means silence. */
    uint8_t level[PAC_BAND_COUNT];
    pac_band_t strongest;    /* strongest ULTRASONIC band                  */
    uint8_t windows;         /* how many observation windows contributed   */
    uint8_t hits;            /* of those, how many had the tone present    */
    uint8_t duty_pct;        /* hits as a percentage - the persistence     */
} pac_verdict_t;

typedef struct {
    /* Running energies, in the same 0..120 unit as the verdict. Only the
     * CURRENT window's are kept; there is deliberately nowhere to accumulate
     * a signal from. */
    uint8_t level[PAC_BAND_COUNT];
    /* One bit per past window per band: was this band present? That is the
     * entire history this engine keeps, and it is four bits per 20 ms. */
    uint32_t seen[PAC_BAND_COUNT];
    uint8_t windows;
    uint8_t floor[PAC_BAND_COUNT]; /* slow noise floor, per band */

    /* THE BEST MARGIN EACH BAND HAS ACHIEVED, decaying slowly.
     *
     * The first version judged on the CURRENT window's level, which for a
     * beacon that is present three windows in four is a coin toss: whenever
     * the last window happened to fall in the gap, the tone vanished from the
     * verdict entirely and a different band won on noise. A beacon is a thing
     * that happens repeatedly, so what it achieved over the history is the
     * measurement, and the instantaneous level is only the newest sample of
     * it. */
    uint8_t peak[PAC_BAND_COUNT];

    /* Loudest the audible band has been, same decay. One window of quiet
     * noise can read as silence at 1 kHz purely by chance, and calling the
     * microphone dead on that basis would be a worse lie than any verdict. */
    uint8_t audible_best;
    bool floor_valid;
} pac_engine_t;

void pac_reset(pac_engine_t *e);

/* Consume one window of samples. They are turned into band energies and
 * DISCARDED - nothing here retains them. `n` should be a few hundred samples;
 * the frequency resolution of the probe is set by it. */
void pac_observe(pac_engine_t *e, const int16_t *samples, unsigned n,
                 uint32_t sample_rate);

void pac_evaluate(const pac_engine_t *e, pac_verdict_t *out);

/* ---- holding the verdict still --------------------------------------
 *
 * pac_evaluate() is a pure function of the rolling window, with no memory of
 * what it last said. When the score sits near a band boundary - which is
 * exactly where a weak or intermittent tone puts it - the answer flips on
 * noise. On hardware this printed QUIET, TRACE, TONE PRESENT and PERSISTENT
 * within one second, repeatedly, and named a different frequency each time.
 *
 * A reading nobody can read is not a reading. This is the same confirm-counter
 * Locate uses on its needle: a NEW verdict has to say the same thing several
 * evaluations running before it replaces the one on the glass. Noise cannot
 * hold an opinion that long; a real tone can.
 *
 * Held separately from the engine so that the engine stays a pure measurement
 * and this stays a pure display decision - and so both remain host-testable
 * on their own. */
#define PAC_CONFIRM 4u

typedef struct {
    pac_verdict_band_t shown;     /* what the operator is being told    */
    pac_verdict_band_t candidate; /* what evaluate keeps saying instead */
    pac_band_t shown_band;    /* the named frequency, held with it  */
    pac_band_t cand_band;
    uint8_t agree;            /* consecutive evaluations agreeing   */
    bool primed;              /* false until the first verdict      */
} pac_hold_t;

void pac_hold_reset(pac_hold_t *h);

/* Fold a fresh verdict into the held one, rewriting v->band and v->strongest
 * to what should actually be displayed. Returns true when the displayed
 * verdict changed, which is the caller's cue to log it. */
bool pac_hold_apply(pac_hold_t *h, pac_verdict_t *v);

const char *pac_band_name(pac_verdict_band_t b);
const char *pac_band_hint(pac_verdict_band_t b);
const char *pac_probe_name(pac_band_t b);
/* Centre frequency of a probe, in Hz. */
uint32_t pac_probe_hz(pac_band_t b);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_ACOUSTIC_H */
