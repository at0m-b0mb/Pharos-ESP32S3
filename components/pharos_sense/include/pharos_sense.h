/* Pharos - the motion service
 *
 * Owns the one task that reads the IMU, and the snapshot everything else
 * reads. Separate from both the driver (pharos_bsp) and the judgement
 * (pharos_motion) because it is neither: it is the thing that samples at a
 * fixed rate regardless of which lens happens to be running.
 *
 * That last part is the point. Motion has to be continuous to be useful - the
 * question "have you moved since you started looking at this tracker" cannot
 * be answered by a sensor that only runs while the tracker lens is open. So
 * this starts at boot and keeps running, and the lenses read a snapshot.
 */
#ifndef PHAROS_SENSE_H
#define PHAROS_SENSE_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_motion.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the sampler. Safe to call when there is no IMU: it probes, finds
 * nothing, and every reader gets PM_UNKNOWN forever - which is the correct
 * answer, and specifically not PM_STILL. */
void pharos_sense_start(void);

/* The current motion verdict. Always fills `out`. */
void pharos_sense_motion(pm_verdict_t *out);

/* Steps counted since boot; 0 when there is no sensor. */
uint32_t pharos_sense_steps(void);

/* Has the person carrying this moved meaningfully since `mark` steps?
 * Delegates to pm_has_travelled, including its rule that a board with no IMU
 * must not veto anything. */
bool pharos_sense_travelled(uint32_t mark);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_SENSE_H */
