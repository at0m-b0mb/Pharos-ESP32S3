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
#include <stddef.h>

#include "pharos_bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the screen is, for the console.
 *
 * `rows` printed the list but not which row the centre tap would act on, so
 * the one thing that had to be verified by hand - does the cursor move, does
 * it open the right thing - was the one thing a log could not show. Returns
 * the cursor's absolute row index, or -1 when the detail page is not up.
 * `opened` receives the row currently expanded, or -1. */
int pharos_ui_detail_cursor(int *opened);

/* Test seam: touch row `row_on_page` (0..PHAROS_HUD_ROWS-1) of the detail
 * page, exactly as a finger landing on it would. Used by the `tap` console
 * command, which is how the detail interaction is verified over USB. */
void pharos_ui_tap_row(unsigned row_on_page);

/* The watchtower, for the `tower` console command: how the ring is doing and
 * whose turn it is. Fills at most `cap` lines into `buf`. */
void pharos_ui_tower_dump(char *buf, size_t cap);

/* ---- customising the ring ----
 *
 * Which watches the home ring carries is the operator's choice: somebody
 * working a Wi-Fi engagement does not need the microphone, and every watch left
 * on is airtime the ones they care about do not get. The Ring lens edits this;
 * the choice is remembered across boots.
 */
/* ---- motion ----
 *
 * The board's QMI8658, read once per UI tick. `state` is a pm_state_t carried
 * as a byte. Returns false when no IMU answered - which callers must treat as
 * UNKNOWN, never as "not moving"; see pharos_motion.h. */
bool pharos_ui_motion(uint8_t *state, uint32_t *steps, uint32_t *still_for_s);

/* Has the person gone somewhere since `since_steps`? The gate Vigil uses
 * before it will call anything a follower. True on a board with no IMU. */
bool pharos_ui_has_travelled(uint32_t since_steps);

unsigned pharos_ui_ring_count(void);
bool pharos_ui_ring_at(unsigned i, const char **name, bool *armed,
                       uint8_t *period, uint8_t *state);
bool pharos_ui_ring_toggle(unsigned i);
bool pharos_ui_ring_cycle_period(unsigned i);
uint32_t pharos_ui_ring_lap_ms(void);
bool pharos_ui_ring_running(void);
void pharos_ui_ring_set_running(bool on);

/* Forget the saved ring and rebuild it from the shipped defaults. The way back
 * when a stored choice - or a stale one from an older firmware - is not what
 * somebody wants. */
void pharos_ui_ring_reset(void);

#include "pharos_aegis.h"
/* pharos_nav_t lives here; the nav request below speaks in it. */
#include "pharos_hud.h"

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

/* File a navigation intent from another task - the console, a test harness,
 * anything that is not a finger. The UI task applies it on its next tick, on
 * exactly the same path touch and the BOOT button use, so what is exercised
 * here is what an operator would get. Added because the touch-only paths
 * could not otherwise be tested without a hand on the glass. */
void pharos_ui_request_nav(pharos_nav_t what);

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
