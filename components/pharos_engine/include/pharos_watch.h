/* Pharos - the deauthentication watch engine
 *
 * Pure C. No ESP-IDF, no FreeRTOS, no allocation, no floating point. It takes
 * 802.11 frame summaries in and produces a graded verdict out, which is why
 * the whole thing can be exercised on a laptop by test/host/test_pharos.c
 * before it ever meets a radio.
 *
 * What it looks for: somebody spraying deauthentication or disassociation
 * frames to knock clients off a network. What it refuses to do is claim
 * certainty, for a reason specific to this hardware:
 *
 *   The ESP32-S3 has one receiver. To cover 2.4 GHz it hops, so it sits on
 *   any given channel for a small fraction of the time. At a 200 ms dwell
 *   over 14 channels you observe roughly 7% of what happens on any one
 *   channel. Multiply an observed count by 14 and you have an estimate, not
 *   a measurement. Every verdict therefore carries a confidence ceiling
 *   derived from how much of the channel you actually heard, and the
 *   ceiling is below 100 in every configuration this firmware allows. While
 *   hopping, the ceiling sits at roughly 60: the same traffic that reads
 *   FLOOD LIKELY from a camped receiver can only read SUSPICIOUS from a
 *   hopping one. Raising the alarm requires going and standing still.
 *
 * Three evidence families - rate, targeting shape, and sender identity -
 * contribute at most 40, 22 and 22 points, with a 12 point modifier for
 * reason-code monoculture. One family alone is capped at ELEVATED. Any two
 * families together top out at 74, one point below the alarm band, so
 * FLOOD LIKELY is reachable only when all three agree. That is arithmetic,
 * not a rule bolted on afterwards.
 *
 * There is no verdict meaning "this network is safe". Absence of evidence on
 * a receiver that hears 7% of the air is not evidence of absence.
 */
#ifndef PHAROS_WATCH_H
#define PHAROS_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PW_MAX_AP        48
#define PW_MAX_EVENTS   192 /* shape sample; rate is counted separately */
#define PW_MAX_VICTIMS   32
#define PW_RATE_BUCKETS  16 /* one per second, so window_ms caps at 16000 */

/* Evidence families. */
#define PW_FAM_RATE     (1u << 0) /* how much is being sent          */
#define PW_FAM_TARGET   (1u << 1) /* the shape of who it is aimed at */
#define PW_FAM_IDENTITY (1u << 2) /* the sender is not who it claims */

typedef enum {
    PW_BAND_QUIET = 0,   /*  0-19  nothing above the ordinary    */
    PW_BAND_BACKGROUND,  /* 20-39  networks do this normally     */
    PW_BAND_ELEVATED,    /* 40-59  more than housekeeping        */
    PW_BAND_SUSPICIOUS,  /* 60-74  looks wrong, evidence is thin */
    PW_BAND_LIKELY,      /* 75-100 deauthentication flood likely */
} pw_band_t;

/* Notes are things worth telling the operator that are not scores. */
#define PW_NOTE_MFP_TARGET   (1u << 0) /* victim network advertises 802.11w   */
#define PW_NOTE_LOSSY        (1u << 1) /* the ingest bus dropped frames       */
#define PW_NOTE_THIN_DWELL   (1u << 2) /* hopping; this is a peek, not a view */
#define PW_NOTE_SHORT_WINDOW (1u << 3) /* not enough elapsed time yet         */
#define PW_NOTE_SAMPLED      (1u << 4) /* shape stats came from a sample      */

typedef struct {
    /* Per mille of wall time the receiver spent on the channel being judged.
     * 1000 = camped on one channel. ~71 = even hop across 14 channels. */
    uint16_t dwell_permil;
    /* Per mille of offered events the ingest bus accepted; see pharos_bus. */
    uint16_t bus_yield_permil;
    /* Analysis window in milliseconds; clamped to PW_RATE_BUCKETS seconds. */
    uint32_t window_ms;
} pw_context_t;

typedef struct {
    uint8_t score;     /* 0..100, after every cap and ceiling  */
    uint8_t raw_score; /* before caps, for the "why" panel     */
    uint8_t ceiling;   /* the most this observation could earn */
    uint8_t families;  /* PW_FAM_* bitmask                     */
    uint8_t notes;     /* PW_NOTE_* bitmask                    */
    pw_band_t band;

    /* Component contributions, drawn as stacked arcs in the UI. */
    uint8_t c_rate, c_target, c_identity, c_reason;

    uint32_t observed;         /* deauth+disassoc counted in window      */
    uint32_t shape_sample;     /* frames the shape stats were drawn from */
    uint32_t est_per_s_x100;   /* duty-corrected rate estimate           */
    uint16_t broadcast_permil; /* share aimed at ff:ff:ff:ff:ff:ff       */
    uint8_t distinct_victims;
    uint8_t distinct_sources;
    uint8_t src[6];            /* dominant transmitter in the window     */
    int8_t rssi_delta;         /* |claimed AP beacon - deauth| in dB     */
    uint16_t dominant_reason;
    uint8_t dominant_reason_pct;
} pw_verdict_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    int16_t rssi_ewma_x4;
    uint16_t beacons;
    uint64_t last_beacon_us;
    bool mfp_capable;
    bool in_use;
} pw_ap_t;

typedef struct {
    uint64_t t_us;
    uint8_t src[6];
    uint8_t dst[6];
    uint16_t reason;
    int8_t rssi;
    uint8_t channel;
} pw_hit_t;

typedef struct {
    uint32_t sec;
    uint32_t count;
} pw_bucket_t;

typedef struct {
    pw_ap_t aps[PW_MAX_AP];
    pw_hit_t hits[PW_MAX_EVENTS];
    pw_bucket_t rate[PW_RATE_BUCKETS];
    uint16_t hit_head;  /* next write slot */
    uint16_t hit_count; /* <= PW_MAX_EVENTS */
    uint32_t hit_total; /* every deauth ever fed in */
    uint64_t first_us;
    uint64_t last_us;
    uint32_t total_frames;
    uint32_t evicted_aps;
} pw_engine_t;

void pw_reset(pw_engine_t *e);

/* Feed one 802.11 management frame summary. Cheap; safe at line rate. */
void pw_observe(pw_engine_t *e, const pharos_ev_dot11_t *f, uint64_t t_us);

/* Grade the trailing ctx->window_ms ending at now_us. */
void pw_evaluate(const pw_engine_t *e, uint64_t now_us, const pw_context_t *ctx,
                 pw_verdict_t *out);

/* The most any verdict could score at this observation quality. Shown on the
 * gauge as a hard stop, so the operator can see what camping would buy. */
uint8_t pw_ceiling(const pw_context_t *ctx);

const char *pw_band_name(pw_band_t band);
const char *pw_band_advice(pw_band_t band);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_WATCH_H */
