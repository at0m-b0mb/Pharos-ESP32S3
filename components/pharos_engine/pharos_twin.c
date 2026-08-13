#include "pharos_twin.h"

#include <string.h>

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void pt_profile_reset(pt_profile_t *p)
{
    if (p) {
        memset(p, 0, sizeof(*p));
    }
}

bool pt_profile_add(pt_profile_t *p, const uint8_t bssid[6])
{
    if (!p || !bssid || p->n >= PT_MAX_PROFILE) {
        return false;
    }
    if (pt_profile_contains(p, bssid)) {
        return true;
    }
    memcpy(p->bssid[p->n++], bssid, 6);
    p->loaded = true;
    return true;
}

bool pt_profile_contains(const pt_profile_t *p, const uint8_t bssid[6])
{
    if (!p || !bssid) {
        return false;
    }
    for (unsigned i = 0; i < p->n; i++) {
        if (memcmp(p->bssid[i], bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

uint8_t pt_ceiling(const pt_context_t *ctx, const pt_profile_t *profile)
{
    const uint32_t dwell = clamp_u32(ctx ? ctx->dwell_permil : 1000, 1, 1000);
    uint32_t c = 55u + (35u * dwell) / 1000u; /* 55..90 */
    if (profile && profile->loaded) {
        /* Knowing which BSSIDs are supposed to be here is real evidence, not
         * an assumption, so it genuinely raises what a verdict may claim. */
        c += 8u;
    }
    return (uint8_t)clamp_u32(c, 45u, 95u);
}

static pt_band_t band_of(uint8_t score)
{
    if (score >= 70) return PT_BAND_TWIN_LIKELY;
    if (score >= 45) return PT_BAND_ANOMALOUS;
    if (score >= 20) return PT_BAND_MIXED;
    return PT_BAND_CONSISTENT;
}

static bool is_locally_administered(const uint8_t bssid[6])
{
    /* Bit 1 of the first octet. Set by software access points - hostapd on a
     * laptop, a phone hotspot - and almost never by shipped infrastructure. */
    return (bssid[0] & 0x02) != 0;
}

static int8_t median_rssi(const pc_ap_t *aps, unsigned n)
{
    int8_t v[PT_MAX_GROUP];
    for (unsigned i = 0; i < n; i++) {
        v[i] = aps[i].rssi;
    }
    for (unsigned i = 1; i < n; i++) { /* insertion sort; n <= 16 */
        const int8_t k = v[i];
        unsigned j = i;
        while (j > 0 && v[j - 1] > k) { v[j] = v[j - 1]; j--; }
        v[j] = k;
    }
    return v[n / 2];
}

void pt_evaluate(const pc_ap_t *aps, unsigned n, const pt_profile_t *profile,
                 const pt_context_t *ctx, pt_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!aps || !ctx || n == 0) {
        return;
    }
    if (n > PT_MAX_GROUP) {
        n = PT_MAX_GROUP;
    }

    out->members = (uint8_t)n;
    out->ceiling = pt_ceiling(ctx, profile);
    if (!profile || !profile->loaded) {
        out->notes |= PT_NOTE_NO_PROFILE;
    }

    if (n == 1) {
        /* One access point cannot impersonate itself. */
        out->notes |= PT_NOTE_SINGLE;
        out->band = PT_BAND_CONSISTENT;
        return;
    }

    /* ---- grade every member ------------------------------------------ */
    pc_verdict_t grades[PT_MAX_GROUP];
    unsigned graded = 0;
    uint8_t best = 0, worst = 100;
    bool any_open = false, any_protected = false;
    bool any_mfp = false, any_no_mfp = false;
    bool any_sae = false, any_no_sae = false;

    for (unsigned i = 0; i < n; i++) {
        pc_grade(&aps[i], &grades[i]);
        if (grades[i].grade == PC_GRADE_UNGRADED) {
            out->notes |= PT_NOTE_THIN;
            continue;
        }
        graded++;
        if (grades[i].score > best) best = grades[i].score;
        if (grades[i].score < worst) worst = grades[i].score;

        if (grades[i].caps_applied & (PC_CAP_OPEN | PC_CAP_WEP)) {
            any_open = true;
            out->notes |= PT_NOTE_OPEN_MEMBER;
        } else {
            any_protected = true;
        }
        if (aps[i].rsn.mfp_capable || aps[i].rsn.mfp_required) any_mfp = true; else any_no_mfp = true;
        if (aps[i].rsn.has_sae) any_sae = true; else any_no_sae = true;
    }
    if (graded < 2) {
        /* Not enough graded members to compare. Say so rather than scoring. */
        out->notes |= PT_NOTE_THIN;
        out->band = PT_BAND_CONSISTENT;
        return;
    }
    out->best_grade_score = best;
    out->worst_grade_score = worst;

    /* ---- posture: does one member ask for less than its siblings? ----- */
    uint32_t posture = 0;
    if (any_open && any_protected) {
        /* The classic. An unprotected member of a protected SSID is not a
         * configuration nuance; it is the shape of the attack. */
        posture = 40;
    } else {
        const uint32_t gap = (uint32_t)(best - worst);
        posture = (gap * 34u) / 40u;
        if (any_mfp && any_no_mfp) posture += 12; /* one sibling can be deauthed */
        if (any_sae && any_no_sae) posture += 12; /* one sibling accepts WPA2   */
    }
    out->c_posture = (uint8_t)clamp_u32(posture, 0, 40);

    /* ---- identity: does one member wear an address it should not? ----- */
    /* Majority OUI. Multiplicity itself is ignored entirely - a roaming
     * deployment has many BSSIDs and that is not evidence of anything. */
    unsigned best_oui_count = 0;
    uint8_t majority_oui[3] = { 0, 0, 0 };
    for (unsigned i = 0; i < n; i++) {
        unsigned c = 0;
        for (unsigned j = 0; j < n; j++) {
            if (memcmp(aps[i].bssid, aps[j].bssid, 3) == 0) c++;
        }
        if (c > best_oui_count) {
            best_oui_count = c;
            memcpy(majority_oui, aps[i].bssid, 3);
        }
    }

    unsigned suspect = 0;
    uint32_t suspect_weight = 0;

    for (unsigned i = 0; i < n; i++) {
        uint32_t w = 0;
        if (memcmp(aps[i].bssid, majority_oui, 3) != 0 && best_oui_count > 1) {
            w += 16; /* a different vendor carrying the same name */
        }
        if (is_locally_administered(aps[i].bssid)) {
            w += 18;
            out->notes |= PT_NOTE_LOCAL_MAC;
        }
        if (profile && profile->loaded && !pt_profile_contains(profile, aps[i].bssid)) {
            w += 14; /* the baseline says this radio should not be here */
        }
        if (w > suspect_weight) {
            suspect_weight = w;
            suspect = i;
        }
    }
    /* With nothing suspicious about any address, the natural suspect is the
     * weakest member - the one an attacker would be impersonating towards.
     * Choosing a suspect first and then measuring its behaviour matters: on
     * a group where every member sits on its own channel, "alone on this
     * channel" is true of everybody and therefore evidence about nobody. */
    if (suspect_weight == 0) {
        uint8_t weakest = 255;
        for (unsigned i = 0; i < n; i++) {
            if (grades[i].grade == PC_GRADE_UNGRADED) continue;
            if (grades[i].score < weakest) {
                weakest = grades[i].score;
                suspect = i;
            }
        }
    }
    out->c_identity = (uint8_t)clamp_u32(suspect_weight, 0, 30);

    /* ---- behaviour: where the suspect sits, and how loud it is --------- */
    const int8_t med = median_rssi(aps, n);
    uint32_t behaviour = 0;

    /* Alone on a channel none of its siblings use. */
    if (n > 2) {
        unsigned same_channel = 0;
        for (unsigned j = 0; j < n; j++) {
            if (aps[j].channel == aps[suspect].channel) same_channel++;
        }
        if (same_channel == 1) {
            behaviour += 12;
        }
    }

    /* Signal excess: an evil twin is usually in the room with you, while the
     * real infrastructure is in a ceiling two walls away. */
    const int rssi_excess = (int)aps[suspect].rssi - (int)med;
    out->rssi_excess = (int8_t)clamp_u32((uint32_t)(rssi_excess > 0 ? rssi_excess : 0), 0, 127);
    if (rssi_excess >= 18) {
        behaviour += clamp_u32(12u + (uint32_t)(rssi_excess - 18) / 2u, 12, 18);
    }

    /* Beacon interval divergence: infrastructure from one vendor is set the
     * same everywhere; a laptop's soft AP usually is not. */
    if (aps[suspect].beacon_ms) {
        unsigned differing = 0;
        for (unsigned i = 0; i < n; i++) {
            if (aps[i].beacon_ms && aps[i].beacon_ms != aps[suspect].beacon_ms) {
                differing++;
            }
        }
        if (differing == n - 1) {
            behaviour += 8;
        }
    }
    out->c_behaviour = (uint8_t)clamp_u32(behaviour, 0, 30);

    out->suspect_index = (uint8_t)suspect;
    memcpy(out->suspect, aps[suspect].bssid, 6);

    /* ---- families and caps --------------------------------------------- */
    if (out->c_posture >= 14) out->families |= PT_FAM_POSTURE;
    if (out->c_identity >= 14) out->families |= PT_FAM_IDENTITY;
    if (out->c_behaviour >= 12) out->families |= PT_FAM_BEHAVIOUR;

    uint32_t raw = (uint32_t)out->c_posture + out->c_identity + out->c_behaviour;
    out->raw_score = (uint8_t)clamp_u32(raw, 0, 100);
    uint32_t score = raw;

    unsigned family_count = 0;
    for (unsigned b = 0; b < 3; b++) {
        if (out->families & (1u << b)) family_count++;
    }

    /* The roaming guard. Same vendor, same posture, nobody using a software
     * address: this is one deployment with many radios, and no number of
     * BSSIDs changes that. */
    if (family_count == 0) {
        out->notes |= PT_NOTE_ROAMING;
        if (score > 19) score = 19;
    }
    if (family_count < 2 && score > 44) {
        score = 44;
    }
    /* An alarm requires the posture family specifically. A different vendor
     * on an odd channel is a different vendor on an odd channel; it becomes
     * impersonation when it also asks for less than its siblings. */
    if (!(out->families & PT_FAM_POSTURE) && score > 59) {
        score = 59;
    }
    if ((out->notes & PT_NOTE_THIN) && score > 59) {
        score = 59;
    }
    if (score > out->ceiling) {
        score = out->ceiling;
    }

    out->score = (uint8_t)score;
    out->band = band_of(out->score);
}

const char *pt_band_name(pt_band_t band)
{
    switch (band) {
    case PT_BAND_CONSISTENT:  return "CONSISTENT";
    case PT_BAND_MIXED:       return "MIXED ESTATE";
    case PT_BAND_ANOMALOUS:   return "ANOMALOUS";
    case PT_BAND_TWIN_LIKELY: return "TWIN LIKELY";
    default:                  return "?";
    }
}

const char *pt_band_advice(pt_band_t band)
{
    switch (band) {
    case PT_BAND_CONSISTENT:
        return "Every radio carrying this name looks like part of the same "
               "deployment. Many BSSIDs on one SSID is roaming, not an attack.";
    case PT_BAND_MIXED:
        return "The radios carrying this name are not identical. Usually a "
               "mixed estate or a migration in progress - worth confirming.";
    case PT_BAND_ANOMALOUS:
        return "One radio does not belong with the others. It is not asking "
               "for less than they do, so this is odd rather than hostile.";
    case PT_BAND_TWIN_LIKELY:
        return "One radio carries this name while asking for materially less "
               "security than its siblings. Do not join it. Preserve the log.";
    default:
        return "";
    }
}
