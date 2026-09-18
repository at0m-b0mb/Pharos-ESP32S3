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
 * whatever they chose.
 *
 * Clear space is the whole difference between a ring that passes a test and
 * one that looks right: the first version checked only for overlap, and
 * thirteen names at one radius passed it - with five pixels between them,
 * which reads as one long word on the glass. Not overlapping is not the same
 * as legible.
 *
 * And labels must clear the CORE, the disc in the middle that carries the
 * headline. Missing that check is what put CENSUS, WARD and SENTINEL straight
 * through "worth a look" on the shipped dial. */
#define PD_RING_CORE_R 100

/* THE HEADLINE IS A WIDE BOX, NOT A DISC.
 *
 * PD_RING_CORE_R keeps labels out of a circle of radius 100 in the middle of
 * the dial. That is the right shape for the score and the clock, and the wrong
 * shape for the thing that actually occupies the middle: the verdict word, set
 * at 36 px and running to about +/-140 px horizontally while being barely 40 px
 * tall.
 *
 * A label at nine o'clock clears the CIRCLE comfortably - measured at 115 px
 * against a guard of 100 - and is still drawn straight through "WORTH A LOOK",
 * because at that height the headline extends far past 100. The guard was
 * satisfied and the screen was unreadable, which is the worst combination: a
 * check that passes while the thing it protects is broken.
 *
 * So the keep-out is the headline's own footprint. Half-width is generous on
 * purpose - the hero band is the one piece of text allowed to run wide, and a
 * label overlapping it costs two readings rather than one. */
/* The band is placed where the hero ACTUALLY sits, not at the centre of the
 * dial. PS_Y_HERO is -18 and the face is 36 px, so the word occupies roughly
 * y -40..+4; a box centred on zero would block the wrong rows - too low, and
 * eating the supporting number's line as well. Kept as literals because this
 * header is pure geometry and does not depend on the style scale. */
#define PD_HERO_HALF_W 148
#define PD_HERO_Y0     (-44)
#define PD_HERO_Y1     (6)

/* THE WIDEST A RING LABEL CAN ACTUALLY BE.
 *
 * Nine capitals of montserrat_12 - FOOTPRINT is the longest name on the dial -
 * which measures about 73 px, not the 68 the layout was being told. Four
 * pixels of lie is enough: the layout believed a staggered arrangement fitted,
 * the renderer drew wider text than that, and SENTINEL, CENSUS and SQUALL ended
 * up written through the middle of the face. The number lives here so the
 * layout and the renderer cannot disagree about it again. */
/* THE RING CARRIES SHORT NAMES.
 *
 * Once the headline's real footprint is respected, a nine-character label -
 * FOOTPRINT is the longest lens name - can be placed at only two of the
 * eleven positions. Seven characters fit at eight of them, which is the
 * difference between a ring that names what matters and a ring that names
 * almost nothing.
 *
 * So the ring truncates. It is an at-a-glance index, not a caption: the full
 * name is on the browse card and on the live face, one press away. Three of
 * the twenty-one lenses are affected (SENTINEL, SPECTRUM, FOOTPRINT) and each
 * is still unambiguous at seven. */
#define PD_RING_NAME_MAX 7
#define PD_RING_LABEL_W 74
typedef struct {
    int16_t r_even, r_odd; /* label radius for even / odd items */
    int16_t r_dot;         /* where the dots sit                */
    bool staggered;        /* true when the two radii differ    */
    int16_t gap_px;        /* clear space it managed between labels */
    unsigned capacity;     /* how many labels this dial can carry   */
} pd_ring_t;

/* Lay out a ring of n labelled dots. `label_w` and `label_h` are the widest
 * and tallest a label can be, in pixels. */
void pd_ring_layout(unsigned n, int16_t label_w, int16_t label_h, int16_t gap,
                    pd_ring_t *out);

/* The label radius for item i under that layout. */
int16_t pd_ring_label_r(const pd_ring_t *r, unsigned i);

/* CAN THE LABEL AT POSITION i BE DRAWN AT ALL?
 *
 * The capacity number answers "how many names fit if they are spread evenly",
 * and the ring does not spread them evenly - it has n fixed dot positions and
 * names whichever SUBSET has something to say. Those are different questions,
 * and the difference is not academic: with eleven watches and a capacity of
 * eight, the layout validated eight evenly-spaced angles while the ring drew
 * names at eleven-spaced ones, so a label could sit exactly where nothing had
 * been checked - through the headline.
 *
 * This asks about one position, at the angle it is actually drawn at. The
 * caller checks each name it intends to draw and silently drops the ones that
 * do not clear. A missing name is a small loss; a name written through the
 * verdict costs two readings. */
bool pd_ring_label_fits(const pd_ring_t *r, unsigned i, unsigned n,
                        int16_t label_w, int16_t label_h);

/* True when no two of the n label boxes overlap and all sit inside the safe
 * radius. This is what the test asserts; it is exported so the device can
 * assert it too if anybody ever wants to. */
bool pd_ring_fits(const pd_ring_t *r, unsigned n, int16_t label_w,
                  int16_t label_h, int16_t gap);

/* HOW MANY LABELS THIS DIAL CAN ACTUALLY CARRY.
 *
 * Fourteen names do not fit on a 466 px circle at any radius, and no amount of
 * shuffling changes that - the arc is only so long and the letters only so
 * small. Pretending otherwise produced a dial with SENTINEL written through
 * WHISPER and both written through the headline.
 *
 * So the layout is allowed to say no. The caller then labels the ones that
 * MATTER - whichever watch holds the radio, and anything with something to
 * report - and leaves the rest as bare dots. That is how an instrument is
 * normally drawn: not every tick is numbered. */
unsigned pd_ring_capacity(int16_t label_w, int16_t label_h, int16_t gap);

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
