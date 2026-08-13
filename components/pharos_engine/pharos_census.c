#include "pharos_census.h"

#include <string.h>

static uint8_t clamp8(int v, int lo, int hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (uint8_t)v;
}

static pc_grade_t grade_of(uint8_t score)
{
    if (score >= 95) return PC_GRADE_A_PLUS;
    if (score >= 88) return PC_GRADE_A;
    if (score >= 78) return PC_GRADE_B;
    if (score >= 68) return PC_GRADE_C;
    if (score >= 58) return PC_GRADE_D;
    if (score >= 48) return PC_GRADE_E;
    return PC_GRADE_F;
}

void pc_grade(const pc_ap_t *ap, pc_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "";
    if (!ap) {
        return;
    }

    /* Honesty gate. One beacon is a glimpse: element chains get truncated,
     * a transition-mode network may have been heard on its weaker half, and
     * a hidden network says almost nothing on its first frame. Below three
     * beacons the engine declines to grade rather than guessing. */
    if (ap->beacons < 3) {
        out->notes |= PC_NOTE_THIN;
        out->grade = PC_GRADE_UNGRADED;
        out->headline = "Heard too briefly to grade - stay in range";
        return;
    }

    if (ap->hidden) {
        out->notes |= PC_NOTE_HIDDEN;
    }

    const bool has_rsn = ap->rsn.has_rsn;
    const bool open = !ap->privacy && !has_rsn && !ap->wpa1_ie;
    const bool wep = ap->privacy && !has_rsn && !ap->wpa1_ie;
    const bool wpa1_only = ap->wpa1_ie && !has_rsn;

    /* ---- authentication: what it takes to get on (45) ---------------- */
    int auth = 0;
    if (open) {
        auth = 0;
        out->caps_applied |= PC_CAP_OPEN;
        out->headline = "Open network - no key, no protection, anyone within range";
    } else if (wep) {
        auth = 2;
        out->caps_applied |= PC_CAP_WEP;
        out->headline = "WEP - broken since 2001 and crackable in minutes";
    } else if (wpa1_only) {
        auth = 8;
        out->caps_applied |= PC_CAP_WPA1;
        out->headline = "Original WPA only - superseded twice over";
    } else if (ap->rsn.has_owe) {
        auth = 18;
        out->notes |= PC_NOTE_OWE;
        out->headline = "Encrypted but unauthenticated - anyone may still join";
    } else if (ap->akm_8021x) {
        auth = 38;
        out->notes |= PC_NOTE_ENTERPRISE;
        out->headline = "Enterprise authentication - per-user credentials";
    } else if (ap->rsn.has_sae && ap->rsn.has_psk) {
        auth = 36;
        out->notes |= PC_NOTE_TRANSITION;
        out->headline = "WPA3 transition mode - a client can still be pushed to WPA2";
    } else if (ap->rsn.has_sae) {
        auth = 45;
        out->headline = "WPA3-Personal - offline password guessing does not work here";
    } else if (ap->rsn.has_psk) {
        auth = 28;
        out->headline = "WPA2-Personal - the handshake can be captured and attacked offline";
    } else {
        /* RSN present but no AKM we recognise. Do not invent a grade. */
        out->notes |= PC_NOTE_THIN;
        out->grade = PC_GRADE_UNGRADED;
        out->headline = "Unrecognised authentication - not graded";
        return;
    }
    out->c_auth = clamp8(auth, 0, 45);

    /* ---- management frame protection (25) ---------------------------- */
    int mfp = 0;
    if (ap->rsn.mfp_required) {
        mfp = 25;
    } else if (ap->rsn.mfp_capable) {
        mfp = 12;
    } else {
        mfp = 0;
        if (!open && !wep) {
            /* The tie to pharos_watch: without 802.11w, the deauthentication
             * flood the Watch lens grades will actually disconnect clients
             * here. That is a hard limit on how good this network can be. */
            out->caps_applied |= PC_CAP_NO_MFP;
        }
    }
    out->c_mfp = clamp8(mfp, 0, 25);

    /* ---- cipher (15) -------------------------------------------------- */
    int cipher = 0;
    if (ap->ccmp_pairwise && ap->tkip_pairwise) {
        cipher = 6;
        out->caps_applied |= PC_CAP_TKIP;
    } else if (ap->ccmp_pairwise) {
        cipher = 15;
    } else if (ap->tkip_pairwise) {
        cipher = 0;
        out->caps_applied |= PC_CAP_TKIP;
    } else if (open) {
        cipher = 0;
    }
    out->c_cipher = clamp8(cipher, 0, 15);

    /* ---- exposure (15) ------------------------------------------------ */
    int exposure = 15;
    if (ap->wps_pin) {
        exposure = 0;
        out->caps_applied |= PC_CAP_WPS_PIN;
    } else if (ap->wps_present) {
        exposure = 8;
    }
    out->c_exposure = clamp8(exposure, 0, 15);

    int score = out->c_auth + out->c_mfp + out->c_cipher + out->c_exposure;
    out->raw_score = clamp8(score, 0, 100);

    /* ---- ceilings ------------------------------------------------------
     * Some weaknesses are not gradual. A network with no authentication is
     * not a C with points off; the first person to walk past is inside. */
    if (out->caps_applied & (PC_CAP_OPEN | PC_CAP_WEP)) {
        if (score > 20) score = 20;
    }
    if (out->caps_applied & PC_CAP_WPA1) {
        if (score > 47) score = 47; /* top of F */
    }
    if (out->caps_applied & PC_CAP_TKIP) {
        if (score > 57) score = 57; /* top of E */
    }
    if (out->caps_applied & PC_CAP_WPS_PIN) {
        if (score > 67) score = 67; /* top of D: a known eight-digit bypass */
    }
    if (out->caps_applied & PC_CAP_NO_MFP) {
        if (score > 77) score = 77; /* top of C: deauth floods work here */
    }

    out->score = clamp8(score, 0, 100);
    out->grade = grade_of(out->score);

    if (out->grade == PC_GRADE_A_PLUS) {
        out->headline = "WPA3 with protected management frames - the attacks this "
                        "device can see would not land";
    }
}

const char *pc_grade_name(pc_grade_t g)
{
    switch (g) {
    case PC_GRADE_A_PLUS:  return "A+";
    case PC_GRADE_A:       return "A";
    case PC_GRADE_B:       return "B";
    case PC_GRADE_C:       return "C";
    case PC_GRADE_D:       return "D";
    case PC_GRADE_E:       return "E";
    case PC_GRADE_F:       return "F";
    case PC_GRADE_UNGRADED:
    default:               return "--";
    }
}

const char *pc_grade_advice(const pc_verdict_t *v)
{
    if (!v) {
        return "";
    }
    if (v->grade == PC_GRADE_UNGRADED) {
        return "Stay in range long enough to hear three beacons.";
    }
    /* Report the ceiling that actually bound the score, worst first: one
     * thing to fix beats a list nobody works through. */
    if (v->caps_applied & PC_CAP_OPEN) {
        return "Anything sent here is readable by anyone in range. Add WPA3, "
               "or WPA2 if the clients cannot manage it.";
    }
    if (v->caps_applied & PC_CAP_WEP) {
        return "WEP is not a weak key, it is a broken design. Replace it today.";
    }
    if (v->caps_applied & PC_CAP_WPA1) {
        return "Original WPA. Move to WPA2-CCMP at minimum, WPA3 if the "
               "hardware allows.";
    }
    if (v->caps_applied & PC_CAP_TKIP) {
        return "TKIP is offered. Disable it - mixed mode drags the whole "
               "network down to its weakest cipher.";
    }
    if (v->caps_applied & PC_CAP_WPS_PIN) {
        return "WPS PIN is advertised. It is an eight-digit bypass of whatever "
               "passphrase you chose. Turn it off.";
    }
    if (v->caps_applied & PC_CAP_NO_MFP) {
        return "No 802.11w. Clients here can be disconnected by anyone with a "
               "radio. Enable protected management frames.";
    }
    if (v->notes & PC_NOTE_TRANSITION) {
        return "WPA3 transition mode still accepts WPA2. Once every client "
               "supports SAE, turn the transition off.";
    }
    return "Nothing this device can see would improve the posture further. "
           "That is not the same as no exposure.";
}

int pc_compare(const pc_ap_t *a, const pc_verdict_t *va,
               const pc_ap_t *b, const pc_verdict_t *vb)
{
    if (!a || !b || !va || !vb) {
        return 0;
    }
    /* Ungraded networks sort last: they are not "good", they are unknown,
     * and putting them at the top would bury the real findings. */
    const bool a_un = va->grade == PC_GRADE_UNGRADED;
    const bool b_un = vb->grade == PC_GRADE_UNGRADED;
    if (a_un != b_un) {
        return a_un ? 1 : -1;
    }
    if (va->grade != vb->grade) {
        return (int)va->grade - (int)vb->grade; /* worst grade first */
    }
    if (a->rssi != b->rssi) {
        return (int)b->rssi - (int)a->rssi; /* strongest first */
    }
    return memcmp(a->bssid, b->bssid, 6);
}
