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
