/* Pharos - KARMA / MANA rogue access point detection
 *
 * Pure C. The detection nobody does on cheap hardware, and the natural
 * companion to the Probe lens.
 *
 * The attack: a phone shouts the names of networks it has joined before (see
 * pharos_probe.h). A KARMA-style rogue AP listens for those shouts and simply
 * answers "yes, that's me" to every one of them. The phone, having asked for
 * "HomeNet_5G" and been told it is right here, associates. MANA is the same
 * idea with a curated list. It is devastating precisely because the victim
 * device volunteers the credential-shaped part first.
 *
 * The detection, and why it is passive: an honest access point *announces*
 * the networks it carries, in beacons, continuously. A KARMA responder cannot
 * - it does not know which names to advertise until somebody asks. So it
 * answers probe requests for names it never beacons, and the more phones walk
 * past, the wider the gap between "SSIDs this radio has answered for" and
 * "SSIDs this radio has ever announced on its own".
 *
 * That gap is the whole signal, and it is why the ABSENCE family is required
 * for an alarm. A legitimate multi-SSID deployment - a corporate AP carrying
 * guest, staff and IoT networks - answers for several SSIDs too, but it also
 * *beacons* every one of them. It scores zero here, by construction.
 *
 * As everywhere in Pharos, "I never heard it beacon" is weak evidence while
 * the receiver is hopping, so the ABSENCE score is scaled by measured dwell
 * and the verdict carries a confidence ceiling.
 */
#ifndef PHAROS_KARMA_H
#define PHAROS_KARMA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PK_SSID_MAX      32
#define PK_MAX_RESPONDERS 16
#define PK_MAX_SSIDS      12 /* tracked per responder */
#define PK_MAX_RECENT     24 /* recent probe requests, for echo detection */

/* A response is "prompt" if it follows a probe for the same name inside this
 * window. Real APs answer fast too, so this is corroboration, not proof. */
#define PK_ECHO_WINDOW_US 500000ull /* 500 ms */

#define PK_FAM_BREADTH (1u << 0) /* answers for many names            */
#define PK_FAM_ABSENCE (1u << 1) /* names it never announces itself   */
#define PK_FAM_ECHO    (1u << 2) /* answers arrive right after asking */

typedef enum {
    PK_BAND_NORMAL = 0,  /*  0-19  looks like an honest access point */
    PK_BAND_MIXED,       /* 20-44  several networks, all announced   */
    PK_BAND_SUSPICIOUS,  /* 45-69  answering for names it never says */
    PK_BAND_KARMA_LIKELY,/* 70-100 responder impersonating on demand */
} pk_band_t;

#define PK_NOTE_THIN_DWELL (1u << 0) /* hopping: "never beaconed" is weak  */
#define PK_NOTE_NO_PROBES  (1u << 1) /* heard no probe requests at all     */
#define PK_NOTE_MULTI_SSID (1u << 2) /* answers for many, announces them   */
#define PK_NOTE_LOCAL_MAC  (1u << 3) /* locally administered address       */
#define PK_NOTE_TABLE_FULL (1u << 4) /* more names than we can track       */

typedef struct {
    uint16_t dwell_permil;
    uint16_t bus_yield_permil;
} pk_context_t;

typedef struct {
    char name[PK_SSID_MAX + 1];
    uint8_t len;
    bool beaconed;   /* this responder announced it unprompted */
    bool answered;   /* this responder answered a probe for it */
    bool echoed;     /* answered promptly after a probe for it */
} pk_ssid_t;

typedef struct {
    uint8_t bssid[6];
    pk_ssid_t ssids[PK_MAX_SSIDS];
    uint8_t n_ssids;
    uint16_t responses;
    uint16_t beacons;
    int8_t rssi;
    uint8_t channel;
    bool overflow;
    bool in_use;
} pk_responder_t;

typedef struct {
    char name[PK_SSID_MAX + 1];
    uint8_t len;
    uint64_t t_us;
} pk_recent_t;

typedef struct {
    pk_responder_t responders[PK_MAX_RESPONDERS];
    unsigned n_responders;
    pk_recent_t recent[PK_MAX_RECENT];
    unsigned recent_head;
    uint32_t probes_seen;
    uint32_t responses_seen;
} pk_engine_t;

typedef struct {
    uint8_t score;
    uint8_t raw_score;
    uint8_t ceiling;
    uint8_t families;
    uint8_t notes;
    pk_band_t band;

    uint8_t c_breadth, c_absence, c_echo;

    uint8_t suspect[6];
    uint8_t answered_ssids;   /* distinct names it answered for     */
    uint8_t unannounced;      /* of those, never beaconed by it     */
    uint8_t echoed;           /* of those, answered promptly        */
    const char *headline;
} pk_verdict_t;

void pk_reset(pk_engine_t *e);

/* A probe request seen on the air. ssid may be NULL/len 0 for a wildcard,
 * which is recorded but reveals nothing. */
void pk_observe_probe(pk_engine_t *e, const char *ssid, uint8_t len, uint64_t t_us);

/* A beacon: this BSSID announces this network of its own accord. */
void pk_observe_beacon(pk_engine_t *e, const uint8_t bssid[6], const char *ssid,
                       uint8_t len, int8_t rssi, uint8_t channel, uint64_t t_us);

/* A probe response: this BSSID claims to carry this network. */
void pk_observe_response(pk_engine_t *e, const uint8_t bssid[6], const char *ssid,
                         uint8_t len, int8_t rssi, uint8_t channel, uint64_t t_us);

/* Grade the worst responder currently in the table. */
void pk_evaluate(const pk_engine_t *e, const pk_context_t *ctx, pk_verdict_t *out);

uint8_t pk_ceiling(const pk_context_t *ctx);
const char *pk_band_name(pk_band_t band);
const char *pk_band_advice(pk_band_t band);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_KARMA_H */
