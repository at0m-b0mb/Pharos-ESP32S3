#include "pharos_sentinel.h"

#include <string.h>

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static bool ssid_eq(const char *a, uint8_t alen, const char *b, uint8_t blen)
{
    return alen == blen && alen > 0 && memcmp(a, b, alen) == 0;
}

void ps_reset(ps_baseline_t *b)
{
    if (b) {
        memset(b, 0, sizeof(*b));
    }
}

/* Snapshot one observed AP into a baseline record, grading it as we go so the
 * comparison later is against a number rather than a re-derivation. */
static void record_of(ps_record_t *r, const pc_ap_t *ap)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->bssid, ap->bssid, 6);
    r->ssid_len = ap->ssid_len > PC_SSID_MAX ? PC_SSID_MAX : ap->ssid_len;
    memcpy(r->ssid, ap->ssid, r->ssid_len);
    r->ssid[r->ssid_len] = '\0';
    r->channel = ap->channel;

    pc_verdict_t v;
    pc_grade(ap, &v);
    r->grade_score = v.score;
    r->mfp = ap->rsn.mfp_capable || ap->rsn.mfp_required;
    r->open = (v.caps_applied & (PC_CAP_OPEN | PC_CAP_WEP)) != 0;
    r->in_use = true;
}

unsigned ps_adopt(ps_baseline_t *b, const pc_ap_t *aps, unsigned n, uint64_t t_us)
{
    if (!b || !aps) {
        return 0;
    }
    memset(b, 0, sizeof(*b));
    for (unsigned i = 0; i < n && b->n < PS_MAX_BASELINE; i++) {
        record_of(&b->aps[b->n++], &aps[i]);
    }
    b->adopted = b->n > 0;
    b->adopted_us = t_us;
    return b->n;
}

uint8_t ps_ceiling(const ps_context_t *ctx)
{
    const uint32_t dwell = clamp_u32(ctx ? ctx->dwell_permil : 1000, 1, 1000);
    uint32_t c = 55u + (40u * dwell) / 1000u;
    return (uint8_t)clamp_u32(c, 45u, 95u);
}

static ps_band_t band_of(uint8_t score)
{
    if (score >= 70) return PS_BAND_INVESTIGATE;
    if (score >= 40) return PS_BAND_NOTABLE;
    if (score >= 15) return PS_BAND_DRIFT;
    return PS_BAND_UNCHANGED;
}

static ps_finding_t *add_finding(ps_verdict_t *out, ps_change_t c,
                                 const uint8_t bssid[6], const char *ssid,
                                 uint8_t ssid_len)
{
    if (out->n_findings >= PS_MAX_FINDINGS) {
        out->notes |= PS_NOTE_FULL;
        return NULL;
    }
    ps_finding_t *f = &out->findings[out->n_findings++];
    memset(f, 0, sizeof(*f));
    f->change = c;
    memcpy(f->bssid, bssid, 6);
    const uint8_t n = ssid_len > PC_SSID_MAX ? PC_SSID_MAX : ssid_len;
    if (ssid && n) {
        memcpy(f->ssid, ssid, n);
    }
    f->ssid[n] = '\0';
    return f;
}

void ps_compare(const ps_baseline_t *b, const pc_ap_t *aps, unsigned n,
                const ps_context_t *ctx, ps_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "";
    if (!ctx) {
        return;
    }
    out->ceiling = ps_ceiling(ctx);

    const uint32_t dwell = clamp_u32(ctx->dwell_permil, 1, 1000);
    if (dwell < 500) {
        out->notes |= PS_NOTE_THIN_SWEEP;
    }

    if (!b || !b->adopted) {
        out->notes |= PS_NOTE_NO_BASELINE;
        out->band = PS_BAND_UNCHANGED;
        out->headline = "No baseline adopted - nothing to compare against";
        return;
    }
    if (!aps) {
        n = 0;
    }

    uint32_t worst = 0;

    /* --- things present now: NEW, or changed since the baseline ---------- */
    for (unsigned i = 0; i < n; i++) {
        const pc_ap_t *ap = &aps[i];
        const ps_record_t *was = NULL;
        for (unsigned k = 0; k < b->n; k++) {
            if (b->aps[k].in_use && mac_eq(b->aps[k].bssid, ap->bssid)) {
                was = &b->aps[k];
                break;
            }
        }

        pc_verdict_t v;
        pc_grade(ap, &v);
        const bool now_open = (v.caps_applied & (PC_CAP_OPEN | PC_CAP_WEP)) != 0;
        const bool now_mfp = ap->rsn.mfp_capable || ap->rsn.mfp_required;

        if (!was) {
            /* A radio we have not seen here before. Ordinary on its own -
             * estates grow, neighbours move in - so it scores low unless it is
             * also weak, or wearing an SSID the baseline already owns, which is
             * the shape of an impersonator rather than a newcomer. */
            uint32_t sev = 15;
            bool reuse = false;
            for (unsigned k = 0; k < b->n; k++) {
                if (b->aps[k].in_use &&
                    ssid_eq(b->aps[k].ssid, b->aps[k].ssid_len, ap->ssid, ap->ssid_len)) {
                    reuse = true;
                    break;
                }
            }
            if (reuse) {
                sev += 30;
                out->notes |= PS_NOTE_SSID_REUSE;
            }
            if (now_open) {
                sev += 25;
            }
            sev = clamp_u32(sev, 0, 100);

            ps_finding_t *f = add_finding(out, PS_CHANGE_NEW, ap->bssid,
                                          ap->ssid, ap->ssid_len);
            if (f) {
                f->severity = (uint8_t)sev;
                f->now = v.score;
                f->channel_now = ap->channel;
            }
            out->n_new++;
            if (sev > worst) worst = sev;
            continue;
        }

        /* Same radio as the baseline. What moved? */
        if (was->grade_score > v.score || (was->mfp && !now_mfp) ||
            (!was->open && now_open)) {
            /* THE finding that matters. An AP that dropped protection is
             * either a misconfiguration or somebody wearing its address, and
             * both need a human today. */
            uint32_t sev;
            if (!was->open && now_open) {
                sev = 90; /* was protected, now needs no key at all */
            } else if (was->mfp && !now_mfp) {
                sev = 75; /* lost 802.11w: deauth attacks work again */
            } else {
                sev = 40u + clamp_u32((uint32_t)(was->grade_score - v.score), 0, 40);
            }
            ps_finding_t *f = add_finding(out, PS_CHANGE_DOWNGRADE, ap->bssid,
                                          ap->ssid, ap->ssid_len);
            if (f) {
                f->severity = (uint8_t)sev;
                f->was = was->grade_score;
                f->now = v.score;
                f->channel_was = was->channel;
                f->channel_now = ap->channel;
            }
            out->n_downgrade++;
            if (sev > worst) worst = sev;
        } else if (v.score > was->grade_score) {
            ps_finding_t *f = add_finding(out, PS_CHANGE_UPGRADE, ap->bssid,
                                          ap->ssid, ap->ssid_len);
            if (f) {
                f->severity = 5;
                f->was = was->grade_score;
                f->now = v.score;
            }
            out->n_upgrade++;
            if (worst < 5) worst = 5;
        }

        if (was->channel != ap->channel && ap->channel) {
            ps_finding_t *f = add_finding(out, PS_CHANGE_MOVED, ap->bssid,
                                          ap->ssid, ap->ssid_len);
            if (f) {
                f->severity = 12;
                f->channel_was = was->channel;
                f->channel_now = ap->channel;
            }
            out->n_moved++;
            if (worst < 12) worst = 12;
        }
        if (!ssid_eq(was->ssid, was->ssid_len, ap->ssid, ap->ssid_len) &&
            (was->ssid_len || ap->ssid_len)) {
            ps_finding_t *f = add_finding(out, PS_CHANGE_RENAMED, ap->bssid,
                                          ap->ssid, ap->ssid_len);
            if (f) {
                f->severity = 25;
            }
            out->n_renamed++;
            if (worst < 25) worst = 25;
        }
    }

    /* --- baseline entries not heard this sweep: MISSING ------------------ */
    for (unsigned k = 0; k < b->n; k++) {
        if (!b->aps[k].in_use) {
            continue;
        }
        bool seen = false;
        for (unsigned i = 0; i < n; i++) {
            if (mac_eq(aps[i].bssid, b->aps[k].bssid)) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        /* Scored lowest of everything, and scaled DOWN by how little of the
         * band this sweep heard: on a hopping receiver an AP you did not hear
         * is far more likely to have been missed than removed. */
        uint32_t sev = (8u * dwell) / 1000u;
        ps_finding_t *f = add_finding(out, PS_CHANGE_MISSING, b->aps[k].bssid,
                                      b->aps[k].ssid, b->aps[k].ssid_len);
        if (f) {
            f->severity = (uint8_t)sev;
            f->was = b->aps[k].grade_score;
            f->channel_was = b->aps[k].channel;
        }
        out->n_missing++;
        if (sev > worst) worst = sev;
    }

    /* The estate's score is its worst finding, nudged by how many there are -
     * ten small changes at once is itself worth a look. */
    uint32_t score = worst;
    if (out->n_findings > 3) {
        score += clamp_u32(out->n_findings - 3u, 0, 10);
    }
    score = clamp_u32(score, 0, 100);
    out->raw_score = (uint8_t)score;
    if (score > out->ceiling) {
        score = out->ceiling;
    }
    out->score = (uint8_t)score;
    out->band = band_of(out->score);

    switch (out->band) {
    case PS_BAND_INVESTIGATE:
        out->headline = out->n_downgrade
                            ? "A network is weaker than when you adopted the baseline"
                            : "A new radio here looks wrong";
        break;
    case PS_BAND_NOTABLE:
        out->headline = "Something changed that is worth a look";
        break;
    case PS_BAND_DRIFT:
        out->headline = "Ordinary churn since the baseline";
        break;
    case PS_BAND_UNCHANGED:
    default:
        out->headline = "The estate looks as you left it";
        break;
    }
}

const char *ps_change_name(ps_change_t c)
{
    switch (c) {
    case PS_CHANGE_MISSING:   return "MISSING";
    case PS_CHANGE_NEW:       return "NEW";
    case PS_CHANGE_MOVED:     return "MOVED";
    case PS_CHANGE_RENAMED:   return "RENAMED";
    case PS_CHANGE_UPGRADE:   return "UPGRADE";
    case PS_CHANGE_DOWNGRADE: return "DOWNGRADE";
    case PS_CHANGE_NONE:
    default:                  return "-";
    }
}

const char *ps_band_name(ps_band_t band)
{
    switch (band) {
    case PS_BAND_UNCHANGED:   return "UNCHANGED";
    case PS_BAND_DRIFT:       return "DRIFT";
    case PS_BAND_NOTABLE:     return "NOTABLE";
    case PS_BAND_INVESTIGATE: return "INVESTIGATE";
    default:                  return "?";
    }
}

const char *ps_band_advice(ps_band_t band)
{
    switch (band) {
    case PS_BAND_UNCHANGED:
        return "Every radio in the baseline is still here and still configured "
               "the way it was. This receiver hears one channel at a time, so "
               "that is a good sign rather than a guarantee.";
    case PS_BAND_DRIFT:
        return "Small changes only - a radio moved channel, or something new "
               "appeared. Normal for a live estate; worth skimming.";
    case PS_BAND_NOTABLE:
        return "Something changed that a defender should look at: a rename, a "
               "cluster of new radios, or a small drop in posture.";
    case PS_BAND_INVESTIGATE:
        return "A network is materially weaker than your baseline, or a new "
               "radio is wearing a name that belongs to the estate. Treat as a "
               "misconfiguration or an impersonation until you know which.";
    default:
        return "";
    }
}
