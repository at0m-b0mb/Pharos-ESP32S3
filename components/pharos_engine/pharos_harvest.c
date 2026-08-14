#include "pharos_harvest.h"

#include <string.h>

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static bool mac_broadcast(const uint8_t *a)
{
    for (int i = 0; i < 6; i++) {
        if (a[i] != 0xFF) return false;
    }
    return true;
}

void ph_reset(ph_state_t *s)
{
    if (s) {
        memset(s, 0, sizeof(*s));
    }
}

static ph_pair_t *pair_get(ph_state_t *s, const uint8_t *bssid, const uint8_t *client)
{
    for (unsigned i = 0; i < s->n; i++) {
        if (s->pairs[i].in_use && mac_eq(s->pairs[i].bssid, bssid) &&
            mac_eq(s->pairs[i].client, client)) {
            return &s->pairs[i];
        }
    }
    if (s->n >= PH_MAX_PAIRS) {
        s->full = true;
        return NULL;
    }
    ph_pair_t *p = &s->pairs[s->n++];
    memset(p, 0, sizeof(*p));
    memcpy(p->bssid, bssid, 6);
    memcpy(p->client, client, 6);
    p->in_use = true;
    return p;
}

void ph_observe(ph_state_t *s, const pharos_ev_dot11_t *f, uint64_t t_us)
{
    if (!s || !f) {
        return;
    }
    if (f->flags & PHAROS_DOT11_F_MFP_SEEN) {
        s->mfp_seen = true;
    }

    if (f->type == PHAROS_FT_MGMT &&
        (f->subtype == PHAROS_ST_DEAUTH || f->subtype == PHAROS_ST_DISASSOC)) {
        s->deauths++;
        /* A broadcast disconnect hits every client at once. It cannot be
         * attributed to one conversation, so it arms every one we know of -
         * which is exactly what it does on the air. */
        if (mac_broadcast(f->a1)) {
            for (unsigned i = 0; i < s->n; i++) {
                if (s->pairs[i].in_use && mac_eq(s->pairs[i].bssid, f->a3)) {
                    s->pairs[i].deauths++;
                    s->pairs[i].last_deauth_us = t_us;
                    s->pairs[i].deauth_armed = true;
                }
            }
            return;
        }
        /* Directed: the victim is whichever side is not the access point. */
        const uint8_t *client = mac_eq(f->a1, f->a3) ? f->a2 : f->a1;
        ph_pair_t *p = pair_get(s, f->a3, client);
        if (p) {
            p->deauths++;
            p->last_deauth_us = t_us;
            p->deauth_armed = true;
        }
        return;
    }

    if (f->type != PHAROS_FT_DATA || f->eapol == 0) {
        return;
    }

    /* Messages 1 and 3 travel from the access point; 2 and 4 from the client.
     * That is how the pair is identified without needing to know either role
     * in advance. */
    const bool from_ap = (f->eapol == 1 || f->eapol == 3);
    const uint8_t *bssid = from_ap ? f->a2 : f->a1;
    const uint8_t *client = from_ap ? f->a1 : f->a2;
    ph_pair_t *p = pair_get(s, bssid, client);
    if (!p) {
        return;
    }

    if (f->eapol == 1) {
        p->m1++;
        s->handshakes++;
        p->last_m1_us = t_us;
        if (f->flags & PHAROS_DOT11_F_PMKID) {
            p->pmkid_req++;
            /* Held open until a message 2 answers it, or the sweep settles.
             * An unanswered PMKID request is the clientless attack; a normal
             * client always follows through. */
            p->m1_pending_pmkid = true;
        }
        /* Did a disconnect just precede this handshake? That ordering, tightly
         * spaced, is the forced cycle. */
        if (p->deauth_armed && t_us >= p->last_deauth_us &&
            (t_us - p->last_deauth_us) <= PH_FORCE_WINDOW_US) {
            p->forced++;
            /* Consume it: one disconnect explains one handshake, not a run. */
            p->deauth_armed = false;
        }
    } else if (f->eapol == 2) {
        p->m2++;
        if (p->m1_pending_pmkid) {
            p->m1_pending_pmkid = false; /* completed: an ordinary client */
        }
    }
}

void ph_settle(ph_state_t *s)
{
    if (!s) {
        return;
    }
    for (unsigned i = 0; i < s->n; i++) {
        if (s->pairs[i].in_use && s->pairs[i].m1_pending_pmkid) {
            s->pairs[i].pmkid_orphan++;
            s->pairs[i].m1_pending_pmkid = false;
        }
    }
}

uint8_t ph_ceiling(const ph_context_t *ctx)
{
    if (!ctx) {
        return 58;
    }
    const uint32_t dwell = clamp_u32(ctx->dwell_permil, 1, 1000);
    uint32_t c = 58u + (38u * dwell) / 1000u;

    /* A handshake is two frames, seconds apart at most. Losing frames to a
     * full ring costs this engine more than it costs a rate-based one, because
     * the evidence is an ORDERING and a gap breaks it. */
    const uint32_t yield = clamp_u32(ctx->yield_permil ? ctx->yield_permil : 1000, 1, 1000);
    if (yield < 900) {
        c -= clamp_u32((900u - yield) / 40u, 0, 14);
    }
    return (uint8_t)clamp_u32(c, 40u, 96u);
}

static ph_band_t band_of(uint8_t score)
{
    if (score >= 70) return PH_BAND_HARVEST_LIKELY;
    if (score >= 45) return PH_BAND_SUSPECTED;
    if (score >= 20) return PH_BAND_HANDSHAKES;
    return PH_BAND_QUIET;
}

static unsigned popcount8(uint8_t v)
{
    unsigned n = 0;
    while (v) { n += (v & 1u); v >>= 1; }
    return n;
}

void ph_evaluate(const ph_state_t *s, const ph_context_t *ctx, ph_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "";
    if (!s || !ctx) {
        return;
    }
    out->ceiling = ph_ceiling(ctx);
    out->handshakes = s->handshakes;

    const uint32_t dwell = clamp_u32(ctx->dwell_permil, 1, 1000);
    if (dwell < 500) {
        out->notes |= PH_NOTE_THIN_SWEEP;
    }
    if (ctx->yield_permil && ctx->yield_permil < 900) {
        out->notes |= PH_NOTE_DROPS;
    }
    if (s->full) {
        out->notes |= PH_NOTE_FULL;
    }
    if (s->handshakes) {
        /* Say plainly which half of the handshake is even visible. */
        out->notes |= PH_NOTE_PROTECTED;
    }
    const bool mfp = ctx->mfp_required || s->mfp_seen;
    if (mfp) {
        out->notes |= PH_NOTE_MFP;
    }

    uint32_t worst_forced = 0;
    for (unsigned i = 0; i < s->n; i++) {
        const ph_pair_t *p = &s->pairs[i];
        if (!p->in_use) {
            continue;
        }
        out->forced_cycles += p->forced;
        out->pmkid_orphans += p->pmkid_orphan;
        if (p->forced) {
            out->victims++;
        }
        if (p->forced > worst_forced) {
            worst_forced = p->forced;
            memcpy(out->worst_client, p->client, 6);
            memcpy(out->worst_bssid, p->bssid, 6);
        }
    }

    uint32_t score = 0;

    /* --- family: forced cycles ---------------------------------------- */
    if (out->forced_cycles >= 2) {
        out->families |= PH_FAM_FORCED;
        score += 30 + clamp_u32((out->forced_cycles - 2u) * 6u, 0, 24);
    } else if (out->forced_cycles == 1) {
        /* Explicitly NOT a family. One disconnect followed by one reconnect is
         * what a rebooting access point looks like. */
        score += 12;
    }

    /* --- family: unanswered PMKID solicitation ------------------------- */
    if (out->pmkid_orphans >= 1) {
        out->families |= PH_FAM_PMKID;
        /* This one is strong on its own evidence: ordinary clients finish
         * their handshakes, so a request that is never completed is a
         * deliberate act rather than a flaky radio. */
        score += 46 + clamp_u32((out->pmkid_orphans - 1u) * 8u, 0, 16);
    }

    /* --- family: the same victim, again and again ---------------------- */
    if (worst_forced >= 4) {
        out->families |= PH_FAM_REPEAT;
        score += 24;
    }

    /* --- family: breadth across victims -------------------------------- */
    if (out->victims >= 3) {
        out->families |= PH_FAM_BREADTH;
        score += 16;
    }

    /* Plain handshake traffic, with none of the above, is worth saying out
     * loud but is not suspicion. */
    if (score == 0 && s->handshakes) {
        score = 20 + clamp_u32(s->handshakes * 2u, 0, 12);
    }

    /* --- honesty caps -------------------------------------------------- */

    /* Where 802.11w is in force, a forged disconnect is discarded by the
     * client - so a handshake that follows one is far more likely to be a
     * coincidence than a consequence. Discount the forced evidence hard, and
     * never let it alone carry the alarm. */
    if (mfp && (out->families & PH_FAM_FORCED)) {
        score = (score * 65u) / 100u;
    }

    /* One family, however loud, cannot reach HARVEST LIKELY. Collection is an
     * argument built from an ordering plus a repetition; a single signal is a
     * coincidence waiting to be explained. */
    const unsigned fams = popcount8(out->families);
    if (fams <= 1 && score > 62) {
        score = 62;
    }
    if (fams == 0 && score > 44) {
        score = 44;
    }

    /* A thin sweep cannot see an ordering reliably: the deauthentication may
     * have landed on a channel we had already left. */
    if ((out->notes & PH_NOTE_THIN_SWEEP) && score > 74) {
        score = 74;
    }

    score = clamp_u32(score, 0, 100);
    out->raw_score = (uint8_t)score;
    if (score > out->ceiling) {
        score = out->ceiling;
    }
    out->score = (uint8_t)score;
    out->band = band_of(out->score);

    switch (out->band) {
    case PH_BAND_HARVEST_LIKELY:
        out->headline = (out->families & PH_FAM_PMKID)
                            ? "Somebody is soliciting PMKIDs and never finishing"
                            : "Clients are being knocked off and their handshakes taken";
        break;
    case PH_BAND_SUSPECTED:
        out->headline = (out->families & PH_FAM_PMKID)
                            ? "A PMKID was requested and never completed"
                            : "A handshake was captured in a way that looks deliberate";
        break;
    case PH_BAND_HANDSHAKES:
        out->headline = "Handshakes seen - normal traffic, nothing forcing them";
        break;
    case PH_BAND_QUIET:
    default:
        out->headline = "No handshake collection in what this sweep heard";
        break;
    }
}

const char *ph_band_name(ph_band_t b)
{
    switch (b) {
    case PH_BAND_QUIET:           return "QUIET";
    case PH_BAND_HANDSHAKES:      return "HANDSHAKES";
    case PH_BAND_SUSPECTED:       return "SUSPECTED";
    case PH_BAND_HARVEST_LIKELY:  return "HARVEST LIKELY";
    default:                      return "?";
    }
}

const char *ph_band_advice(ph_band_t b)
{
    switch (b) {
    case PH_BAND_QUIET:
        return "Nothing in this sweep looks like handshake collection. This "
               "receiver hears one channel at a time and a capture lasts only "
               "seconds, so that is an absence of evidence rather than proof.";
    case PH_BAND_HANDSHAKES:
        return "Handshakes were seen, which is what every device does when it "
               "joins a network. Nothing appears to be forcing them.";
    case PH_BAND_SUSPECTED:
        return "A handshake followed a disconnection closely, or a PMKID was "
               "requested and never completed. One instance has innocent "
               "explanations - a rebooting access point, a roaming client. "
               "Watch this channel and see whether it repeats.";
    case PH_BAND_HARVEST_LIKELY:
        return "The pattern repeats or spans several devices: disconnect, "
               "reconnect, capture. Treat the passphrase as exposed and plan "
               "to rotate it. Enable 802.11w so forged disconnects stop "
               "working, and prefer WPA3-SAE, where a captured handshake does "
               "not yield to offline guessing.";
    default:
        return "";
    }
}

const char *ph_family_name(unsigned family_bit)
{
    switch (family_bit) {
    case PH_FAM_FORCED:  return "forced";
    case PH_FAM_PMKID:   return "pmkid";
    case PH_FAM_REPEAT:  return "repeat";
    case PH_FAM_BREADTH: return "breadth";
    default:             return "-";
    }
}
