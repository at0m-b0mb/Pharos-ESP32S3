/* Pharos - evil twin and rogue access point detection
 *
 * Pure C. Given every access point currently advertising one SSID, decide
 * whether one of them is pretending.
 *
 * The whole difficulty of this problem is a false positive that every naive
 * implementation produces: a normal corporate network has one SSID on twenty
 * BSSIDs, and that is not an attack, it is roaming. So in this engine
 * **BSSID multiplicity scores exactly zero**. What earns points is
 * inconsistency - one member of the group offering materially less security
 * than the others, wearing a different vendor's address, sitting on a channel
 * none of the others use, or arriving far louder than the rest because it is
 * in the room with you rather than in the ceiling.
 *
 * And the ordering matters: an alarm requires the posture family
 * specifically. A different vendor on an odd channel is a different vendor on
 * an odd channel. It only becomes an evil twin when it is also asking for
 * less than its siblings, because taking your credentials or your plaintext
 * is the entire point of the attack.
 */
#ifndef PHAROS_TWIN_H
#define PHAROS_TWIN_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_census.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PT_MAX_GROUP   16
#define PT_MAX_PROFILE 32

#define PT_FAM_POSTURE   (1u << 0) /* asks for less than its siblings  */
#define PT_FAM_IDENTITY  (1u << 1) /* wears an address it should not   */
#define PT_FAM_BEHAVIOUR (1u << 2) /* sits or shouts where it should not */

typedef enum {
    PT_BAND_CONSISTENT = 0, /*  0-19  looks like one deployment      */
    PT_BAND_MIXED,          /* 20-44  mixed estate, or mid-migration */
    PT_BAND_ANOMALOUS,      /* 45-69  one member does not belong     */
    PT_BAND_TWIN_LIKELY,    /* 70-100 impersonation likely           */
} pt_band_t;

#define PT_NOTE_SINGLE      (1u << 0) /* one AP: nothing to compare       */
#define PT_NOTE_THIN        (1u << 1) /* some members barely heard        */
#define PT_NOTE_NO_PROFILE  (1u << 2) /* no site baseline loaded          */
#define PT_NOTE_ROAMING     (1u << 3) /* consistent group, multiplicity ignored */
#define PT_NOTE_OPEN_MEMBER (1u << 4) /* one member needs no key at all   */
#define PT_NOTE_LOCAL_MAC   (1u << 5) /* locally administered address     */

/* A site baseline: the BSSIDs that are supposed to be carrying this SSID.
 * Loading one is the single biggest thing an operator can do for accuracy,
 * which is why its absence is disclosed on every verdict. */
typedef struct {
    uint8_t bssid[PT_MAX_PROFILE][6];
    unsigned n;
    bool loaded;
} pt_profile_t;

typedef struct {
    uint16_t dwell_permil;
} pt_context_t;

typedef struct {
    uint8_t score;
    uint8_t raw_score;
    uint8_t ceiling;
    uint8_t families;
    uint8_t notes;
    pt_band_t band;

    uint8_t c_posture, c_identity, c_behaviour;

    uint8_t members;         /* APs in the group                     */
    uint8_t suspect_index;   /* which member looks wrong             */
    uint8_t suspect[6];
    uint8_t best_grade_score;
    uint8_t worst_grade_score;
    int8_t rssi_excess;      /* dB the suspect exceeds the group median */
} pt_verdict_t;

void pt_profile_reset(pt_profile_t *p);
bool pt_profile_add(pt_profile_t *p, const uint8_t bssid[6]);
bool pt_profile_contains(const pt_profile_t *p, const uint8_t bssid[6]);

/* aps must all be advertising the same SSID. profile may be NULL. */
void pt_evaluate(const pc_ap_t *aps, unsigned n, const pt_profile_t *profile,
                 const pt_context_t *ctx, pt_verdict_t *out);

uint8_t pt_ceiling(const pt_context_t *ctx, const pt_profile_t *profile);

const char *pt_band_name(pt_band_t band);
const char *pt_band_advice(pt_band_t band);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_TWIN_H */
