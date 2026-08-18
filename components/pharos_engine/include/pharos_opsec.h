/* Pharos - OPSEC footprint: how detectable is this attack?
 *
 * Pure C. This is the red team's mirror, and it is the honest way for a
 * receive-only tool to be a red-team tool.
 *
 * Pharos cannot run an attack. What it can do - and what a good red-teamer
 * actually needs - is show, from the defender's side of the glass, how loud a
 * given attack is: what score it earns, which single piece of evidence gives
 * it away, and crucially whether a defender who is merely *hopping* would ever
 * notice. Understanding your own signature is tradecraft. A red team that
 * knows a broadcast deauth lights up three families, but a hopping defender
 * caps at SUSPICIOUS, knows more about the engagement than one that just
 * fires and hopes.
 *
 * It works on the exact same verdicts the blue side sees, because it is the
 * same engine. Feed it a verdict graded twice - once against a camped
 * receiver, once against a hopping one - and it reports:
 *
 *   - a detectability grade (worst case for the attacker: the camped score),
 *   - the dominant tell (the family contributing most to the score),
 *   - the stealth gap (how much a hopping defender misses), and
 *   - concrete, defensive guidance on what made the attack loud.
 *
 * Nothing here helps anyone attack anything. It helps an authorised operator
 * understand what they look like to a watchtower - which is exactly what makes
 * them careful, and exactly what a blue team wants a red team to internalise.
 */
#ifndef PHAROS_OPSEC_H
#define PHAROS_OPSEC_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_watch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PO_GRADE_GHOST = 0,  /* a camped defender barely sees it      */
    PO_GRADE_FAINT,      /* visible, but below the alarm band     */
    PO_GRADE_LOUD,       /* a camped defender alarms              */
    PO_GRADE_BLARING,    /* alarms, and every family agrees       */
} po_grade_t;

typedef enum {
    PO_TELL_NONE = 0,
    PO_TELL_RATE,      /* the sheer volume of frames             */
    PO_TELL_SHAPE,     /* broadcast targeting, or burst runs     */
    PO_TELL_FORGERY,   /* claiming an address it cannot sustain  */
    PO_TELL_AFTERMATH, /* the clients it knocked off came back   */
    PO_TELL_REASON,    /* a monoculture of reason codes          */
} po_tell_t;

typedef struct {
    po_grade_t grade;
    po_tell_t dominant_tell;

    uint8_t camped_score;   /* what a camped defender scores it    */
    uint8_t hopping_score;  /* what a hopping defender scores it   */
    uint8_t stealth_gap;    /* camped - hopping: the cost of camping */
    uint8_t families_lit;   /* how many families the camped view lit */

    /* True when a hopping defender would NOT alarm although a camped one
     * would - the single most operationally useful fact here. */
    bool invisible_to_hoppers;

    const char *headline;   /* the one-line summary                */
    const char *tell_name;  /* the dominant tell, named            */
    const char *guidance;   /* what made it loud, stated plainly   */
} po_report_t;

/* camped and hopping are the SAME attack graded against two receiver
 * postures (see pharos_watch.h). Either may be NULL, in which case that side
 * is treated as unknown and the report says so. */
void po_assess(const pw_verdict_t *camped, const pw_verdict_t *hopping,
               po_report_t *out);

const char *po_grade_name(po_grade_t g);
const char *po_tell_name(po_tell_t t);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_OPSEC_H */
