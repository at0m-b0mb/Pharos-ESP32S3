/* Pharos - the activity ribbon, shared
 *
 * The live face carries a sixteen-second ribbon along the top: what shape the
 * traffic had over time, as opposed to what the score is right now. A score
 * says whether something is happening; the ribbon says whether it arrived in
 * one violent burst or as a steady trickle, and those are not the same event
 * even when they average identically.
 *
 * Until now exactly ONE lens filled it. Watch grew a per-second histogram
 * inside its own engine because it needed one for its rate family, and the
 * ribbon came along for free. Every other lens left `has_history` false, so
 * nineteen of the twenty screens on this device drew an empty timeline -
 * reported from the glass as "there is no showing of the packets ... it's
 * completely zero", which was exactly right.
 *
 * Giving nineteen engines their own histogram would be nineteen chances to
 * get the same arithmetic subtly different. This is that arithmetic once: a
 * ring of per-second counters that any lens can feed with one call per event
 * and read back normalised in one more.
 *
 * ---------------------------------------------------------------------------
 * IT COUNTS EVENTS, IT DOES NOT KEEP THEM
 *
 * A slot is a count and a second. No addresses, no payloads, no identifiers -
 * there is nothing in here that could say WHO, only HOW MUCH and WHEN. That
 * matters because this now runs in the ingest path of most of the lenses on
 * the device, and a shared structure that quietly accumulated identifying
 * material would undo the care taken everywhere else.
 */
#ifndef PHAROS_PULSE_H
#define PHAROS_PULSE_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_lens.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One slot per second, matching the width of the ribbon the HUD draws. */
#define PHAROS_PULSE_SLOTS PHAROS_DISP_HISTORY

typedef struct {
    uint16_t slot[PHAROS_PULSE_SLOTS];
    uint32_t newest_sec;  /* wall second owning slot[newest_idx] */
    uint8_t newest_idx;
    bool primed;
} pharos_pulse_t;

void pharos_pulse_reset(pharos_pulse_t *p);

/* One event happened at t_us. The cheap call, for an ingest path. */
void pharos_pulse_note(pharos_pulse_t *p, uint64_t t_us);

/* n events at once, for a lens that counts in batches rather than singly. */
void pharos_pulse_add(pharos_pulse_t *p, uint64_t t_us, uint16_t n);

/* Fill `out` oldest-first, each slot normalised 0..255 against the window's
 * own peak - the ribbon shows SHAPE, so it is scaled to itself rather than to
 * any absolute rate the lens would have to invent.
 *
 * Returns false when nothing has ever been noted, which the caller should pass
 * straight through to has_history: a lens that has seen nothing must draw no
 * ribbon rather than a flat one, because a flat ribbon is a claim that the
 * room was quiet and an empty one is an admission that nothing was measured.
 */
bool pharos_pulse_fill(const pharos_pulse_t *p, uint64_t now_us,
                       uint8_t out[PHAROS_PULSE_SLOTS]);

/* The busiest second in the window, unnormalised - useful to a lens that
 * wants to put the peak in a detail row. */
uint16_t pharos_pulse_peak(const pharos_pulse_t *p, uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_PULSE_H */
