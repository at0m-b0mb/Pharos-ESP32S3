/* Pharos - the palette, as data
 *
 * The face used to be twenty #defines in the middle of pharos_hud.c. That is
 * fine until somebody wants it to look different, and then it is a rebuild.
 * This makes the chrome a table so it can be chosen on the device.
 *
 * ---------------------------------------------------------------------------
 * WHAT A THEME MAY AND MAY NOT CHANGE
 *
 * A theme changes the CHROME: the rim, the grooves, the unlit pips, the body
 * text, the accent that the arcs and the cursor are drawn in. That is the part
 * of the screen that carries no meaning - it is the instrument, not the
 * reading.
 *
 * A theme does NOT change GREEN, AMBER, ORANGE or RED. Those four are the
 * verdict, and the whole tone contract exists so that a red on the Census page
 * means what a red on the Watch page means. A theme that could recolour danger
 * would be a way to make the device lie quietly, which is the one thing this
 * project spends most of its effort not doing.
 *
 * That leaves a trap, and it is the reason for the host test next to this
 * file: an ACCENT close to amber or red would put an ordinary structural line
 * in the colour reserved for "act on this". So every accent here is either far
 * from the warning hues or has no hue at all, and test_theme.c fails the build
 * if a new one is not.
 */
#ifndef PHAROS_THEME_H
#define PHAROS_THEME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name; /* <= 11 chars: it has to fit the settings row  */
    const char *note; /* what this one is FOR, not what it looks like */

    uint32_t accent;  /* arcs, cursor, the live band word             */
    uint32_t rim;     /* the one structural line that emits           */
    uint32_t track;   /* gauge groove                                 */
    uint32_t track2;  /* row rules, unlit segments                    */
    uint32_t pip_on;  /* a lit evidence pip                           */
    uint32_t pip_up;  /* an unlit one                                 */
    uint32_t text;    /* body                                         */
    uint32_t dim;     /* supporting numbers                           */
    uint32_t dimmer;  /* headings, units, page counters               */
    uint32_t denied;  /* a bar the region forbids                     */
    uint32_t ghost;   /* the score the caps took away                 */
} pharos_theme_t;

unsigned pharos_theme_count(void);
const pharos_theme_t *pharos_theme_at(unsigned index);

/* The one in force. Never NULL, including before pharos_theme_load(). */
const pharos_theme_t *pharos_theme(void);
unsigned pharos_theme_index(void);

/* Returns true when the choice actually changed, which is the caller's cue to
 * rebuild the face. Out-of-range indices are ignored rather than clamped: a
 * caller that computed one is wrong, and wrapping would hide it. */
bool pharos_theme_set(unsigned index);
bool pharos_theme_next(void);

/* Panel brightness, 0..100, remembered across boots with the theme. An AMOLED
 * at 20% is genuinely dark, which matters when the thing giving you away in a
 * corridor is your own screen. */
uint8_t pharos_theme_brightness(void);
bool pharos_theme_set_brightness(uint8_t pct);

/* NVS on the device, no-ops on the host. */
void pharos_theme_load(void);
void pharos_theme_save(void);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_THEME_H */
