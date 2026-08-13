/* Pharos - access point posture grading
 *
 * Pure C. Takes what a beacon told us about a network and grades how well it
 * would resist the attacks Pharos can already see being attempted.
 *
 * The grade is not a vibe. It is four components summing to 100 - what the
 * network requires to join (45), whether its management frames are protected
 * (25), the cipher it negotiates (15), and what it exposes for convenience
 * (15) - and then a set of ceilings that model the fact that some weaknesses
 * are not gradual. An open network is not "a C with points off"; it is an F,
 * because the first attacker to walk past is already inside.
 *
 * The MFP ceiling is the one that ties this engine to the rest of the device:
 * a network without 802.11w cannot exceed a B, because the deauthentication
 * flood that pharos_watch grades will work on it. The two engines are talking
 * about the same weakness from opposite ends.
 *
 * Nothing here grades a network "secure". The best grade is A+, and its
 * headline says what it survived, not that it is safe.
 */
#ifndef PHAROS_CENSUS_H
#define PHAROS_CENSUS_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_dot11.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PC_SSID_MAX 32

/* What one beacon told us. Assembled by the Census lens from the fixed
 * header plus an information-element walk on the analytics core. */
typedef struct {
    uint8_t bssid[6];
    char ssid[PC_SSID_MAX + 1];
    uint8_t ssid_len;
    uint8_t channel;
    int8_t rssi;
    uint16_t beacons;      /* how many times we heard it              */
    uint16_t beacon_ms;    /* advertised beacon interval              */
    bool hidden;           /* SSID element empty or all-zero          */
    bool privacy;          /* capability field Privacy bit            */
    bool wpa1_ie;          /* legacy WPA vendor element present       */
    bool wps_present;
    bool wps_pin;          /* the PIN method specifically             */
    bool ccmp_pairwise;    /* CCMP or GCMP offered                    */
    bool tkip_pairwise;
    bool akm_8021x;        /* enterprise                              */
    pharos_rsn_t rsn;
} pc_ap_t;

typedef enum {
    PC_GRADE_UNGRADED = 0, /* not enough observation to say anything */
    PC_GRADE_F,
    PC_GRADE_E,
    PC_GRADE_D,
    PC_GRADE_C,
    PC_GRADE_B,
    PC_GRADE_A,
    PC_GRADE_A_PLUS,
} pc_grade_t;

/* Which ceiling bound the score. Shown in the UI so the operator sees the
 * one thing to fix rather than a number to argue with. */
#define PC_CAP_OPEN     (1u << 0) /* no authentication at all       */
#define PC_CAP_WEP      (1u << 1) /* privacy bit, no RSN            */
#define PC_CAP_TKIP     (1u << 2) /* TKIP anywhere in the offer     */
#define PC_CAP_WPS_PIN  (1u << 3) /* WPS PIN method advertised      */
#define PC_CAP_NO_MFP   (1u << 4) /* deauth floods will work here   */
#define PC_CAP_WPA1     (1u << 5) /* legacy WPA only                */

#define PC_NOTE_HIDDEN      (1u << 0) /* hiding an SSID is not security  */
#define PC_NOTE_TRANSITION  (1u << 1) /* WPA3 downgradeable to WPA2      */
#define PC_NOTE_THIN        (1u << 2) /* fewer than 3 beacons heard      */
#define PC_NOTE_ENTERPRISE  (1u << 3)
#define PC_NOTE_OWE         (1u << 4) /* encrypted, but anyone may join  */

typedef struct {
    uint8_t score;
    uint8_t raw_score;
    pc_grade_t grade;
    uint8_t c_auth, c_mfp, c_cipher, c_exposure;
    uint8_t caps_applied;
    uint8_t notes;
    const char *headline; /* the single sentence the card leads with */
} pc_verdict_t;

void pc_grade(const pc_ap_t *ap, pc_verdict_t *out);

const char *pc_grade_name(pc_grade_t g);

/* The one thing to fix first, given what bound the score. */
const char *pc_grade_advice(const pc_verdict_t *v);

/* Sort key for the census list: worst posture first, then strongest signal,
 * so the thing a defender should look at is at the top of the dial. Returns
 * <0 if a should sort before b. */
int pc_compare(const pc_ap_t *a, const pc_verdict_t *va,
               const pc_ap_t *b, const pc_verdict_t *vb);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_CENSUS_H */
