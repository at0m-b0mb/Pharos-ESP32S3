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

static void heard_add(ph_state_t *s, const uint8_t mac[6]);

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

    /* Any frame transmitted BY a radio proves we can hear it - which is what
     * licenses us to treat its silence as meaningful later. Recorded before
     * the EAPOL filter, because most of a client's traffic is not EAPOL. */
    heard_add(s, f->a2);

    /* THE APPROACH. An association request is a management frame: unencrypted,
     * always delivered, and impossible for a PMKID attack to skip. */
    if (f->type == PHAROS_FT_MGMT &&
        (f->subtype == PHAROS_ST_ASSOC_REQ || f->subtype == PHAROS_ST_REASSOC_REQ)) {
        ph_pair_t *ap = pair_get(s, f->a1, f->a2); /* a1 = AP, a2 = client */
        if (ap) {
            ap->assoc_req++;
            ap->data_since_assoc = false;
        }
        return;
    }

    /* Data FROM a client that has associated is the network being used, which
     * is exactly what a harvester never does. */
    if (f->type == PHAROS_FT_DATA) {
        for (unsigned i = 0; i < s->n; i++) {
            ph_pair_t *q = &s->pairs[i];
            if (q->in_use && q->assoc_req && memcmp(q->client, f->a2, 6) == 0) {
                q->data_since_assoc = true;
                break;
            }
        }
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
        heard_add(s, f->a2); /* message 2 is itself proof we can hear them */
        if (p->m1_pending_pmkid) {
            p->m1_pending_pmkid = false; /* completed: an ordinary client */
        }
    }
}

static void heard_add(ph_state_t *s, const uint8_t mac[6])
{
    for (unsigned i = 0; i < s->heard_n; i++) {
        if (memcmp(s->heard[i], mac, 6) == 0) {
            return;
        }
    }
    if (s->heard_n < PH_MAX_HEARD) {
        memcpy(s->heard[s->heard_n++], mac, 6);
    }
}

static bool heard_has(const ph_state_t *s, const uint8_t mac[6])
{
    for (unsigned i = 0; i < s->heard_n; i++) {
        if (memcmp(s->heard[i], mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

void ph_settle(ph_state_t *s)
{
    if (!s) {
        return;
    }
    for (unsigned i = 0; i < s->n; i++) {
        ph_pair_t *q = &s->pairs[i];
        if (!q->in_use || !q->m1_pending_pmkid) {
            continue;
        }
        q->m1_pending_pmkid = false;
        /* Never heard this client transmit at all? Then we have no standing
         * to call its message 2 missing - see the note on `heard`. */
        if (heard_has(s, q->client)) {
            q->pmkid_orphan++;
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
        out->assoc_reqs += p->assoc_req;
        if (p->assoc_req && !p->data_since_assoc) {
            out->touch_and_go++;
        }
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
    /* ONE IS NOT A FAMILY, AND THE HEADER ALREADY PROMISED THAT.
     *
     * "The engine requires REPETITION or BREADTH before it will use the word
     * harvest" - but this branch fired a full family, and 46 points, on a
     * single unanswered request. PMKID in message 1 is ROUTINE: it is how PMK
     * caching and fast roaming work, so every modern client solicits one every
     * time it roams. One of them is a Tuesday, not an attack.
     *
     * A lone request is still worth a couple of points - it is the shape of
     * the clientless attack - but it may not light a family and it may not on
     * its own reach a band that says somebody is collecting your handshakes. */
    if (out->pmkid_orphans >= 2) {
        out->families |= PH_FAM_PMKID;
        /* 46 clears SUSPECTED at 45 - the weight the single orphan used to
         * carry, now requiring the repetition the header always promised. */
        score += 46 + clamp_u32((out->pmkid_orphans - 2u) * 8u, 0, 20);
    } else if (out->pmkid_orphans == 1) {
        /* Worth a couple of points - it is the right SHAPE - but nowhere near
         * a band that says somebody is collecting your handshakes. */
        score += 10;
    }

    /* --- family: associated, took what it came for, and left ----------- */
    /* ONE IS AMBIGUOUS AND ALWAYS WILL BE. A client with the wrong password
     * associates and leaves too, and so does one that wandered out of range
     * mid-connect. Two separate approaches that both went nowhere is a
     * pattern; one is a Tuesday, exactly as with the lone PMKID above. */
    if (out->touch_and_go >= 2) {
        out->families |= PH_FAM_TOUCH_GO;
        /* 46 clears SUSPECTED at 45, matching the PMKID family: two
         * approaches that both went nowhere is the same weight of
         * evidence as two unanswered solicitations. */
        score += 46 + clamp_u32((out->touch_and_go - 2u) * 10u, 0, 24);
    } else if (out->touch_and_go == 1) {
        score += 8;
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
                        : (out->families & PH_FAM_TOUCH_GO)
                            ? "Something keeps joining this network and never using it"
                            : "Clients are being knocked off and their handshakes taken";
        break;
    case PH_BAND_SUSPECTED:
        /* THE HEADLINE MUST NAME THE EVIDENCE THAT ACTUALLY FIRED.
         *
         * On hardware this said "A handshake was captured in a way that looks
         * deliberate" while `handshakes seen` was ZERO - the touch-and-go
         * family had raised the score and the words still described the
         * handshake family. A verdict that misreports its own reason is worse
         * than a quiet one: it sends the operator looking for the wrong
         * thing. */
        out->headline = (out->families & PH_FAM_PMKID)
                            ? "A PMKID was requested and never completed"
                        : (out->families & PH_FAM_TOUCH_GO)
                            ? "Something joined this network and never used it"
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
    case PH_FAM_TOUCH_GO: return "touch-and-go";
    case PH_FAM_REPEAT:  return "repeat";
    case PH_FAM_BREADTH: return "breadth";
    default:             return "-";
    }
}
