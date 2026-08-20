/* Pharos - the Lamp Room dial and the evidence gauge
 *
 * Pure geometry, built on pharos_round.h. Both of the things that make the
 * round screen feel like an instrument live here, and both are host-tested,
 * because a layout bug on an embedded target costs a flash cycle to find and
 * a clipped label is invisible until somebody in the field cannot read it.
 *
 * The gauge is where the honesty model becomes something you can see. It
 * draws the evidence that was earned, the ceiling that observation quality
 * imposed, and - crucially - the points that were capped away, rendered as a
 * denied arc rather than silently dropped. The operator can watch confidence
 * appear as they stop hopping and camp.
 */
#ifndef PHAROS_DIAL_H
#define PHAROS_DIAL_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_round.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PD_MAX_ITEMS 16
#define PD_MAX_ARCS   6

/* ---- the Lamp Room: a radial launcher ------------------------------- */

typedef struct {
    unsigned n;
    float start_deg;    /* where item 0 sits                       */
    float step_deg;     /* angular spacing                         */
    float wedge_deg;    /* drawn width of one wedge, <= step       */
    int16_t r_inner, r_outer;
    bool hittable;      /* every wedge meets the 44 px thumb rule  */
    unsigned max_hittable; /* how many items would still be hittable */
} pd_dial_t;

/* Lay out n items around the rim annulus. Always fills the struct; check
 * .hittable before drawing, and fall back to a paged dial if it is false. */
void pd_dial_layout(unsigned n, float start_deg, int16_t r_inner, int16_t r_outer,
                    pd_dial_t *out);

/* Angle of item i under this layout. */
float pd_dial_item_angle(const pd_dial_t *d, unsigned i);

/* Which item is under the touch, or -1. */
int pd_dial_hit(const pd_dial_t *d, int16_t x, int16_t y);

/* The item currently at the top of the dial after the operator has dragged
 * it by delta degrees. This is the crown gesture's whole model. */
unsigned pd_dial_selected(const pd_dial_t *d, float rotation_deg);

/* ---- the home ring's labels ------------------------------------------
 *
 * Thirteen names round a 466 px circle collided, and the collision was not
 * where it looked. Spacing ALONG the arc was fine - about sixty pixels each -
 * but the text is horizontal, so the two labels nearest the top of the dial
 * (and the two nearest the bottom) end up side by side with only that gap
 * between them, for names that want fifty. They ran into each other and read
 * as one long word.
 *
 * Photographing the screen to find out whether a layout fits is a slow and
 * unreliable way to answer a question that is pure arithmetic. So the layout
 * is a function, and test_dial.c proves that for EVERY count the ring can
 * carry, no two label boxes overlap and none escapes the safe radius. It has
 * to hold for every count rather than the shipped default, because the ring is
 * now something the operator configures.
 *
 * Where one radius will not do, adjacent labels alternate between two - a pair
 * that is horizontally close is then vertically apart.
 *
 * `gap` is the clear space WANTED between two labels; the layout reports what
 * it actually achieved in `gap_px` and never refuses, because the ring is
 * something the operator configures and must be drawn as well as possible
 * whatever they chose. It, and it is the whole
 * difference between a ring that passes a test and one that looks right. The
 * first version of this checked only for overlap, and thirteen names at one
 * radius passed it - with five pixels between them, which reads as one long
 * word on the glass. Not overlapping is not the same as legible. */
typedef struct {
    int16_t r_even, r_odd; /* label radius for even / odd items */
    int16_t r_dot;         /* where the dots sit                */
    bool staggered;        /* true when the two radii differ    */
    int16_t gap_px;        /* clear space it managed between labels */
} pd_ring_t;

/* Lay out a ring of n labelled dots. `label_w` and `label_h` are the widest
 * and tallest a label can be, in pixels. */
void pd_ring_layout(unsigned n, int16_t label_w, int16_t label_h, int16_t gap,
                    pd_ring_t *out);

/* The label radius for item i under that layout. */
int16_t pd_ring_label_r(const pd_ring_t *r, unsigned i);

/* True when no two of the n label boxes overlap and all sit inside the safe
 * radius. This is what the test asserts; it is exported so the device can
 * assert it too if anybody ever wants to. */
bool pd_ring_fits(const pd_ring_t *r, unsigned n, int16_t label_w,
                  int16_t label_h, int16_t gap);

/* ---- the evidence gauge --------------------------------------------- */

typedef struct {
    float start_deg;
    float sweep_deg;
    uint8_t value;   /* the component's points, for the legend */
    bool denied;     /* earned, then removed by a cap or ceiling */
} pd_arc_t;

typedef struct {
    pd_arc_t arcs[PD_MAX_ARCS];
    unsigned n_arcs;
    float ceiling_deg; /* where the hard stop tick is drawn      */
    float score_deg;   /* where the score arc actually ends      */
    uint8_t denied_points; /* how much was capped away           */
    bool capped;
} pd_gauge_t;

/* Map a verdict onto an arc. components[] are the per-family point values in
 * draw order; score is what the engine finally allowed; ceiling is the most
 * this observation could have earned. Any component points beyond `score`
 * are emitted as denied arcs so the display shows what was taken away. */
void pd_gauge_layout(const uint8_t *components, unsigned n, uint8_t score,
                     uint8_t ceiling, float start_deg, float total_sweep,
                     pd_gauge_t *out);

/* ---- type that survives the curve ------------------------------------ */

/* The four sizes in the Pharos scale, largest first. */
extern const int16_t PD_TYPE_SCALE[4];

/* Largest size from the scale at which `chars` characters fit on one line,
 * centred, dy pixels off centre, inside radius r. Returns 0 when nothing in
 * the scale fits - which means the caller must shorten the string, not draw
 * it and hope. There is a host test asserting that every string the firmware
 * ships gets a non-zero answer at its intended radius. */
int16_t pd_label_size(unsigned chars, int16_t dy, int16_t r);

/* How many characters fit at a given size and offset. Use to truncate. */
unsigned pd_label_capacity(int16_t size, int16_t dy, int16_t r);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_DIAL_H */
