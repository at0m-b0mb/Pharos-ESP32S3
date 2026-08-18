/* Pharos - the deauthentication watch engine (v2)
 *
 * Pure C. No ESP-IDF, no FreeRTOS, no allocation, no floating point. It takes
 * 802.11 frame summaries in and produces a graded verdict out, which is why
 * the whole thing can be exercised on a laptop by test/host/test_watch.c
 * before it ever meets a radio.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS WAS REWRITTEN
 *
 * v1 scored three families - rate, targeting shape, sender identity - and
 * capped the result by how much of the channel the receiver actually heard.
 * The arithmetic was sound and the tests passed, and in the field it was
 * still useless, for one reason that no test could see:
 *
 *   While hopping 13 channels the ceiling sat at ~60, and the identity family
 *   needed beacons this receiver was usually not on the channel to hear. So
 *   at most two families could ever fire, two families could not exceed 74,
 *   and 74 was above the ceiling anyway. The alarm band was arithmetically
 *   unreachable in the only posture the device ships in, and the operator had
 *   no control that would change the posture. A detector that cannot alarm is
 *   not a cautious detector, it is a broken one.
 *
 * The fix is not to lower the bar. It is to notice that the ceiling was
 * punishing the WRONG THING. Hopping weakens an extrapolated RATE - you heard
 * 7% of a second and multiplied by 14 - but it does not weaken a logical
 * contradiction. If a network advertises that its management frames are
 * cryptographically protected, and an UNPROTECTED deauthentication frame
 * arrives claiming to be from it, that frame is forged. Hearing it during a
 * 200 ms visit makes it no less forged. Evidence like that is dwell
 * independent, and v2 says so.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT LOOKS FOR - four independent families
 *
 *   RATE (0..34)      How much disconnect traffic there is, duty-corrected,
 *                     plus the peak second. Anchored on Kismet's long-standing
 *                     DEAUTHFLOOD thresholds (5/min sustained, 2/sec burst) so
 *                     "background" and "flood" mean what a WIDS operator
 *                     already expects them to mean.
 *
 *   SHAPE (0..22)     Who it is aimed at: broadcast share, how many distinct
 *                     victims, and whether the frames arrive in the tight
 *                     repeated bursts per victim that every deauth tool emits
 *                     and that no access point produces.
 *
 *   FORGERY (0..30)   Whether the sender is the device it claims to be. Four
 *                     independent tests, described at pw_forgery_t below. Two
 *                     of them (MFP contradiction, sequence-order violation)
 *                     are contradictions rather than estimates, so they are
 *                     not weakened by hopping.
 *
 *   AFTERMATH (0..18) Whether it WORKED. A disconnection that lands is
 *                     followed within a few seconds by the clients coming
 *                     back: a spike of authentication and (re)association
 *                     requests. Correlating the stampede with the burst that
 *                     caused it is what separates a real attack from air that
 *                     merely looks busy - and it is the one family that still
 *                     works on the low-volume TARGETED attacks that volume
 *                     thresholds are documented to miss.
 *
 * plus a small reason-code modifier (0..10), deliberately small: Kismet
 * deprecated its own invalid-reason-code alert because modern access points
 * emit odd codes routinely. Monoculture is a hint, never a family.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT REFUSES TO DO
 *
 *   - Alarm on volume alone. The alarm band needs three families to agree,
 *     and since only four exist - two of which are volume and shape - three
 *     of them necessarily includes FORGERY or AFTERMATH: somebody is lying
 *     about who they are, or devices actually got knocked off. A busy network
 *     is not an attack. PW_CAP_NO_CORROBORATION states that requirement
 *     directly as well, so that adding a fifth family later cannot quietly
 *     weaken the promise; today it is implied by the three-family rule and
 *     never binds on its own. test_watch_no_corroboration_invariant asserts
 *     the guarantee itself rather than either mechanism.
 *
 *   - Claim certainty. Every verdict carries a ceiling below 100 in every
 *     configuration this firmware allows. One antenna, one receiver, and no
 *     way to rule out a transmitter it simply did not hear.
 *
 *   - Say a network is safe. There is no such band. Absence of evidence on a
 *     receiver that hears one channel at a time is not evidence of absence.
 */
#ifndef PHAROS_WATCH_H
#define PHAROS_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Table sizes. The engine is a fixed-size struct with no allocation; on target
 * it lives in PSRAM via EXT_RAM_BSS_ATTR. */
#define PW_MAX_AP        32 /* access points learned from beacons          */
#define PW_MAX_HITS     256 /* disconnect ring; shape is sampled from this */
#define PW_MAX_VICTIMS   24
#define PW_MAX_SOURCES   12
#define PW_WINDOW_SLOTS  16 /* one second each: the window caps at 16 s    */

/* Evidence families. */
#define PW_FAM_RATE      (1u << 0)
#define PW_FAM_SHAPE     (1u << 1)
#define PW_FAM_FORGERY   (1u << 2)
#define PW_FAM_AFTERMATH (1u << 3)

/* Score caps, named so the tests can assert on the design rather than on a
 * magic number that might drift. */
#define PW_CAP_ONE_FAMILY        45 /* a single signal is never a case      */
#define PW_CAP_TWO_FAMILIES      66 /* two is a lead, not a conclusion      */
#define PW_CAP_NO_CORROBORATION  74 /* volume+shape alone: one below alarm  */
#define PW_CAP_SHORT_WINDOW      45 /* a blink is not a rate                */
#define PW_CEILING_MAX           96 /* nothing this device hears earns more */
#define PW_CEILING_HARD_EVIDENCE 88 /* what a proven contradiction is worth */

typedef enum {
    PW_BAND_QUIET = 0,   /*  0-19  nothing above the ordinary    */
    PW_BAND_BACKGROUND,  /* 20-39  networks do this normally     */
    PW_BAND_ELEVATED,    /* 40-59  more than housekeeping        */
    PW_BAND_SUSPICIOUS,  /* 60-74  looks wrong, evidence is thin */
    PW_BAND_LIKELY,      /* 75-100 deauthentication flood likely */
} pw_band_t;

/* The forgery tests, as a bitmask, so the UI can name the one that fired
 * instead of showing an anonymous score.
 *
 *   MFP_PROOF     The claimed network advertises that management frames are
 *                 REQUIRED to be protected (802.11w, RSN capability bit 6),
 *                 and this deauthentication frame is not protected. A genuine
 *                 frame from that access point could not look like this. This
 *                 is a contradiction, not an estimate: it is dwell
 *                 independent and it raises the confidence ceiling.
 *
 *   MFP_HINT      The network advertises MFP capable but not required, so a
 *                 client that did not negotiate protection can legitimately
 *                 be disconnected in the clear. Suspicious, not proof.
 *
 *   SEQ_ORDER     802.11 sequence numbers are a single 12-bit counter per
 *                 transmitter that only ever advances. We do NOT test the
 *                 SIZE of the gap - the literature is clear that gap
 *                 thresholds false-positive heavily on frames the receiver
 *                 simply missed. We test ORDER: a frame claiming this BSSID
 *                 carried a counter value BEHIND a beacon we already heard
 *                 from it, or AHEAD of the next beacon that followed. Missed
 *                 frames widen gaps; they never reverse them. Silent if the
 *                 attacker bothers to clone the counter - most tools do not.
 *
 *   SEQ_FROZEN    Many frames from one source carrying one or two distinct
 *                 sequence values. A real transmitter increments; a tool that
 *                 hand-builds a frame and blasts it usually does not.
 *
 *   RSSI_SPLIT    The frames claim an access point we can hear, but arrive at
 *                 a level well outside the spread that access point's own
 *                 beacons occupy. Two transmitters wearing one address.
 *                 Measured against the beacon's OBSERVED variance, not a flat
 *                 threshold, because a distant AP fading 10 dB is normal and
 *                 a stable one moving 10 dB is not.
 *
 *   GHOST         The source has never been heard to beacon. Suggestive, and
 *                 scaled by dwell: while hopping we very plausibly just missed
 *                 the beacons.
 */
#define PW_FORGE_MFP_PROOF  (1u << 0)
#define PW_FORGE_MFP_HINT   (1u << 1)
#define PW_FORGE_SEQ_ORDER  (1u << 2)
#define PW_FORGE_SEQ_FROZEN (1u << 3)
#define PW_FORGE_RSSI_SPLIT (1u << 4)
#define PW_FORGE_GHOST      (1u << 5)

/* Notes are things worth telling the operator that are not scores. */
#define PW_NOTE_LOSSY        (1u << 0) /* the ingest bus dropped frames      */
#define PW_NOTE_THIN_DWELL   (1u << 1) /* hopping; this is a peek, not a view */
#define PW_NOTE_SHORT_WINDOW (1u << 2) /* not enough elapsed time yet        */
#define PW_NOTE_SAMPLED      (1u << 3) /* shape stats came from a sample     */
#define PW_NOTE_HARD         (1u << 4) /* a dwell-independent proof fired    */
#define PW_NOTE_MFP_TARGET   (1u << 5) /* the victim network runs 802.11w    */
#define PW_NOTE_PROTECTED    (1u << 6) /* some disconnects were MFP-protected */
#define PW_NOTE_NO_RATE      (1u << 7) /* too little channel time to quote a rate */

/* The least channel time, in milliseconds, that makes a duty-corrected rate
 * worth quoting.
 *
 * The correction divides what was heard by the share of the channel that was
 * listened to, and as that share approaches zero the quotient approaches
 * nonsense. On the first hardware run this produced `est=33600.00/s dwell=0%`
 * moments after the lens stopped camping - the receiver had just retuned, the
 * decaying dwell window still held time spent elsewhere, and two frames became
 * an estimate of thirty-three thousand a second. The RATE family duly fired on
 * an empty room.
 *
 * A rate needs a denominator that was actually observed. Below this much
 * channel time there is no denominator worth dividing by, so the engine quotes
 * no rate at all and says so (PW_NOTE_NO_RATE) rather than extrapolating from
 * a sliver. */
#define PW_MIN_CHANNEL_MS 400u

typedef struct {
    /* Per mille of wall time the receiver spent on the channel being judged.
     * 1000 = camped on one channel. ~77 = even hop across 13 channels. */
    uint16_t dwell_permil;
    /* Per mille of offered events the ingest bus accepted; see pharos_bus. */
    uint16_t bus_yield_permil;
    /* Analysis window in milliseconds; clamped to PW_WINDOW_SLOTS seconds. */
    uint32_t window_ms;
} pw_context_t;

typedef struct {
    uint8_t score;     /* 0..100, after every cap and ceiling  */
    uint8_t raw_score; /* before caps, for the "why" panel     */
    uint8_t ceiling;   /* the most this observation could earn */
    uint8_t families;  /* PW_FAM_* bitmask                     */
    uint8_t forgery;   /* PW_FORGE_* bitmask                   */
    uint8_t notes;     /* PW_NOTE_* bitmask                    */
    pw_band_t band;

    /* Component contributions, drawn as stacked arcs in the UI. */
    uint8_t c_rate, c_shape, c_forgery, c_aftermath, c_reason;

    uint32_t observed;       /* deauth+disassoc counted in the window     */
    uint32_t shape_sample;   /* frames the shape stats were drawn from    */
    uint32_t est_per_s_x100; /* duty-corrected rate estimate              */
    uint32_t peak_second;    /* busiest single second in the window       */

    uint16_t broadcast_permil; /* share aimed at ff:ff:ff:ff:ff:ff        */
    uint8_t distinct_victims;
    uint8_t distinct_sources;
    uint8_t max_burst;       /* longest run at one victim inside 500 ms   */

    uint8_t src[6];          /* dominant transmitter in the window        */
    char ssid[PHAROS_EV_SSID_MAX + 1]; /* its network name, if we heard it */
    uint8_t channel;         /* where the pressure is - what to camp on   */

    int8_t rssi_delta;       /* |beacon mean - deauth mean| in dB         */
    int8_t rssi_spread;      /* the beacon's own spread, for comparison   */
    uint16_t seq_violations; /* frames that broke the counter's order     */
    uint8_t seq_distinct;    /* distinct sequence values from the source  */

    uint32_t rejoins;        /* auth/assoc frames in the window           */
    uint32_t rejoins_after;  /* ...that followed a disconnect burst       */

    uint16_t dominant_reason;
    uint8_t dominant_reason_pct;
} pw_verdict_t;

/* ---- engine state (opaque in spirit; exposed so it can be a static) ---- */

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    int16_t rssi_ewma_x4;  /* quarter-dBm, 1/4-weight EWMA               */
    int16_t rssi_dev_x4;   /* mean absolute deviation, same units        */
    uint16_t beacons;
    uint64_t last_beacon_us;
    uint16_t last_beacon_seq;
    /* How fast this AP's own counter runs, in steps per second, held as a
     * decaying peak measured from consecutive beacons. The counter advances on
     * data frames this receiver never sees, so the order test has to bound the
     * plausible advance before it may accuse anyone - a busy AP is not a liar.
     * See ap_seq_bound(). */
    uint16_t seq_rate_peak;
    /* The furthest-ahead disconnect sequence seen since that beacon, and
     * whether one has been seen at all. This is what makes the forward half
     * of the order test O(1) instead of a rescan. */
    uint16_t pending_seq;
    bool pending_valid;
    uint8_t rsn_flags;     /* PHAROS_RSN_F_* from the beacon             */
    uint16_t seq_back;     /* disconnects behind this AP's own counter   */
    uint16_t seq_fwd;      /* disconnects ahead of its next beacon       */
    uint8_t ssid_len;
    char ssid[PHAROS_EV_SSID_MAX];
    bool in_use;
} pw_ap_t;

typedef struct {
    uint64_t t_us;
    uint8_t src[6];
    uint8_t dst[6];
    uint16_t reason;
    uint16_t seq;
    int8_t rssi;
    uint8_t channel;
    uint8_t flags; /* PHAROS_DOT11_F_* as received */
} pw_hit_t;

typedef struct {
    uint32_t sec;
    uint32_t disconnects;
    uint32_t rejoins; /* auth + assoc_req + reassoc_req */
} pw_slot_t;

typedef struct {
    pw_ap_t aps[PW_MAX_AP];
    pw_hit_t hits[PW_MAX_HITS];
    pw_slot_t slots[PW_WINDOW_SLOTS];

    uint16_t hit_head;  /* next write slot            */
    uint16_t hit_count; /* <= PW_MAX_HITS             */
    uint32_t hit_total; /* every disconnect ever fed in */

    uint64_t first_us;
    uint64_t last_us;
    uint32_t total_frames;
    uint32_t evicted_aps;
} pw_engine_t;

/* ---- API ------------------------------------------------------------- */

void pw_reset(pw_engine_t *e);

/* Feed one 802.11 management frame summary. Cheap; safe at line rate. */
void pw_observe(pw_engine_t *e, const pharos_ev_dot11_t *f, uint64_t t_us);

/* Grade the trailing ctx->window_ms ending at now_us. */
void pw_evaluate(const pw_engine_t *e, uint64_t now_us, const pw_context_t *ctx,
                 pw_verdict_t *out);

/* The most any verdict could score at this observation quality, ignoring what
 * has actually been heard. Shown on the gauge as a hard stop so the operator
 * can see what camping would buy. pw_evaluate may raise this for a specific
 * verdict when a dwell-independent contradiction fired - see PW_NOTE_HARD. */
uint8_t pw_ceiling(const pw_context_t *ctx);

/* Which channel is carrying the disconnect traffic, or 0 if none is. This is
 * what the lens camps on when it decides to stop hopping and confirm. */
uint8_t pw_pressure_channel(const pw_engine_t *e, uint64_t now_us,
                            uint32_t window_ms);

/* The last PW_WINDOW_SLOTS seconds of disconnect counts, oldest first, with
 * seconds that were never written filled in as zero.
 *
 * A single score answers "is this happening"; the history answers "what shape
 * is it", which is the question an operator actually asks next. A steady
 * trickle and a single violent burst can produce the same ten-second mean and
 * they are not the same event. The engine already keeps these slots to do its
 * own arithmetic - this only lets the screen show them. */
void pw_history(const pw_engine_t *e, uint64_t now_us,
                uint16_t out[PW_WINDOW_SLOTS]);

const char *pw_band_name(pw_band_t band);
const char *pw_band_advice(pw_band_t band);
/* One short line naming the strongest forgery test that fired, for the UI.
 * Returns NULL when none did. */
const char *pw_forgery_name(uint8_t forgery_mask);
/* The 802.11 reason code in words, for the detail line. */
const char *pw_reason_name(uint16_t reason);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_WATCH_H */
