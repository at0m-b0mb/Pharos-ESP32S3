/* Pharos - Aegis: one picture, and a memory that outlives the screen
 *
 * Pure C. Every other lens answers one question well and then forgets. Aegis
 * exists because of two things that are true of real defensive work and false
 * of every single-purpose detector:
 *
 *   1. An attack is a SEQUENCE, not a reading. Somebody scans, then stands up
 *      a lookalike access point, then knocks clients off it so they land on
 *      the wrong one, then collects the handshakes. Any one of those, alone,
 *      has an innocent explanation - a phone probing, a mesh node, a rebooting
 *      router, a client roaming. Together, in that order, they are an
 *      operation. Nothing that looks at one lens at a time can see that.
 *
 *   2. YOU ARE NOT LOOKING. A capture lasts seconds; a deauthentication burst
 *      lasts less. A handheld shows one lens at a time, and the operator is
 *      walking, talking to a client, or looking at the ceiling. A detector
 *      that only reports the present tense is a detector that misses almost
 *      everything that happens.
 *
 * So Aegis LATCHES. Every stage keeps its own high-water mark with the time it
 * happened, and it stays there until it is explicitly cleared. Walk back to
 * the device ten minutes later and it can still tell you that at 14:02 there
 * was a deauthentication flood on channel 6, even though the air is quiet now
 * and the lens that saw it has long since been swapped out.
 *
 * The honesty rules are what keep this from becoming a machine for inventing
 * alarms out of coincidences:
 *
 *   - Correlation may not invent evidence. One stage alone can never score
 *     higher through Aegis than it scored on its own; fusing is only allowed
 *     to add confidence when there is genuinely more than one thing to fuse.
 *   - The overall confidence ceiling is the MINIMUM of the contributing
 *     stages' ceilings. A conclusion drawn from a thin sweep and a good one is
 *     only as trustworthy as the thin sweep.
 *   - The ordering bonus requires the stages to have actually happened in
 *     order, in time. Three alarms in a jumble is a noisy room; three alarms
 *     in the attacker's own sequence is a campaign.
 *   - A latched finding is always reported WITH ITS AGE. "Twenty minutes ago"
 *     is a different operational fact from "right now", and the device says
 *     which.
 */
#ifndef PHAROS_AEGIS_H
#define PHAROS_AEGIS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The stages, in the order an operation tends to walk through them. The order
 * matters: it is what the sequence bonus is measured against. */
typedef enum {
    PA_STAGE_RECON = 0,   /* probing, scanning - somebody is looking       */
    PA_STAGE_IMPERSONATE, /* a twin, or a radio answering to any name      */
    PA_STAGE_DISRUPT,     /* deauthentication, beacon flood - pushing      */
    PA_STAGE_HARVEST,     /* handshakes or PMKIDs being collected          */
    PA_STAGE_DRIFT,       /* the estate itself changed since the baseline  */
    PA_STAGE_COUNT,
} pa_stage_t;

/* A stage counts as raised at or above this. Below it, a stage is background
 * and contributes nothing but context. */
#define PA_STAGE_ALARM 45

typedef enum {
    PA_BAND_CLEAR = 0,  /*  0-19  nothing raised                          */
    PA_BAND_NOTED,      /* 20-44  something worth knowing, not acting on  */
    PA_BAND_ELEVATED,   /* 45-69  one real finding, or several small ones */
    PA_BAND_INCIDENT,   /* 70-100 several stages, in order: an operation  */
    PA_BAND_COUNT,
} pa_band_t;

#define PA_NOTE_LATCHED   (1u << 0) /* the peak is history, not the present */
#define PA_NOTE_SEQUENCE  (1u << 1) /* stages arrived in attack order       */
#define PA_NOTE_SINGLE    (1u << 2) /* one stage only - correlation added 0 */
#define PA_NOTE_THIN      (1u << 3) /* a contributing sweep was thin        */
#define PA_NOTE_STALE     (1u << 4) /* nothing has updated in a long while  */

/* Anything older than this is reported as history rather than the present. */
#define PA_FRESH_US 60000000ull /* 60 s */

typedef struct {
    uint8_t peak;      /* high-water score for this stage        */
    uint8_t ceiling;   /* the ceiling that came with that peak   */
    uint8_t current;   /* what it reads right now                */
    uint32_t hits;     /* how many times it has been raised      */
    uint64_t first_us; /* when it was first raised               */
    uint64_t peak_us;  /* when the high-water mark was set       */
    uint64_t last_us;  /* the most recent observation of any kind*/
    bool raised;       /* has it ever reached PA_STAGE_ALARM     */
} pa_stage_state_t;

typedef struct {
    pa_stage_state_t stages[PA_STAGE_COUNT];
    uint64_t now_us;
} pa_state_t;

typedef struct {
    uint8_t score;
    uint8_t raw_score;
    uint8_t ceiling;
    uint8_t notes;
    pa_band_t band;

    uint8_t n_raised;      /* stages that have ever reached alarm      */
    uint8_t n_live;        /* ... that are still current               */
    uint8_t stage_mask;    /* bit per pa_stage_t that has been raised  */
    pa_stage_t worst;      /* the highest-scoring stage                */
    uint8_t worst_peak;
    uint32_t worst_age_s;  /* how long ago that peak happened          */

    const char *headline;
} pa_verdict_t;

void pa_reset(pa_state_t *s);

/* Push one lens' verdict in. score/ceiling are that engine's own numbers, so
 * Aegis never has to know how any particular detector reasons. */
void pa_observe(pa_state_t *s, pa_stage_t stage, uint8_t score, uint8_t ceiling,
                uint64_t t_us);

/* Clear the latch - the operator has seen it and is starting a fresh watch. */
void pa_acknowledge(pa_state_t *s);

void pa_evaluate(const pa_state_t *s, uint64_t now_us, pa_verdict_t *out);

const char *pa_stage_name(pa_stage_t st);
const char *pa_stage_meaning(pa_stage_t st);
const char *pa_band_name(pa_band_t b);
const char *pa_band_advice(pa_band_t b);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_AEGIS_H */
