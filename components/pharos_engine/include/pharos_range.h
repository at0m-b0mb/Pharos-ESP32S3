/* Pharos - the training range
 *
 * A red-and-blue teaching tool that never transmits.
 *
 * The range synthesises realistic 802.11 event streams - a deauth flood, an
 * evil twin coming online, a phone leaking its history - and plays them
 * through the very same detection engines the operator uses on live air. The
 * learner watches a verdict form frame by frame and can pause to ask "why did
 * it say SUSPICIOUS and not LIKELY there?" The answer is always the same
 * arithmetic that runs in the field, because it is literally the same code.
 *
 * This is how Pharos teaches both sides of the house honestly:
 *   - Red: this is what your attack looks like from a defender's receiver,
 *     including the parts of it you cannot hide (rate, targeting, identity).
 *   - Blue: this is how a careful detector reasons, and this is exactly where
 *     and why it refuses to be certain.
 *
 * It is pure C, deterministic from a seed, and fully host-tested: a scenario
 * is a reproducible fixture, so "the flood scenario reaches FLOOD LIKELY when
 * camped and only SUSPICIOUS when hopping" is an assertion, not a vibe.
 *
 * Because it generates events rather than frames, and feeds them to engines
 * rather than a radio, there is no code path from the range to an antenna.
 * The range holds no radio capability at all.
 */
#ifndef PHAROS_RANGE_H
#define PHAROS_RANGE_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PR_SCENARIO_CALM = 0,     /* a healthy network: the baseline to know   */
    PR_SCENARIO_ROAMING,      /* many BSSIDs, one SSID: the false positive */
    PR_SCENARIO_DEAUTH_FLOOD, /* the blue-team headliner                   */
    PR_SCENARIO_EVIL_TWIN,    /* a rogue AP undercutting its siblings      */
    PR_SCENARIO_PROBE_LEAK,   /* a device broadcasting its history         */
    PR_SCENARIO_COUNT,
} pr_scenario_t;

typedef struct {
    pr_scenario_t scenario;
    uint32_t seed;      /* deterministic; same seed, same range          */
    uint16_t dwell_permil; /* the receiver posture the learner is running */
    uint16_t intensity;    /* 0..1000, how hard the attacker is pushing   */
} pr_config_t;

/* One lesson beat: a short human line the UI shows as the scenario unfolds,
 * timed to the moment the verdict crosses a band. */
typedef struct {
    uint32_t at_ms;
    const char *text;
} pr_beat_t;

typedef struct {
    pr_config_t cfg;
    uint64_t t_us;       /* virtual clock                              */
    uint32_t rng;        /* xorshift state                             */
    uint32_t emitted;
    uint8_t phase;
    /* Deterministic actors for the scenario. */
    uint8_t ap_bssid[6];
    uint8_t twin_bssid[6];
    uint8_t victim[6];
    uint8_t attacker[6];
    uint8_t phone[6];
} pr_range_t;

void pr_range_init(pr_range_t *r, const pr_config_t *cfg);

/* Produce the next synthetic event into *out, advancing the virtual clock.
 * Returns false when the scripted scenario has run its course. Feed *out to
 * whichever engine the lesson is about (pw_observe, pp_observe, or the
 * census/twin table) exactly as the live path would. */
bool pr_range_next(pr_range_t *r, pharos_event_t *out);

/* The lesson beats for a scenario, for the on-screen narration. Returns the
 * count and points *beats at a static table. */
unsigned pr_range_beats(pr_scenario_t s, const pr_beat_t **beats);

const char *pr_scenario_name(pr_scenario_t s);
const char *pr_scenario_teaches(pr_scenario_t s); /* one line: the lesson */

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_RANGE_H */
