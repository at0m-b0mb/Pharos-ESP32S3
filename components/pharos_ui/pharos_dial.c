#include "pharos_dial.h"

#include <math.h>

const int16_t PD_TYPE_SCALE[4] = { 64, 34, 20, 14 };

/* Mean advance width as a fraction of the em, measured for the shipped face.
 * Deliberately pessimistic: it is better to shorten a string that would have
 * fitted than to clip one that would not. */
#define PD_ADVANCE_NUM 3
#define PD_ADVANCE_DEN 5 /* 0.60 em */

/* Cap height plus leading, as a fraction of the em. */
#define PD_LINE_NUM 5
#define PD_LINE_DEN 4 /* 1.25 em */

static float norm_deg(float d)
{
    while (d < 0.0f)    d += 360.0f;
    while (d >= 360.0f) d -= 360.0f;
    return d;
}

/* ---- dial ------------------------------------------------------------ */

void pd_dial_layout(unsigned n, float start_deg, int16_t r_inner, int16_t r_outer,
                    pd_dial_t *out)
{
    if (!out) {
        return;
    }
    out->n = n;
    out->start_deg = norm_deg(start_deg);
    out->r_inner = r_inner;
    out->r_outer = r_outer;
    out->step_deg = n ? (360.0f / (float)n) : 360.0f;

    /* The thumb rule is evaluated at the inner radius, where the arc is
     * shortest: if it is hittable there, it is hittable everywhere in the
     * annulus. */
    const float min_deg = pr_min_wedge_deg(r_inner > 0 ? r_inner : 1);
    out->max_hittable = (min_deg > 0.0f) ? (unsigned)(360.0f / min_deg) : 0u;
    out->hittable = (n > 0) && (out->step_deg >= min_deg);

    /* Leave a hairline gap so adjacent wedges read as separate targets. */
    out->wedge_deg = out->step_deg * 0.88f;
    if (out->wedge_deg < min_deg && out->hittable) {
        out->wedge_deg = min_deg;
    }
}

float pd_dial_item_angle(const pd_dial_t *d, unsigned i)
{
    if (!d || d->n == 0) {
        return 0.0f;
    }
    return norm_deg(d->start_deg + d->step_deg * (float)(i % d->n));
}

int pd_dial_hit(const pd_dial_t *d, int16_t x, int16_t y)
{
    if (!d || d->n == 0) {
        return -1;
    }
    const float r = pr_radius_of(x, y);
    if (r < (float)d->r_inner || r > (float)d->r_outer) {
        return -1;
    }
    for (unsigned i = 0; i < d->n; i++) {
        const float centre = pd_dial_item_angle(d, i);
        /* Wedges are centred on their item, so the sector starts half a
         * wedge earlier. */
        const float a0 = norm_deg(centre - d->wedge_deg * 0.5f);
        if (pr_wedge_hit(x, y, d->r_inner, d->r_outer, a0, d->wedge_deg)) {
            return (int)i;
        }
    }
    return -1;
}

unsigned pd_dial_selected(const pd_dial_t *d, float rotation_deg)
{
    if (!d || d->n == 0) {
        return 0;
    }
    /* Item 0 sits at start_deg; rotating the dial by `rotation_deg` brings
     * whichever item now lies nearest 12 o'clock under the selector. */
    const float offset = norm_deg(-(d->start_deg + rotation_deg));
    const float idx = offset / d->step_deg;
    return ((unsigned)lrintf(idx)) % d->n;
}

/* ---- gauge ----------------------------------------------------------- */

void pd_gauge_layout(const uint8_t *components, unsigned n, uint8_t score,
                     uint8_t ceiling, float start_deg, float total_sweep,
                     pd_gauge_t *out)
{
    if (!out) {
        return;
    }
    out->n_arcs = 0;
    out->denied_points = 0;
    out->capped = false;
    out->ceiling_deg = norm_deg(start_deg + total_sweep * (float)ceiling / 100.0f);
    out->score_deg = norm_deg(start_deg + total_sweep * (float)score / 100.0f);

    if (!components || n == 0) {
        return;
    }
    if (n > PD_MAX_ARCS) {
        n = PD_MAX_ARCS;
    }

    /* Walk the components in draw order, converting points to degrees. The
     * moment the running total passes the score the engine allowed, the rest
     * is drawn as denied: those points were earned by the evidence and then
     * removed by a cap or a ceiling, and hiding that would make the gauge a
     * worse teacher than the log. */
    unsigned running = 0;
    float cursor = norm_deg(start_deg);

    for (unsigned i = 0; i < n; i++) {
        const unsigned pts = components[i];
        if (pts == 0) {
            continue;
        }
        const unsigned allowed = (running >= score) ? 0u
                                 : ((running + pts > score) ? (score - running) : pts);
        const unsigned denied = pts - allowed;

        if (allowed) {
            pd_arc_t *a = &out->arcs[out->n_arcs++];
            a->start_deg = cursor;
            a->sweep_deg = total_sweep * (float)allowed / 100.0f;
            a->value = (uint8_t)allowed;
            a->denied = false;
            cursor = norm_deg(cursor + a->sweep_deg);
        }
        if (denied && out->n_arcs < PD_MAX_ARCS) {
            pd_arc_t *a = &out->arcs[out->n_arcs++];
            a->start_deg = cursor;
            a->sweep_deg = total_sweep * (float)denied / 100.0f;
            a->value = (uint8_t)denied;
            a->denied = true;
            cursor = norm_deg(cursor + a->sweep_deg);
            out->denied_points = (uint8_t)(out->denied_points + denied);
            out->capped = true;
        }
        running += pts;
        if (out->n_arcs >= PD_MAX_ARCS) {
            break;
        }
    }
}

/* ---- type ------------------------------------------------------------ */

unsigned pd_label_capacity(int16_t size, int16_t dy, int16_t r)
{
    if (size <= 0) {
        return 0;
    }
    const int16_t line_h = (int16_t)(((int32_t)size * PD_LINE_NUM) / PD_LINE_DEN);
    int16_t far_dy = (int16_t)((dy >= 0) ? (dy + line_h / 2) : (dy - line_h / 2));
    if (far_dy < 0) {
        far_dy = (int16_t)(-far_dy);
    }
    const int16_t half = pr_chord_halfwidth(r, far_dy);
    const int32_t advance = ((int32_t)size * PD_ADVANCE_NUM) / PD_ADVANCE_DEN;
    if (advance <= 0) {
        return 0;
    }
    const int32_t chars = ((int32_t)half * 2) / advance;
    return (chars > 0) ? (unsigned)chars : 0u;
}

int16_t pd_label_size(unsigned chars, int16_t dy, int16_t r)
{
    if (chars == 0) {
        return PD_TYPE_SCALE[0];
    }
    for (unsigned i = 0; i < 4; i++) {
        if (pd_label_capacity(PD_TYPE_SCALE[i], dy, r) >= chars) {
            return PD_TYPE_SCALE[i];
        }
    }
    return 0; /* shorten the string; do not draw and hope */
}

/* ---- the home ring's labels; see pharos_dial.h ---------------------- */

#include <math.h>

static void ring_label_box(const pd_ring_t *r, unsigned i, unsigned n,
                           int16_t w, int16_t h, float *x0, float *y0,
                           float *x1, float *y1)
{
    const float step = 360.0f / (float)n;
    const float a = -90.0f + step * (float)i;
    const float rad = (float)pd_ring_label_r(r, i);
    const float cx = rad * cosf(a * 3.14159265f / 180.0f);
    const float cy = rad * sinf(a * 3.14159265f / 180.0f);
    *x0 = cx - (float)w / 2.0f;
    *x1 = cx + (float)w / 2.0f;
    *y0 = cy - (float)h / 2.0f;
    *y1 = cy + (float)h / 2.0f;
}

int16_t pd_ring_label_r(const pd_ring_t *r, unsigned i)
{
    if (!r) {
        return 0;
    }
    return (i & 1u) ? r->r_odd : r->r_even;
}

bool pd_ring_fits(const pd_ring_t *r, unsigned n, int16_t label_w,
                  int16_t label_h, int16_t gap)
{
    if (!r || n < 2u) {
        return true;
    }
    for (unsigned i = 0; i < n; i++) {
        float ax0, ay0, ax1, ay1;
        ring_label_box(r, i, n, label_w, label_h, &ax0, &ay0, &ax1, &ay1);

        /* Inside the safe radius, corners included - a box whose centre is in
         * bounds can still have a corner off the glass. */
        const float corners[4][2] = {
            { ax0, ay0 }, { ax1, ay0 }, { ax0, ay1 }, { ax1, ay1 },
        };
        for (unsigned c = 0; c < 4; c++) {
            const float d = sqrtf(corners[c][0] * corners[c][0] +
                                  corners[c][1] * corners[c][1]);
            if (d > (float)PR_SAFE_R) {
                return false;
            }
        }

        /* Clear of the core disc. A label whose box crosses the middle is
         * drawn over the headline, and the two together are unreadable. The
         * nearest corner to the centre is the one to test. */
        {
            const float nx = (ax0 > 0.0f) ? ax0 : ((ax1 < 0.0f) ? -ax1 : 0.0f);
            const float ny = (ay0 > 0.0f) ? ay0 : ((ay1 < 0.0f) ? -ay1 : 0.0f);
            if (sqrtf(nx * nx + ny * ny) < (float)PD_RING_CORE_R) {
                return false;
            }
        }

        /* Against every other label. Only the immediate neighbours can
         * realistically touch, but checking all of them costs nothing here and
         * cannot be fooled by an unusual count. */
        for (unsigned k = i + 1u; k < n; k++) {
            float bx0, by0, bx1, by1;
            ring_label_box(r, k, n, label_w, label_h, &bx0, &by0, &bx1, &by1);
            /* Inflated by the required gap, so "just touching" fails. */
            const float g = (float)gap;
            const bool apart = (ax1 + g <= bx0) || (bx1 + g <= ax0) ||
                               (ay1 + g <= by0) || (by1 + g <= ay0);
            if (!apart) {
                return false;
            }
        }
    }
    return true;
}

/* Best gap achievable at a given pair of radii, or a negative number when a
 * label corner would leave the glass. */
static float ring_score(const pd_ring_t *r, unsigned n, int16_t w, int16_t h)
{
    float worst = 1e9f;
    for (unsigned i = 0; i < n; i++) {
        float ax0, ay0, ax1, ay1;
        ring_label_box(r, i, n, w, h, &ax0, &ay0, &ax1, &ay1);
        const float corners[4][2] = {
            { ax0, ay0 }, { ax1, ay0 }, { ax0, ay1 }, { ax1, ay1 },
        };
        for (unsigned c = 0; c < 4; c++) {
            if (sqrtf(corners[c][0] * corners[c][0] +
                      corners[c][1] * corners[c][1]) > (float)PR_SAFE_R) {
                return -1.0f;
            }
        }

        /* AND IT MUST CLEAR THE CORE.
         *
         * This scorer maximised the space BETWEEN labels and had no opinion
         * about the middle of the dial, so the search happily bought elbow
         * room by pushing the inner ring inward - onto the headline. On the
         * glass that read as CENSUS, WARD and SQUALL written through "worth a
         * look". Disqualifying, not merely penalised: a label over the
         * headline makes two things unreadable and no amount of spacing
         * elsewhere compensates. */
        {
            const float nx = (ax0 > 0.0f) ? ax0 : ((ax1 < 0.0f) ? -ax1 : 0.0f);
            const float ny = (ay0 > 0.0f) ? ay0 : ((ay1 < 0.0f) ? -ay1 : 0.0f);
            if (sqrtf(nx * nx + ny * ny) < (float)PD_RING_CORE_R) {
                return -1.0f;
            }
        }

        for (unsigned k = i + 1u; k < n; k++) {
            float bx0, by0, bx1, by1;
            ring_label_box(r, k, n, w, h, &bx0, &by0, &bx1, &by1);
            const float gx = (bx0 - ax1) > (ax0 - bx1) ? (bx0 - ax1) : (ax0 - bx1);
            const float gy = (by0 - ay1) > (ay0 - by1) ? (by0 - ay1) : (ay0 - by1);
            const float g = (gx > gy) ? gx : gy;
            if (g < worst) {
                worst = g;
            }
        }
    }
    return (n < 2u) ? 1e9f : worst;
}

/* BEST EFFORT, NEVER FAILURE.
 *
 * An earlier version returned "does not fit" for thirteen watches, which is
 * true and useless: the ring is something the operator configures, so the
 * layout has to produce the best arrangement for whatever they chose rather
 * than give up and overlap. What the caller needs to know is how much room it
 * managed - `gap_px` - so the shipped default can be set somewhere comfortable
 * and anything tighter is a choice made knowingly.
 *
 * The measured numbers on this 466 px dial, seven-character names: eight
 * watches clear each other by 64 px, ten by 30, twelve by 24, and thirteen by
 * 10 - which is the point at which it starts reading as one long word. */
unsigned pd_ring_capacity(int16_t label_w, int16_t label_h, int16_t gap)
{
    /* Defined in terms of pd_ring_fits, so the capacity and the checker can
     * never disagree about what fits. */
    unsigned best = 0;
    for (unsigned n = 1; n <= 24u; n++) {
        pd_ring_t r;
        pd_ring_layout(n, label_w, label_h, gap, &r);
        if (!pd_ring_fits(&r, n, label_w, label_h, gap)) {
            break;
        }
        best = n;
    }
    return best;
}

void pd_ring_layout(unsigned n, int16_t label_w, int16_t label_h, int16_t gap,
                    pd_ring_t *out)
{
    if (!out) {
        return;
    }
    out->r_dot = 168;
    out->r_even = 142;
    out->r_odd = 142;
    out->staggered = false;
    out->gap_px = 0;
    out->capacity = n;
    if (n < 2u) {
        out->gap_px = 999;
        return;
    }

    /* ONE RADIUS, ALWAYS.
     *
     * Two radii buy clear space by putting alternate labels nearer the middle,
     * and on the glass that reads as broken rather than clever: the eye sees a
     * ring of names at one distance with every other one pulled inward, and
     * calls it misaligned. It was, reported exactly that way. A dial's labels
     * belong on a circle.
     *
     * So the only freedom left is HOW FAR OUT the circle sits, and how many
     * names it carries. Push the radius outward for more circumference, and
     * when even the outermost cannot give every label its clear space, carry
     * fewer names rather than moving some of them somewhere they do not
     * belong. */
    float best = -1.0f;
    for (int16_t r = 140; r <= 156; r = (int16_t)(r + 2)) {
        pd_ring_t t = { .r_even = r, .r_odd = r, .r_dot = 168,
                        .staggered = false, .gap_px = 0, .capacity = n };
        const float g = ring_score(&t, n, label_w, label_h);
        if (g > best) {
            best = g;
            *out = t;
            out->gap_px = (int16_t)((g < 0.0f) ? 0.0f : g);
        }
    }

    /* HOW MANY NAMES THIS CIRCLE CAN CARRY.
     *
     * When the ring cannot give every label its clear space, the honest answer
     * is that it holds fewer labels - not that the labels should be squeezed
     * or scattered. The caller reads `capacity` and names the watches that
     * matter most. */
    out->capacity = n;
    if (best < (float)gap) {
        out->capacity = 0;
        for (unsigned k = n; k >= 2u; k--) {
            for (int16_t r = 140; r <= 156; r = (int16_t)(r + 2)) {
                pd_ring_t t = { .r_even = r, .r_odd = r, .r_dot = 168,
                                .staggered = false, .gap_px = 0, .capacity = k };
                if (ring_score(&t, k, label_w, label_h) >= (float)gap) {
                    out->capacity = k;
                    break;
                }
            }
            if (out->capacity) {
                break;
            }
        }
    }
}
