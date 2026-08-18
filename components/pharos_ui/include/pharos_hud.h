/* Pharos - the on-device screen.
 *
 * Two views, because a 466 px circle cannot show a list and a reading at the
 * same time and should not try:
 *
 *   BROWSE - one lens at a time, with its NAME, what it actually DOES in plain
 *            words, and where you are in the list. Nothing is running. This is
 *            the answer to "I don't know what this device is doing": you read
 *            what a tool does before you start it, not after.
 *
 *   LIVE   - that lens running: the activity ribbon, the score arc with its
 *            ceiling, the headline number, the band word, the evidence pips
 *            and one line of what to do.
 *
 * ---------------------------------------------------------------------------
 * THE FLICKER, AND WHY THE API CHANGED
 *
 * The previous HUD was one flat pile of widgets on the screen object, and the
 * repaint pushed every value into every widget five times a second whether or
 * not anything had changed. Four things followed from that, and together they
 * are the "it flickers and breaks" the device was reported with:
 *
 *   1. Neither the screen nor the touch zones ever had LV_OBJ_FLAG_SCROLLABLE
 *      removed, and lv_obj_create() sets it. The content is laid out well
 *      past the screen's own bounds (labels at -122, zones 466 px tall), so a
 *      drag - or a slightly smeared tap, which on a round glass is most of
 *      them - SCROLLED THE WHOLE FACE and it never came back. That is the
 *      "breaks" half, and it is one flag.
 *
 *   2. pharos_hud_live() hid the summary label and pharos_hud_advice(), called
 *      immediately after it in the same repaint, showed it again. Every frame.
 *      Two full invalidations of a wide text block, forever.
 *
 *   3. lv_label_set_text() marks the label dirty even when the string is
 *      identical, so five labels and three arcs were redrawn 5 times a second
 *      with the same content, under a 210 px opaque disc that then had to be
 *      recomposited along with everything above it.
 *
 *   4. The advice label was LV_LABEL_LONG_WRAP with a size that changed as the
 *      text did, so its height changed, so it re-laid-out, so the invalidated
 *      region moved around underneath the arc.
 *
 * So this version: two page containers, shown and hidden only when the view
 * actually changes; every widget written through a dirty check that compares
 * against what is already there; no wrapping text anywhere; and scrolling
 * removed from the screen and from every object that is created. In the steady
 * state - a score that has not moved - a repaint now invalidates nothing at
 * all.
 *
 * Touch moves between them; see pharos_nav_t. The whole surface is LVGL, so it
 * follows the panel rotation without any geometry of ours.
 */
#ifndef PHAROS_HUD_H
#define PHAROS_HUD_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_lens.h"

#ifdef __cplusplus
extern "C" {
#endif

bool pharos_hud_create(void);

/* Have the widgets actually been built? Reported by the `diag` command: a
 * panel that is up with no HUD on it is a different fault from a dark one. */
bool pharos_hud_present(void);

/* Navigation intents, raised by touch or the BOOT button.
 *
 *   BROWSE view:  tap left/right = previous/next lens, tap centre = START it
 *   LIVE view:    tap left/right = previous/next lens, long-press = BACK
 */
typedef enum {
    PHAROS_NAV_PREV = 0,
    PHAROS_NAV_NEXT,
    PHAROS_NAV_SELECT,
    PHAROS_NAV_HOME,
} pharos_nav_t;

/* The callback is invoked from LVGL's task, so implementations MUST only
 * record the intent and return. Doing the work here - activating a lens, which
 * restarts the radio - overflows LVGL's stack and reboots the device. That is
 * not hypothetical; it shipped in v1.8.0. */
typedef void (*pharos_hud_nav_cb_t)(pharos_nav_t what);
void pharos_hud_set_nav_cb(pharos_hud_nav_cb_t cb);

/* BROWSE: show one lens and what it is for. */
void pharos_hud_browse(const char *name, const char *summary, const char *team,
                       unsigned index, unsigned total, uint32_t rgb);

/* LIVE: show a running lens.
 *
 * ONE call per repaint, taking the whole picture. The old API was three calls
 * - live(), then ceiling(), then advice() - which is how the summary label
 * ended up being hidden by one and shown by the next on every single frame.
 * A single entry point cannot disagree with itself.
 *
 * `d` may be NULL, which draws the "running, nothing to report yet" face. */
void pharos_hud_live(const char *lens, const struct pharos_lens_display *d,
                     uint32_t rgb_override);

/* The boot splash and the `screen test` command. */
void pharos_hud_splash(const char *version, bool fence_clean);
void pharos_hud_toast(const char *msg);

/* Paint six full-screen colour patches with their names.
 *
 * A panel driven with the wrong channel order still shows black as black and
 * white as white - only the mid-tones move - so a photograph of a normal screen
 * cannot tell you what is wrong. Naming each patch can: if the patch labelled
 * RED is blue, the red and blue channels are swapped; if everything is shifted,
 * the byte order is. It turns a guess into a reading. */
void pharos_hud_colourbars(void);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_HUD_H */
