/* Pharos - attribution: who actually sent that?
 *
 * A deauthentication frame carries the access point's address because that is
 * the whole point of the attack. Every detector therefore treats the source
 * MAC as a dead end, and stops at "an attack is happening".
 *
 * But the ADDRESS is forged and the RADIO is not. A transmitter sitting at
 * some distance, at some power, arrives at this receiver at a level that is
 * a property of the physical world rather than of the frame's contents. An
 * attacker can write any six bytes they like into the header; they cannot
 * write their signal strength.
 *
 * So this engine keeps a level profile for every radio it hears, and when the
 * Watch engine proves a frame was forged, it asks a different question from
 * the usual one: not "who does this frame claim to be", but "which of the
 * radios in this room arrives at the same level as the forgery". If exactly
 * one does, that is a lead worth having - it is very often the attacker's own
 * device, transmitting normally under its real address a moment earlier.
 *
 * ---------------------------------------------------------------------------
 * WHY A MATCH ONLY COUNTS WHEN IT IS THE ONLY MATCH
 *
 * Signal level is not an identity. Two devices at similar distance produce
 * similar levels, and a room where six things all sit around -45 dBm gives
 * six equally good "matches", which is the same as none. Reporting the first
 * of them would manufacture an accusation out of a coincidence, and pointing
 * at an innocent device is a worse failure than saying nothing.
 *
 * The rule is therefore uniqueness, asserted as a test: a candidate is
 * reported only when no OTHER radio is also within the window. Ambiguity is
 * reported as ambiguity, with the count, and the operator is told plainly
 * that the room is too crowded to say.
 *
 * Nothing here ever reaches certainty. The ceiling is PAT_MAX_CONFIDENCE and
 * the wording stays "the same radio as", never "the attacker is".
 *
 * ---------------------------------------------------------------------------
 * THE SECOND HALF: WHICH TOOL
 *
 * Attack tools ship templates. A real access point computes the duration
 * field from the frame it is about to send, so across a capture it varies; an
 * injector that hard-codes it emits the identical value forever. The same is
 * true of the sequence number, which some injectors never set at all.
 *
 * That is not who the attacker is, but it is what they are running, and it
 * narrows the question usefully. The signatures are drawn from published
 * detectors (AntiHunter fingerprints seq=0xFFF0 with dur=0x013A for the
 * Marauder / Evil-M5 family) and from the invariant behind them: a constant
 * where a computed value belongs.
 */
#ifndef PHAROS_ATTRIB_H
#define PHAROS_ATTRIB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How many distinct radios to profile. Beyond this the room is too busy for
 * uniqueness to mean anything anyway, which is itself the answer. */
#define PAT_MAX_RADIOS 24

/* How close two levels must be to be called the same radio, in dB.
 *
 * Chosen from what the hardware can actually resolve rather than from what
 * would be convenient: the ESP32's RSSI is quantised to 1 dB and moves a few
 * dB frame to frame from multipath alone, so anything tighter than this would
 * reject the true match as often as a false one. */
#define PAT_RSSI_WINDOW 4

/* Never certain, by construction. */
#define PAT_MAX_CONFIDENCE 85

typedef enum {
    PAT_TOOL_UNKNOWN = 0,
    PAT_TOOL_TEMPLATED,  /* a constant where a computed value belongs   */
    PAT_TOOL_MARAUDER,   /* seq 0xFFF0 with duration 0x013A             */
    PAT_TOOL_NO_SEQUENCE,/* never sets the sequence number at all       */
    PAT_TOOL_MICHAEL,    /* reason 14 - TKIP countermeasure, or a tool  */
} pat_tool_t;

const char *pat_tool_name(pat_tool_t t);

typedef struct {
    uint8_t mac[6];
    int16_t sum_rssi;    /* running mean, kept as sum/count to stay integer */
    uint16_t frames;
    int8_t rssi_min, rssi_max;
    bool used;
} pat_radio_t;

typedef struct {
    pat_radio_t radio[PAT_MAX_RADIOS];
    unsigned n_radios;

    /* The profile of the frames Watch proved were forged. */
    int32_t forged_sum;
    uint16_t forged_n;
    int8_t forged_min, forged_max;

    /* Tool tells, accumulated over the forged frames only. */
    uint16_t first_duration;
    bool duration_constant;
    uint16_t first_seq;
    bool seq_always_zero;
    bool seq_constant;
    uint16_t reason;
    bool saw_marauder_pair;
} pat_engine_t;

typedef struct {
    /* Attribution. */
    bool have_lead;
    uint8_t lead_mac[6];
    uint8_t confidence;   /* 0..PAT_MAX_CONFIDENCE */
    unsigned candidates;  /* how many radios fell in the window */
    bool ambiguous;       /* more than one - the room is too crowded */

    int8_t forged_rssi;   /* mean level of the forged frames */
    uint8_t forged_spread;

    /* Tooling, and the raw tells behind it - so an operator can check the
     * reasoning instead of taking a one-word label on trust. */
    pat_tool_t tool;
    bool duration_constant;
    uint16_t first_duration;

    /* Were any disconnect frames seen at all? Separates "no attacker" from
     * "an attacker whose radio says nothing else". */
    bool forged_n_seen;
} pat_verdict_t;

void pat_reset(pat_engine_t *e);

/* Every frame from a transmitter, forged or not. Builds the room's profile. */
void pat_observe(pat_engine_t *e, const uint8_t mac[6], int8_t rssi);

/* A frame Watch has proved forged. Feeds the attacker's own profile and the
 * tool tells. `seq` is the 12-bit sequence, `duration` the header field. */
void pat_observe_forged(pat_engine_t *e, int8_t rssi, uint16_t seq,
                        uint16_t duration, uint16_t reason);

/* Who, and what. Never returns certainty; see the header comment. */
void pat_evaluate(const pat_engine_t *e, const uint8_t spoofed[6],
                  pat_verdict_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_ATTRIB_H */
