#include "pharos_opsec.h"

#include <string.h>

static po_grade_t grade_of(uint8_t camped_score, unsigned families)
{
    if (camped_score >= 75 && families >= 3) return PO_GRADE_BLARING;
    if (camped_score >= 75)                  return PO_GRADE_LOUD;
    if (camped_score >= 40)                  return PO_GRADE_FAINT;
    return PO_GRADE_GHOST;
}

/* The dominant tell is the family that contributed the most points to the
 * camped verdict. Ties resolve toward the tell a red-teamer can most readily
 * do something about: identity, then targeting, then rate. */
static po_tell_t dominant_tell(const pw_verdict_t *v)
{
    uint8_t best = 0;
    po_tell_t tell = PO_TELL_NONE;
    if (v->c_rate > best)     { best = v->c_rate;     tell = PO_TELL_RATE; }
    if (v->c_target >= best)  { best = v->c_target;   tell = PO_TELL_TARGET; }
    if (v->c_identity >= best){ best = v->c_identity; tell = PO_TELL_IDENTITY; }
    if (v->c_reason > best)   { best = v->c_reason;   tell = PO_TELL_REASON; }
    return best ? tell : PO_TELL_NONE;
}

static const char *guidance_for(po_tell_t tell, const pw_verdict_t *v)
{
    switch (tell) {
    case PO_TELL_TARGET:
        return "Broadcast targeting is your loudest tell - it lights the "
               "targeting family instantly. A defender reads ff:ff:ff:ff:ff:ff "
               "as an attack; real disconnects are unicast.";
    case PO_TELL_IDENTITY:
        return "You are heard at a different signal level than the AP you "
               "claim to be. To a camped receiver, that address mismatch is "
               "unforgeable evidence of a second transmitter.";
    case PO_TELL_RATE:
        return "Raw volume carries the score. Rate alone caps below the alarm "
               "band, but it is what a defender notices first and what widens "
               "the ingest drop-count they will cite.";
    case PO_TELL_REASON:
        return "Every frame carries the same reason code - a monoculture no "
               "real network produces. It is a fingerprint of the tool, not "
               "of a disconnect.";
    case PO_TELL_NONE:
    default:
        return "No single family dominates; the signature is diffuse. That is "
               "the quietest an attack of this kind gets on this receiver.";
    }
}

void po_assess(const pw_verdict_t *camped, const pw_verdict_t *hopping,
               po_report_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "";
    out->tell_name = "";
    out->guidance = "";
    out->dominant_tell = PO_TELL_NONE;

    if (!camped) {
        out->headline = "No camped verdict - cannot judge worst-case visibility";
        return;
    }

    unsigned families = 0;
    for (unsigned b = 0; b < 3; b++) {
        if (camped->families & (1u << b)) families++;
    }
    out->families_lit = (uint8_t)families;
    out->camped_score = camped->score;
    out->hopping_score = hopping ? hopping->score : 0;
    out->grade = grade_of(camped->score, families);

    out->dominant_tell = dominant_tell(camped);
    out->tell_name = po_tell_name(out->dominant_tell);
    out->guidance = guidance_for(out->dominant_tell, camped);

    if (hopping) {
        out->stealth_gap = (camped->score > hopping->score)
                               ? (uint8_t)(camped->score - hopping->score)
                               : 0;
        /* The operationally interesting line: loud to somebody standing
         * still, missed by somebody who keeps moving. "Missed" means the
         * hopping view does not reach the alarm band (75) - which, because a
         * hopping receiver's ceiling sits around 60, is the normal case for an
         * attack that only a camped defender can pin down. */
        out->invisible_to_hoppers =
            (camped->score >= 75) && (hopping->score < 75);
    }

    switch (out->grade) {
    case PO_GRADE_BLARING:
        out->headline = "Blaring - a camped defender alarms and every family agrees";
        break;
    case PO_GRADE_LOUD:
        out->headline = out->invisible_to_hoppers
                            ? "Loud when watched, but a hopping defender would miss it"
                            : "Loud - a camped defender alarms on this";
        break;
    case PO_GRADE_FAINT:
        out->headline = "Faint - visible to a camped defender but below the alarm";
        break;
    case PO_GRADE_GHOST:
    default:
        out->headline = "Ghost - even a camped defender barely registers this";
        break;
    }
}

const char *po_grade_name(po_grade_t g)
{
    switch (g) {
    case PO_GRADE_GHOST:   return "GHOST";
    case PO_GRADE_FAINT:   return "FAINT";
    case PO_GRADE_LOUD:    return "LOUD";
    case PO_GRADE_BLARING: return "BLARING";
    default:               return "?";
    }
}

const char *po_tell_name(po_tell_t t)
{
    switch (t) {
    case PO_TELL_RATE:     return "frame rate";
    case PO_TELL_TARGET:   return "broadcast targeting";
    case PO_TELL_IDENTITY: return "spoofed identity";
    case PO_TELL_REASON:   return "reason-code monoculture";
    case PO_TELL_NONE:
    default:               return "no dominant tell";
    }
}
