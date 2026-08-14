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
 *   LIVE   - that lens running: the gauge, the headline number, the band word
 *            and a detail line.
 *
 * Touch moves between them; see pharos_nav_t. The whole surface is LVGL, so it
 * follows the panel rotation without any geometry of ours.
 */
#ifndef PHAROS_HUD_H
#define PHAROS_HUD_H

#include <stdbool.h>
#include <stdint.h>

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

/* LIVE: show a running lens. `summary` may be NULL. */
void pharos_hud_live(const char *lens, const char *big, const char *band,
                     const char *detail, int score, uint32_t rgb);

/* Kept for the boot splash and the `screen test` command. */
void pharos_hud_update(const char *lens, const char *big, const char *band,
                       const char *detail, int score, uint32_t rgb);
void pharos_hud_splash(const char *version, bool fence_clean);
void pharos_hud_toast(const char *msg);

/* The confidence ceiling, drawn as a dimmed arc behind the score: the part a
 * thin sweep EARNED and then had taken away. Showing it is the whole argument
 * of the project, so it is on the glass rather than only in a report. */
void pharos_hud_ceiling(int ceiling);

/* One line of what to DO about the reading, under the gauge. */
void pharos_hud_advice(const char *advice);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_HUD_H */
