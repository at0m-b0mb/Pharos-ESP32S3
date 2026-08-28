/* Pharos - Harvest: somebody is collecting your handshakes
 *
 * Pure C. The attack this detects is the one that ends in a cracked Wi-Fi
 * password, and it is quiet: nothing goes down, no user complains, and by the
 * time the passphrase is broken the attacker is a week away and somewhere
 * else. The capture itself lasts seconds. If you are not watching *at that
 * moment*, there is nothing left to find.
 *
 * Two routes, and Pharos distinguishes them because the response differs:
 *
 *   FORCED (the classic).  Deauthenticate a client, wait for it to come
 *   straight back, and record messages 1 and 2 of the 4-way handshake as it
 *   reconnects. Those two messages are exchanged before the pairwise key
 *   exists, so they are readable by anyone in range. The tell is the *order
 *   and the tightness*: a disconnect, then a handshake, within a second or
 *   two, again and again.
 *
 *   PMKID (clientless).  Associate to the access point yourself and it may
 *   hand you a PMKID in message 1, from which the passphrase can be attacked
 *   with no client involved at all. No deauthentication, no victim, no
 *   outage - and no second message, because the attacker never intended to
 *   finish connecting. An unanswered PMKID request is a very deliberate
 *   thing; ordinary clients complete their handshakes.
 *
 * What makes this honest rather than alarmist:
 *
 *   - A handshake is not an attack. Every device performs one every time it
 *     joins a network. Seeing them is normal and the QUIET band says so.
 *   - One deauthentication followed by one handshake is not an attack either:
 *     access points reboot, clients roam, radios reset. The engine requires
 *     REPETITION or BREADTH before it will use the word harvest.
 *   - Where the network requires 802.11w, forged deauthentication is rejected
 *     by the client, so a handshake following one is far more likely to be a
 *     coincidence than a consequence. That evidence is explicitly discounted.
 *   - As everywhere in Pharos, the alarm band needs more than one evidence
 *     family, and the confidence ceiling falls with the sweep quality.
 *
 * Pharos cannot capture a handshake in any useful sense and could not use one
 * if it did: it stores no nonces, no MICs and no key data, and it has no
 * transmitter with which to complete or replay anything. This is the defender
 * being told that their handshakes are being taken.
 */
#ifndef PHAROS_HARVEST_H
#define PHAROS_HARVEST_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_MAX_PAIRS 24 /* distinct (access point, client) conversations */

/* How soon after a disconnect a handshake counts as caused by it. Chosen to
 * cover a normal client's reconnect and little else. */
#define PH_FORCE_WINDOW_US 3000000ull /* 3 s */

typedef enum {
    PH_BAND_QUIET = 0,      /*  0-19  nothing that looks like collection   */
    PH_BAND_HANDSHAKES,     /* 20-44  handshakes seen, all explainable     */
    PH_BAND_SUSPECTED,      /* 45-69  a forced cycle, or an unfinished PMKID */
    PH_BAND_HARVEST_LIKELY, /* 70-100 repetition or breadth: a collector   */
    PH_BAND_COUNT,
} ph_band_t;

/* Evidence families. As everywhere in Pharos, the top band needs more than
 * one of these - a single family, however loud, cannot raise the alarm. */
#define PH_FAM_FORCED  (1u << 0) /* disconnect, then handshake, tightly    */
#define PH_FAM_PMKID   (1u << 1) /* PMKID solicited and never completed    */
#define PH_FAM_REPEAT  (1u << 2) /* the same victim forced again and again */
#define PH_FAM_BREADTH (1u << 3) /* several victims, same pattern          */
/* ASSOCIATED, TOOK WHAT IT CAME FOR, AND LEFT.
 *
 * hcxdumptool's primary PMKID vector is simply to CONNECT to the access point
 * - the AP hands out a PMKID in message 1 and the tool walks away. Watching
 * for that message 1 means watching for a single brief DATA frame, which is
 * exactly what this lens spent a whole live attack failing to catch.
 *
 * The association request is a MANAGEMENT frame. It is never encrypted, it is
 * always delivered to a sniffer, and no PMKID attack can skip it. Watching the
 * approach instead of the payload is both easier and harder to evade.
 *
 * A real client associates and then USES the network. A harvester associates
 * and is never heard from again. */
#define PH_FAM_TOUCH_GO (1u << 4)

#define PH_NOTE_THIN_SWEEP  (1u << 0) /* hopping: cycles are easily missed */
#define PH_NOTE_DROPS       (1u << 1) /* the ingest ring lost frames       */
#define PH_NOTE_MFP         (1u << 2) /* 802.11w seen: deauth should fail  */
#define PH_NOTE_FULL        (1u << 3) /* more conversations than we track  */
#define PH_NOTE_PROTECTED   (1u << 4) /* M3/M4 are encrypted; we see 1 & 2 */
/* THIS NETWORK DOES NOT APPEAR TO HAND OUT PMKIDs.
 *
 * A zero on the PMKID row meant two completely different things and looked
 * identical: "nothing has happened yet" and "this network is not vulnerable to
 * the clientless attack at all". The second is a security FINDING and was
 * being displayed as a blank.
 *
 * There is no honest way to read this off a beacon - PMKID caching is not
 * advertised in the RSN element, and opportunistic key caching is not
 * advertised anywhere. So it is measured instead: after enough message-1
 * frames have gone by carrying no PMKID, the absence is worth reporting. The
 * claim is only as strong as the sample, which is why it needs one. */
#define PH_NOTE_NO_PMKID    (1u << 5)

/* How many message 1s must pass before their silence means something. */
#define PH_PMKID_SAMPLE 3u

typedef struct {
    uint16_t dwell_permil; /* how much of the band this sweep heard     */
    uint16_t yield_permil; /* how much of the offered traffic survived  */
    bool mfp_required;     /* the network under discussion requires 11w */
} ph_context_t;

/* One access-point/client conversation. */
typedef struct {
    uint8_t bssid[6];
    uint8_t client[6];
    bool in_use;

    uint32_t deauths;   /* disconnects seen either way        */
    uint32_t m1, m2;    /* handshake messages seen            */
    uint32_t forced;    /* handshakes tightly following one   */
    uint32_t pmkid_req; /* message 1 carrying a PMKID         */
    uint32_t pmkid_orphan; /* ... that no message 2 answered  */

    uint64_t last_deauth_us;
    /* Separate from the timestamp above, because 0 is a perfectly good time:
     * a disconnect in the first microsecond of a sweep must arm the window
     * like any other. Using the timestamp itself as the sentinel silently
     * dropped exactly that case. */
    bool deauth_armed;
    uint64_t last_m1_us;
    bool m1_pending_pmkid; /* an unanswered PMKID is outstanding */

    /* The approach, and whether anything came of it. See PH_FAM_TOUCH_GO. */
    uint32_t assoc_req;
    bool data_since_assoc;

    /* HAVE WE EVER HEARD THIS CLIENT SPEAK?
     *
     * Message 1 comes from the access point and message 2 from the client, and
     * this device has one antenna. An access point across the room arrives at
     * -40 dBm; the phone it is talking to may be in a pocket, behind a wall,
     * or simply quieter. Hearing M1 and not M2 is therefore the ORDINARY case
     * for a passive receiver, not a remarkable one.
     *
     * Without this the engine read "no message 2" as "the handshake was
     * abandoned", which is absence of evidence dressed up as evidence of
     * absence - and scored a single ordinary roam at 46/96, SUSPECTED. It
     * accused a quiet network of being under attack.
     *
     * So an unanswered request only counts against somebody we can actually
     * hear. Zero frames from this client means the silence is ours, not
     * theirs. */
} ph_pair_t;

/* Clients we have actually heard TRANSMIT.
 *
 * Kept beside the pairs rather than on them, because a client's ordinary
 * traffic usually arrives BEFORE its handshake - so a flag on the pair is
 * written only if the frames happen to come in the luckier order, and the
 * first version of this silently counted nothing at all. */
#define PH_MAX_HEARD 24

typedef struct {
    ph_pair_t pairs[PH_MAX_PAIRS];
    unsigned n;

    uint8_t heard[PH_MAX_HEARD][6];
    unsigned heard_n;

    uint32_t m1_total, m1_pmkid; /* the PMKID sample, see PH_NOTE_NO_PMKID */
    bool full;
    uint32_t handshakes;  /* total, across everything          */
    uint32_t deauths;     /* total                             */
    bool mfp_seen;
} ph_state_t;

typedef struct {
    uint8_t score;
    uint8_t raw_score;
    uint8_t ceiling;
    uint8_t families; /* PH_FAM_* bitmap    */
    uint8_t notes;    /* PH_NOTE_* bitmap   */
    ph_band_t band;

    uint32_t forced_cycles;  /* deauth-then-handshake pairs      */
    uint32_t pmkid_orphans;  /* solicited, never completed       */
    uint32_t m1_seen;        /* message 1s, whatever they carried */
    uint32_t m1_with_pmkid;  /* ...of which carried a PMKID       */
    uint32_t assoc_reqs;     /* association attempts seen        */
    uint32_t touch_and_go;   /* ...that never carried any data   */
    uint32_t victims;        /* distinct clients showing forcing */
    uint32_t handshakes;     /* total handshakes observed        */

    uint8_t worst_client[6]; /* the most-forced client, if any   */
    uint8_t worst_bssid[6];
    const char *headline;
} ph_verdict_t;

void ph_reset(ph_state_t *s);

/* Feed one frame. Deauthentication/disassociation and EAPOL-Key messages are
 * the only things that matter; everything else is ignored cheaply. */
void ph_observe(ph_state_t *s, const pharos_ev_dot11_t *f, uint64_t t_us);

/* A sweep ended without the outstanding PMKID request ever being answered.
 * Call before evaluating so an unfinished solicitation is counted as one. */
void ph_settle(ph_state_t *s);

void ph_evaluate(const ph_state_t *s, const ph_context_t *ctx, ph_verdict_t *out);

uint8_t ph_ceiling(const ph_context_t *ctx);
const char *ph_band_name(ph_band_t b);
const char *ph_band_advice(ph_band_t b);
const char *ph_family_name(unsigned family_bit);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_HARVEST_H */
