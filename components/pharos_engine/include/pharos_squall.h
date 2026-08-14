/* Pharos - Squall: is the air busy, broken, or being denied?
 *
 * Pure C. Every other lens in Pharos reasons about frames somebody sent. This
 * one reasons about the frames that are NOT arriving, which is a different
 * discipline and answers the question a defender actually asks in a crisis:
 *
 *     "The Wi-Fi is down. Is it broken, is it just busy, or is somebody
 *      jamming us?"
 *
 * Those three have completely different responses - call the ISP, add capacity,
 * or start a physical search - and to the user they look identical.
 *
 * THE DISCRIMINATOR, and the reason this is worth an engine rather than a
 * threshold on a noise meter:
 *
 *     energy is high AND decodable frames are high  -> CONGESTED. A busy
 *         building. Loud, and entirely healthy. The most common false alarm
 *         in every naive "high noise = jamming" detector ever shipped.
 *
 *     energy is high AND decodable frames are LOW   -> DENIAL. The band is
 *         full of power that will not resolve into frames. That is what a
 *         jammer looks like from a receiver: it does not have to send valid
 *         802.11, it only has to make everybody else's unreadable.
 *
 *     energy low, frames low                        -> QUIET. Nothing here.
 *         Which is also what a dead radio looks like, and Squall says so
 *         rather than implying the air is fine.
 *
 * A second, independent family: RETRIES. When a channel is contended but
 * usable, senders retransmit and the retry fraction climbs. Under real denial
 * the retry fraction climbs *and then the frame count collapses* - the senders
 * give up. Retry pressure without an energy explanation is corroboration, not
 * proof, which is exactly how it is weighted.
 *
 * The honesty rules, as everywhere:
 *
 *   - DENIAL needs BOTH families. Energy alone is a microwave oven, a video
 *     sender, a neighbour's outdoor bridge, or a badly-sited camera. None of
 *     those are attacks, and a tool that calls them attacks gets ignored.
 *   - Nothing is graded from a single dwell. A jammer is a sustained condition;
 *     one bad 300 ms visit to a channel is a sample, not a finding.
 *   - A hopping receiver sees each channel for a fraction of the time, so its
 *     ceiling is lower - and unlike the frame-counting lenses, this one cannot
 *     even extrapolate, because "no frames arrived" is exactly what a channel
 *     you were not listening to also looks like.
 *   - There is no band named "safe". The best available is HEALTHY, which is a
 *     statement about the last few seconds on the channels actually visited.
 *
 * Squall never transmits, and could not clear a jam if it wanted to. It tells
 * a defender which of three very different problems they have.
 */
#ifndef PHAROS_SQUALL_H
#define PHAROS_SQUALL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PQ_MAX_CHANNELS 14
#define PQ_HISTORY 8 /* dwells remembered per channel */

/* A channel must be visited at least this many times before it may be graded.
 * One bad visit is a sample, not a finding. */
#define PQ_MIN_SAMPLES 3

typedef enum {
    PQ_STATE_UNKNOWN = 0, /* not yet visited enough to say anything      */
    PQ_STATE_QUIET,       /* little energy, few frames                   */
    PQ_STATE_HEALTHY,     /* energy and frames in proportion             */
    PQ_STATE_CONGESTED,   /* loud AND productive: a busy building        */
    PQ_STATE_DEGRADED,    /* frames are suffering; cause not established */
    PQ_STATE_DENIAL,      /* loud and unproductive: the shape of a jam   */
    PQ_STATE_COUNT,
} pq_state_t;

/* Evidence families. DENIAL requires both - see the header note. */
#define PQ_FAM_ENERGY  (1u << 0) /* power present that does not decode   */
#define PQ_FAM_RETRIES (1u << 1) /* senders retransmitting, then failing */
#define PQ_FAM_SPREAD  (1u << 2) /* the condition covers several channels*/

#define PQ_NOTE_THIN     (1u << 0) /* hopping: cannot distinguish absent  */
#define PQ_NOTE_FEW      (1u << 1) /* not enough dwells yet to grade      */
#define PQ_NOTE_NARROW   (1u << 2) /* one channel only: could be a device */
#define PQ_NOTE_NOFLOOR  (1u << 3) /* no noise-floor estimate available   */

typedef struct {
    uint16_t dwell_permil; /* share of time on the channel being judged */
    bool camped;
} pq_context_t;

/* One channel visit, as the radio reports it. */
typedef struct {
    uint8_t channel;
    uint16_t dwell_ms;
    uint16_t frames;      /* frames that actually decoded         */
    uint16_t retries;     /* of those, how many were retransmits  */
    int8_t noise_floor;   /* dBm, quartile estimate; 0 = unknown  */
    int8_t peak_rssi;
    uint16_t busy_permil; /* airtime the radio judged occupied    */
} pq_dwell_t;

typedef struct {
    bool in_use;
    uint8_t channel;
    uint32_t visits;
    uint32_t frames_total;
    uint32_t retries_total;
    uint32_t dwell_ms_total;
    int32_t floor_sum;   /* for a mean noise floor    */
    uint32_t floor_n;
    uint32_t busy_sum;
    pq_state_t state;
    uint8_t severity;    /* 0..100 for this channel   */
} pq_channel_t;

typedef struct {
    pq_channel_t ch[PQ_MAX_CHANNELS + 1];
    unsigned n_seen;
} pq_state_table_t;

typedef struct {
    uint8_t score;
    uint8_t raw_score;
    uint8_t ceiling;
    uint8_t families;
    uint8_t notes;
    pq_state_t worst;      /* the worst state on any graded channel */
    uint8_t worst_channel;

    uint8_t n_graded;      /* channels with enough samples to judge */
    uint8_t n_denial;      /* channels showing the denial shape     */
    uint8_t n_congested;
    uint16_t retry_permil; /* across everything graded              */

    const char *headline;
} pq_verdict_t;

void pq_reset(pq_state_table_t *t);

/* Feed one completed channel visit. */
void pq_observe(pq_state_table_t *t, const pq_dwell_t *d);

void pq_evaluate(const pq_state_table_t *t, const pq_context_t *ctx, pq_verdict_t *out);

uint8_t pq_ceiling(const pq_context_t *ctx);
const char *pq_state_name(pq_state_t s);
const char *pq_state_advice(pq_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_SQUALL_H */
