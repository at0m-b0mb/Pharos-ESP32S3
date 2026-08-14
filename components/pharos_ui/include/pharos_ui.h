/* Pharos - the user interface runtime
 *
 * pharos_ui_run owns the device once the board is up. It builds the Lamp Room
 * dial from the self-registered lens list (it does not know any lens by name),
 * dispatches touch, ticks the active lens, and pumps the analytics loop that
 * drains the active lens' ingest ring.
 *
 * The LVGL widget layer that draws the dial, the gauge and the cards on the
 * round panel is milestone M2. Until it lands, pharos_ui_run brings the
 * runtime up honestly - it mounts lenses, runs their ticks, pumps their buses
 * and logs their verdicts - so the whole firmware is exercisable on hardware
 * over the serial console before a single pixel is drawn. The geometry those
 * widgets will use (pharos_round, pharos_dial) is already complete and tested.
 */
#ifndef PHAROS_UI_H
#define PHAROS_UI_H

#include <stdbool.h>

#include "pharos_bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "pharos_aegis.h"

/* The Aegis latch. The UI loop feeds it from whichever lens is active (see
 * pharos_lens_t::stage_report), so it accumulates findings across lens
 * changes and remembers them long after the air has gone quiet. */
/* Ask for a lens change from ANOTHER task (the console).
 *
 * Lens switching happens only on the UI task: it tears down and restarts the
 * radio, which must not race the analytics loop and must not run on the
 * console REPL's small stack. Returns false immediately for an unknown id;
 * true means the request was filed, and it is applied within a tick. */
bool pharos_ui_request_lens(const char *id);
void pharos_ui_request_stop(void);

bool pharos_ui_aegis_snapshot(pa_verdict_t *out);
void pharos_ui_aegis_ack(void);

/* Never returns. fence_ok gates the radio lenses: when the transmit fence did
 * not verify clean, the UI still runs but refuses to launch anything holding a
 * radio capability, and says why. */
void pharos_ui_run(const pharos_bsp_status_t *bsp, bool fence_ok);

/* Pump one batch of ingest events for the active lens: drain its ring, call
 * on_event per event. Exposed so the analytics task can call it directly.
 * Returns the number of events dispatched. */
unsigned pharos_ui_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_UI_H */
