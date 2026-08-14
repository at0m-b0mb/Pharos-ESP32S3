/* Pharos - the on-device LVGL HUD
 *
 * The widgets that actually light the panel. Deliberately small and
 * conservative: a background, a gauge arc, four labels and a receive-only
 * pip - built once, then updated in place. Nothing here allocates per frame,
 * and nothing here decides anything; it renders what a lens already computed.
 *
 * The round-screen *geometry* work (pharos_round / pharos_dial, and the
 * Virtual Pharos renderer) is the design reference for where things sit. This
 * is the LVGL realisation of the same layout: core readout in the middle, the
 * evidence gauge on the ring, status on the rim.
 *
 * Every function here MUST be called with the LVGL lock held
 * (pharos_bsp_display_lock) because the vendor BSP runs LVGL on its own task.
 */
#ifndef PHAROS_HUD_H
#define PHAROS_HUD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build the widget tree on the active screen. Safe to call once at boot.
 * Returns false if LVGL is not available on this build. */
bool pharos_hud_create(void);

/* Have the widgets actually been built? Reported by the `diag` command: a
 * panel that is up with no HUD on it is a different fault from a dark one. */
bool pharos_hud_present(void);

/* Touch navigation.
 *
 * The panel is a touchscreen (BSP_CAPS_TOUCH), and the vendor BSP already
 * registers it with LVGL, so navigation is expressed as LVGL event handlers
 * rather than hand-rolled hit-testing against the dial geometry. That means it
 * works with whatever rotation is in force, for free.
 *
 *   tap the LEFT third   -> previous lens
 *   tap the RIGHT third  -> next lens
 *   long-press anywhere  -> stop the active lens (home)
 */
typedef enum {
    PHAROS_NAV_PREV = 0,
    PHAROS_NAV_NEXT,
    PHAROS_NAV_HOME,
} pharos_nav_t;

typedef void (*pharos_hud_nav_cb_t)(pharos_nav_t what);

/* Install the handler the touch zones call. Safe to call before the HUD is
 * built; it is remembered and applied when the widgets are created. */
void pharos_hud_set_nav_cb(pharos_hud_nav_cb_t cb);

/* Flash a short message over the HUD - used to confirm a tap landed. */
void pharos_hud_toast(const char *msg);

/* Update the readout. Any string may be NULL to leave it unchanged.
 *
 *   lens    - the active tool, e.g. "SPECTRUM"
 *   big     - the headline value, e.g. "88" or "--"
 *   band    - the verdict word, e.g. "FLOOD LIKELY"
 *   detail  - a small line under it, e.g. "camped ch6  dwell 100%"
 *   score   - 0..100, drives the gauge arc
 *   rgb     - accent colour for the band + gauge (0xRRGGBB)
 */
void pharos_hud_update(const char *lens, const char *big, const char *band,
                       const char *detail, int score, uint32_t rgb);

/* The boot splash: the Pharos identity while the radio comes up. */
void pharos_hud_splash(const char *version, bool fence_clean);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_HUD_H */
