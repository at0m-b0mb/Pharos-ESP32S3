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

/* Build the face again from scratch, which is how a theme change is applied.
 * Caller holds the display lock. Safe to call before create(); it does
 * nothing. See the comment on the definition for why this is a rebuild rather
 * than a walk over the widgets. */
void pharos_hud_rebuild(void);

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
    PHAROS_NAV_DETAIL, /* the bottom strip: "show me what you actually found" */
} pharos_nav_t;

/* The callback is invoked from LVGL's task, so implementations MUST only
 * record the intent and return. Doing the work here - activating a lens, which
 * restarts the radio - overflows LVGL's stack and reboots the device. That is
 * not hypothetical; it shipped in v1.8.0. */
typedef void (*pharos_hud_nav_cb_t)(pharos_nav_t what);
void pharos_hud_set_nav_cb(pharos_hud_nav_cb_t cb);

/* A row on the detail page was touched directly.
 *
 * Stepping a cursor with the side zones and then pressing the centre is three
 * or four presses to reach one setting, on a device somebody is holding up
 * one-handed. Touching the row itself is one. `row_on_page` is 0..ROWS-1; the
 * caller adds its own page offset, because the HUD does not know what it is
 * showing.
 *
 * Same rule as the nav callback: this runs on LVGL's task, so record the
 * intent and return. */
typedef void (*pharos_hud_row_cb_t)(unsigned row_on_page);
void pharos_hud_set_row_cb(pharos_hud_row_cb_t cb);

/* A dot on the home ring was touched. The HUD does the hit test itself,
 * because the ring's geometry is the HUD's and asking the UI layer to reach
 * into LVGL to find the touch point would put the coordinate system in two
 * places. A press that lands near no dot is NOT reported here - it falls
 * through to the ordinary nav meaning, so the bottom strip still works.
 *
 * Same rule as the others: this runs on LVGL's task, so record and return. */
typedef void (*pharos_hud_home_cb_t)(unsigned dot);
void pharos_hud_set_home_cb(pharos_hud_home_cb_t cb);

/* HOME: every armed watch at once, around the rim.
 *
 * The answer to "I should not have to be sitting inside the right lens at the
 * moment the attack happens". One dot per watch, positioned round the circle,
 * coloured by what that watch last found and FADED by how long ago it found
 * it - because there is one radio and the watches take turns, and a ring that
 * drew a forty-second-old reading in the same ink as a live one would be
 * claiming six receivers this device does not have.
 *
 * `dots` is the state per watch (a ptw_state_t), `fade` the freshness
 * (a ptw_freshness_t), both carried as plain bytes so this header does not
 * have to depend on the engine's. Tapping a dot activates that watch: see
 * pharos_hud_home_hit(). */
#define PHAROS_HUD_HOME_MAX 12
struct pharos_hud_home {
    unsigned n;
    const char *label[PHAROS_HUD_HOME_MAX];
    uint8_t state[PHAROS_HUD_HOME_MAX];
    uint8_t fade[PHAROS_HUD_HOME_MAX];
    uint8_t score[PHAROS_HUD_HOME_MAX];
    int active;          /* which watch holds the radio, or -1     */
    const char *headline;/* "all quiet", "ALERT", ...              */
    const char *sub;     /* "6 watches armed"                      */
    const char *clock;   /* wall time, or uptime if never set      */
    uint8_t worst_state; /* drives the centre colour               */
    uint8_t worst_score; /* drives the rim arc                     */
};
void pharos_hud_home(const struct pharos_hud_home *h);

/* Which home dot is under the last touch, or -1. Uses the same layout the
 * paint used, so the thing you press is the thing you saw. */
int pharos_hud_home_hit(int16_t x, int16_t y);

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

/* DETAIL: the active lens' own evidence, a page of rows at a time.
 *
 * `rows` points at `n` filled rows - already the page's worth, the caller does
 * the slicing - and page/pages drive the indicator. n = 0 draws the empty
 * state rather than a blank page, because a list with nothing in it is a
 * finding too. */
/* FOUR ROWS, NOT SIX, AND WHY THE NUMBER WENT DOWN.
 *
 * Six rows fitted the glass at a 36 px pitch. This panel is 466 px across a
 * 1.75 inch circle - 266 ppi - so 36 px is 3.4 mm, and a fingertip contact
 * patch is around 9 mm. The rows were legible and untappable, which is how
 * "sometimes I am not able to click them correctly" happens: not a calibration
 * fault, a target a finger physically cannot resolve.
 *
 * Four rows at 58 px is 5.5 mm, and each row's touch target spans the full
 * width of the glass, so only the vertical dimension has to be got right. The
 * cost is more paging, which is why the page controls are the two largest
 * targets on the screen. */
#define PHAROS_HUD_ROWS 4
#define PHAROS_HUD_ROW_PITCH 58
void pharos_hud_detail(const char *lens, const char *head_left,
                       const char *head_right,
                       const struct pharos_lens_row *rows, unsigned n,
                       unsigned page, unsigned pages, int focus, bool openable);

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
