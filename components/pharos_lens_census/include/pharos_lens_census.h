/* Pharos - reading Census' graded table from another lens.
 *
 * Census holds the only list on the device of what access points are nearby
 * and how well defended each one is. Ward needs it to offer the operator a
 * network to guard, and the Survey needs it to accumulate; neither should be
 * re-parsing beacons of its own to get there.
 *
 * Snapshot semantics: the entry is COPIED out under Census' lock, so the
 * caller never holds a pointer into a table another core is rewriting.
 */
#ifndef PHAROS_LENS_CENSUS_H
#define PHAROS_LENS_CENSUS_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_census.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Copy out the index-th access point and its grade. Returns false past the
 * end, or if the table could not be locked promptly - a caller on the UI task
 * must never block behind the analytics core. */
bool pharos_lens_census_at(unsigned index, pc_ap_t *ap, pc_verdict_t *verdict);

/* How many are currently in the table. */
unsigned pharos_lens_census_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_LENS_CENSUS_H */
