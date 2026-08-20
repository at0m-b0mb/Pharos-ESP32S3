/* Pharos - the Watchtower: every sensor at once, honestly
 *
 * Pure C. The rotation policy and the freshness rules live here so they can be
 * host-tested, because this is the part of the device most able to tell a
 * comfortable lie.
 *
 * ---------------------------------------------------------------------------
 * ONE RADIO, SIX WATCHES
 *
 * The request was reasonable and the naive reading of it is impossible: show
 * every sensor at once so nobody has to be sitting inside the right lens at
 * the moment an attack happens. There is one 2.4 GHz receiver and one BLE
 * controller in this device. Six detectors cannot listen simultaneously, and a
 * home screen with six green dots on it would be claiming they do.
 *
 * What IS possible is a rotation: each armed watch takes the radio in turn,
 * reports, and hands it on. That is strictly better than the operator doing it
 * by hand - it never forgets, it never gets bored - and it is the whole reason
 * the screen can say anything about a lens nobody is looking at.
 *
 * It is also a duty cycle, and a duty cycle has consequences that must be
 * visible rather than smoothed over:
 *
 *   1. A READING HAS AN AGE. A watch that last held the radio forty seconds
 *      ago knows nothing about now. Its dot therefore FADES with age and goes
 *      hollow once expired, and the ring never shows a stale reading in the
 *      same ink as a live one. `ptw_freshness` is that rule and the UI has no
 *      say in it.
 *
 *   2. QUIET IS NOT THE SAME AS CLEAR. A watch that has never held the radio
 *      has no opinion, which is a different thing from having looked and found
 *      nothing. The two get different states - PTW_UNKNOWN and PTW_QUIET - and
 *      the summary counts them separately, so "all quiet" is only ever said
 *      about watches that have actually looked.
 *
 *   3. THE CEILING ALREADY KNOWS. Every engine here scores against a
 *      confidence ceiling derived from how much of the channel it actually
 *      heard (see pharos_watch.h). A lens running one sixth of the time earns
 *      a low ceiling and says SUSPICIOUS where a camped one would say FLOOD
 *      LIKELY. That is not a defect of the rotation - it is the rotation being
 *      reported honestly, by machinery that already existed.
 *
 * The other half of the ask - "if there is an alert it is there in one go" -
 * is why anything alarming is LATCHED rather than left to the rotation. The
 * air can go quiet between two visits from the same watch; the finding does
 * not evaporate because the radio walked away. Aegis already does exactly this
 * across lens switches (see pharos_aegis.h), and the tower defers to it for
 * anything that reaches an alarm.
 */
#ifndef PHAROS_TOWER_H
#define PHAROS_TOWER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTW_MAX_WATCHES 16
#define PTW_NAME_MAX 11 /* what fits a rim label on a 466 px circle */
#define PTW_ID_MAX 23

/* How a watch is doing, as the ring draws it. Deliberately NOT a copy of any
 * one engine's band enum: the ring has to put a Wi-Fi flood, a BLE spam burst
 * and a rogue-AP appliance on the same dial, and the only thing they share is
 * how much they should worry somebody. */
typedef enum {
    PTW_UNKNOWN = 0, /* has not held the radio yet - no opinion   */
    PTW_QUIET,       /* looked, found nothing                     */
    PTW_NOTED,       /* something worth knowing                   */
    PTW_ELEVATED,    /* one real finding                          */
    PTW_ALARM,       /* act on this                               */
    PTW_STATE_COUNT,
} ptw_state_t;

/* How much a reading can still be trusted, purely as a function of its age.
 * Drawn as opacity, so an old reading literally fades. */
typedef enum {
    PTW_FRESH = 0, /* taken within its own dwell - this is now      */
    PTW_AGEING,    /* still the best we have, visibly older         */
    PTW_EXPIRED,   /* too old to stand behind; drawn hollow         */
    PTW_FRESHNESS_COUNT,
} ptw_freshness_t;

/* A reading is fresh until it has missed a full rotation, and expired once it
 * has missed three. Both are expressed in rotations rather than seconds so
 * that arming another watch - which necessarily slows every other watch down -
 * cannot silently turn honest dots into lying ones. */
#define PTW_AGEING_ROTATIONS 1u
#define PTW_EXPIRED_ROTATIONS 3u
#define PTW_MAX_PERIOD 4u

/* HOW LONG A WATCH MAY HOLD THE RADIO BECAUSE IT IS HEARING SOMETHING.
 *
 * Holding is right: walking away from an attack in progress to keep a rota
 * tidy would be the worst possible moment to leave, and the confidence ceiling
 * needs the airtime precisely then. But an UNBOUNDED hold is a starvation bug,
 * and it fired on the first run: the microphone watch sits at ELEVATED in any
 * ordinary room - there is always something at 19 kHz - so it took the radio
 * and never gave it back. Eleven of twelve watches had never run, the ring was
 * eleven hollow dots, and the one feature the ring exists to provide was gone.
 *
 * A hold may extend a slice, never abolish it. After this many consecutive
 * slices the watch hands on regardless, and gets its turn again next time
 * round - by which point anything real is still there to be found. */
#define PTW_MAX_HOLD_SLICES 4u

typedef struct {
    char id[PTW_ID_MAX + 1];     /* lens id, for activation      */
    char name[PTW_NAME_MAX + 1]; /* rim label                    */
    ptw_state_t state;
    uint8_t score;    /* 0..100, for the ring arc                */
    uint8_t ceiling;  /* what the duty cycle allowed it to earn  */
    uint64_t seen_us; /* when this reading was taken, 0 = never  */
    uint32_t visits;  /* how many times it has held the radio    */
    uint8_t period;   /* take a turn every Nth lap; 1 = every lap */
    bool armed;       /* in the rotation at all                  */
} ptw_watch_t;

typedef struct {
    ptw_watch_t w[PTW_MAX_WATCHES];
    unsigned n;
    unsigned cursor;      /* whose turn it is                    */
    uint64_t handover_us; /* when the current watch took over    */
    uint32_t dwell_ms;    /* the base slice, per watch           */
    uint32_t rotations;   /* completed passes round the ring     */
    uint32_t held;        /* consecutive slices the turn was held */
} ptw_state_st;

/* The one-line answer the middle of the dial shows. */
typedef struct {
    ptw_state_t worst;
    unsigned armed;     /* watches in the rotation                */
    unsigned reporting; /* armed AND holding a non-expired reading */
    unsigned quiet;     /* looked and found nothing               */
    unsigned unknown;   /* not yet looked                         */
    unsigned alarms;    /* at PTW_ALARM                           */
    int worst_index;    /* which watch, or -1                     */
    const char *headline;
} ptw_summary_t;

void ptw_reset(ptw_state_st *s, uint32_t dwell_ms);

/* Put a lens in the rotation. Returns its index, or -1 if the table is full
 * or the id is already present. */
int ptw_arm(ptw_state_st *s, const char *id, const char *name);

/* NOT EVERY WATCH NEEDS THE RADIO EQUALLY OFTEN.
 *
 * A deauthentication flood is an EVENT: it lasts seconds and is missed
 * entirely if the receiver is elsewhere. A census of the networks around you
 * is a STANDING FACT: it changes over minutes, and looking at it every lap
 * buys nothing while costing the event detectors airtime they cannot spare.
 *
 * Arming every observer uniformly made that trade-off badly - thirteen watches
 * at five seconds is a sixty-five second lap, so the flood detector was deaf
 * for a minute at a time in order to keep re-counting the same access points.
 *
 * `period` is how many laps pass between this watch's turns. Event detectors
 * take 1 and stay fast however many surveys are added beside them; surveys
 * take 2 or more and are none the worse for it. Freshness scales with it, so a
 * watch that is only due every third lap is not called stale for being on
 * time - see ptw_freshness. */
int ptw_arm_every(ptw_state_st *s, const char *id, const char *name,
                  uint8_t period);

/* A watch just reported. `score` and `ceiling` are the engine's own numbers;
 * `state` is what the lens makes of them. */
void ptw_report(ptw_state_st *s, const char *id, ptw_state_t state,
                uint8_t score, uint8_t ceiling, uint64_t now_us);

/* How old is watch i's reading, in rotations-relative terms. */
ptw_freshness_t ptw_freshness(const ptw_state_st *s, unsigned i, uint64_t now_us);

/* Whose turn is it now? Returns the index whose lens should be holding the
 * radio, or -1 when nothing is armed. Advances on its own schedule; call it
 * every tick. `*changed` is set when the turn just passed to somebody new,
 * which is the caller's cue to actually switch lens.
 *
 * `hold` asks the tower to leave the current watch where it is - the caller
 * passes true when the active lens is reporting something above background,
 * because walking away from an attack in progress to keep a rota tidy is the
 * one thing a rotation must never do. */
int ptw_turn(ptw_state_st *s, uint64_t now_us, bool hold, bool *changed);

void ptw_summarise(const ptw_state_st *s, uint64_t now_us, ptw_summary_t *out);

const char *ptw_state_name(ptw_state_t st);

/* Index of the watch with this id, or -1. */
int ptw_find(const ptw_state_st *s, const char *id);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_TOWER_H */
