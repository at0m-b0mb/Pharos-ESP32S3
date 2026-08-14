/* Pharos - Sentinel: the site baseline and what changed since
 *
 * Pure C. The blue team's most-asked question is not "is anything wrong right
 * now" - the other lenses answer that. It is *"what changed since I last swept
 * this building?"* A new access point that appeared overnight, a network that
 * quietly dropped its 802.11w, a radio that moved channel, an AP that has gone
 * missing: those are the findings that start incidents, and none of them are
 * visible from a single snapshot. They need memory.
 *
 * Sentinel is that memory. Adopt a baseline while you believe the estate is
 * clean, then sweep again later and it diffs the two:
 *
 *   NEW        a BSSID that was not in the baseline
 *   MISSING    a baseline BSSID that is no longer heard
 *   DOWNGRADE  same BSSID, materially weaker security than baseline
 *   UPGRADE    same BSSID, stronger than baseline (good news, still a change)
 *   MOVED      same BSSID, different channel
 *   RENAMED    same BSSID, different SSID
 *
 * The severity model is the point, and it is deliberately asymmetric:
 *
 *   - A **downgrade** is the finding that matters. An AP that dropped from
 *     WPA3+MFP to WPA2, or from protected to open, is either a misconfiguration
 *     or an impersonation, and both need a human. It is scored highest.
 *   - A **new** AP is interesting but ordinary: estates grow, neighbours move
 *     in, someone tethers a phone. It is scored low on its own, and only rises
 *     when it is *also* weak or wearing a familiar SSID.
 *   - **Missing** is scored lowest of all, because this receiver hears one
 *     channel at a time: an AP you did not hear this sweep is far more likely
 *     to have been missed than removed. That asymmetry is the honesty here -
 *     absence of evidence, again, is not evidence of absence.
 *
 * As everywhere, the verdict carries a confidence ceiling from the sweep
 * quality, and MISSING findings are additionally discounted by how thin the
 * sweep was.
 */
#ifndef PHAROS_SENTINEL_H
#define PHAROS_SENTINEL_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_census.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS_MAX_BASELINE 48
#define PS_MAX_FINDINGS 32

typedef enum {
    PS_CHANGE_NONE = 0,
    PS_CHANGE_MISSING,    /* in the baseline, not heard now       */
    PS_CHANGE_NEW,        /* heard now, not in the baseline       */
    PS_CHANGE_MOVED,      /* same radio, different channel        */
    PS_CHANGE_RENAMED,    /* same radio, different network name   */
    PS_CHANGE_UPGRADE,    /* same radio, stronger than before     */
    PS_CHANGE_DOWNGRADE,  /* same radio, WEAKER than before       */
    PS_CHANGE_COUNT,
} ps_change_t;

typedef enum {
    PS_BAND_UNCHANGED = 0, /*  0-14  the estate looks as you left it  */
    PS_BAND_DRIFT,         /* 15-39  ordinary churn                   */
    PS_BAND_NOTABLE,       /* 40-69  something worth a look           */
    PS_BAND_INVESTIGATE,   /* 70-100 a downgrade or a weak newcomer   */
} ps_band_t;

#define PS_NOTE_NO_BASELINE (1u << 0) /* nothing adopted yet          */
#define PS_NOTE_THIN_SWEEP  (1u << 1) /* hopping: MISSING is unreliable */
#define PS_NOTE_FULL        (1u << 2) /* more findings than we can hold */
#define PS_NOTE_SSID_REUSE  (1u << 3) /* a newcomer wears a known SSID  */

typedef struct {
    uint16_t dwell_permil; /* how much of the band this sweep heard */
    uint32_t sweep_ms;     /* how long the sweep ran                */
} ps_context_t;

typedef struct {
    uint8_t bssid[6];
    char ssid[PC_SSID_MAX + 1];
    uint8_t ssid_len;
    uint8_t channel;
    uint8_t grade_score;   /* pc_grade() score at adoption          */
    bool mfp;              /* had protected management frames       */
    bool open;             /* needed no key                         */
    bool in_use;
} ps_record_t;

typedef struct {
    ps_record_t aps[PS_MAX_BASELINE];
    unsigned n;
    bool adopted;
    uint64_t adopted_us;
} ps_baseline_t;

typedef struct {
    ps_change_t change;
    uint8_t bssid[6];
    char ssid[PC_SSID_MAX + 1];
    uint8_t severity;      /* 0..100 for this one finding           */
    uint8_t was, now;      /* grade scores before/after (0 if n/a)  */
    uint8_t channel_was, channel_now;
} ps_finding_t;

typedef struct {
    uint8_t score;
    uint8_t raw_score;
    uint8_t ceiling;
    uint8_t notes;
    ps_band_t band;

    uint8_t n_findings;
    ps_finding_t findings[PS_MAX_FINDINGS];

    uint8_t n_new, n_missing, n_downgrade, n_upgrade, n_moved, n_renamed;
    const char *headline;
} ps_verdict_t;

void ps_reset(ps_baseline_t *b);

/* Adopt what is currently in view as the trusted baseline. A deliberate act:
 * it is only meaningful when the operator believes the estate is clean, which
 * is why it is a command and not something the firmware does on its own.
 * Returns how many records were stored. */
unsigned ps_adopt(ps_baseline_t *b, const pc_ap_t *aps, unsigned n, uint64_t t_us);

/* Diff a fresh sweep against the baseline. */
void ps_compare(const ps_baseline_t *b, const pc_ap_t *aps, unsigned n,
                const ps_context_t *ctx, ps_verdict_t *out);

uint8_t ps_ceiling(const ps_context_t *ctx);
const char *ps_change_name(ps_change_t c);
const char *ps_band_name(ps_band_t band);
const char *ps_band_advice(ps_band_t band);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_SENTINEL_H */
