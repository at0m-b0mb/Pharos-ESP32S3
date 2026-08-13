/* Pharos - round-screen geometry
 *
 * A 466x466 AMOLED that is actually a circle. The corners do not exist, so
 * the usual rectangular layout instincts quietly delete your content. Every
 * placement in Pharos goes through this file, which is pure maths and
 * therefore host-testable: the layout is verified on a laptop before any
 * pixel is lit.
 *
 * Angle convention: degrees clockwise from 12 o'clock, because that is how
 * people read a dial. Screen coordinates are the usual y-down.
 */
#ifndef PHAROS_ROUND_H
#define PHAROS_ROUND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PR_W  466
#define PR_H  466
#define PR_CX 233
#define PR_CY 233
#define PR_R  233

/* Three concentric zones. Every screen in the product is built from these,
 * which is what makes twenty different lenses feel like one instrument. */
#define PR_CORE_R 90  /* the one number that matters                */
#define PR_RING_R 180 /* arcs, polar plots, the body of the reading  */
#define PR_RIM_R  233 /* ticks, channel labels, battery, cap badges  */

/* Nothing renders outside this radius: the panel's own edge treatment and
 * any bezel eat the last few pixels, and glass curvature makes text there
 * unreadable at an angle. */
#define PR_SAFE_R 224

/* Smallest comfortable touch target, in pixels, on this pixel density. */
#define PR_TOUCH_MIN 44

typedef enum {
    PR_ZONE_CORE = 0,
    PR_ZONE_RING,
    PR_ZONE_RIM,
    PR_ZONE_OUTSIDE,
} pr_zone_t;

typedef struct {
    int16_t x, y;
} pr_point_t;

typedef struct {
    int16_t x, y, w, h;
} pr_rect_t;

/* Point at radius r and angle deg, measured clockwise from 12 o'clock. */
pr_point_t pr_polar(int16_t r, float deg);
pr_point_t pr_polar_at(int16_t cx, int16_t cy, int16_t r, float deg);

/* Radius and bearing of a screen point, relative to screen centre. */
float pr_radius_of(int16_t x, int16_t y);
float pr_bearing_of(int16_t x, int16_t y);

pr_zone_t pr_zone_of(int16_t x, int16_t y);

/* Half-width of the circle at a given vertical offset from centre. This is
 * the number that stops a long label being clipped by the curve: a line of
 * text placed dy pixels off centre may only be 2*this wide. */
int16_t pr_chord_halfwidth(int16_t r, int16_t dy);

/* Largest axis-aligned rect of the given aspect (w:h) centred on the screen
 * that fits inside radius r. Use for cards, lists and dialogs. */
pr_rect_t pr_inscribe(int16_t r, int16_t aspect_w, int16_t aspect_h);

/* Does a text run of the given pixel width and height fit, centred, at
 * vertical offset dy from centre? */
bool pr_text_fits(int16_t text_w, int16_t text_h, int16_t dy, int16_t r);

/* Wedge hit test for the radial menu: is (x,y) inside the annulus sector
 * from r0..r1 spanning a0..a0+sweep degrees? Handles wrap past 360. */
bool pr_wedge_hit(int16_t x, int16_t y, int16_t r0, int16_t r1, float a0, float sweep);

/* Angular width, in degrees, that gives at least PR_TOUCH_MIN of arc length
 * at radius r. Rim controls must be at least this wide or they cannot be
 * hit reliably with a thumb. */
float pr_min_wedge_deg(int16_t r);

/* Map a value onto an arc. Returns degrees clockwise from 12 o'clock. */
float pr_value_to_deg(int32_t value, int32_t lo, int32_t hi, float a0, float sweep);

/* AMOLED burn-in mitigation. Static HUD elements are offset by a slow
 * Lissajous walk of a few pixels; over an hour no pixel holds one colour.
 * Call with monotonic milliseconds and add the result to static positions. */
void pr_burnin_offset(uint64_t now_ms, int16_t *dx, int16_t *dy);

/* Places n items evenly around a dial, returning the angle of item i.
 * start_deg is where item 0 sits. */
float pr_dial_angle(unsigned i, unsigned n, float start_deg);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_ROUND_H */
