#include "pharos_round.h"

#include <math.h>

#define PR_DEG2RAD 0.017453292519943295f

pr_point_t pr_polar_at(int16_t cx, int16_t cy, int16_t r, float deg)
{
    /* 0 deg is up, angles increase clockwise, screen y grows downward. */
    const float rad = (deg - 90.0f) * PR_DEG2RAD;
    pr_point_t p;
    p.x = (int16_t)lrintf((float)cx + (float)r * cosf(rad));
    p.y = (int16_t)lrintf((float)cy + (float)r * sinf(rad));
    return p;
}

pr_point_t pr_polar(int16_t r, float deg)
{
    return pr_polar_at(PR_CX, PR_CY, r, deg);
}

float pr_radius_of(int16_t x, int16_t y)
{
    const float dx = (float)x - (float)PR_CX;
    const float dy = (float)y - (float)PR_CY;
    return sqrtf(dx * dx + dy * dy);
}

float pr_bearing_of(int16_t x, int16_t y)
{
    const float dx = (float)x - (float)PR_CX;
    const float dy = (float)y - (float)PR_CY;
    float deg = atan2f(dx, -dy) / PR_DEG2RAD; /* up = 0, clockwise positive */
    if (deg < 0.0f) {
        deg += 360.0f;
    }
    return deg;
}

pr_zone_t pr_zone_of(int16_t x, int16_t y)
{
    const float r = pr_radius_of(x, y);
    if (r <= (float)PR_CORE_R) return PR_ZONE_CORE;
    if (r <= (float)PR_RING_R) return PR_ZONE_RING;
    if (r <= (float)PR_RIM_R)  return PR_ZONE_RIM;
    return PR_ZONE_OUTSIDE;
}

int16_t pr_chord_halfwidth(int16_t r, int16_t dy)
{
    const float rr = (float)r * (float)r;
    const float yy = (float)dy * (float)dy;
    if (yy >= rr) {
        return 0;
    }
    return (int16_t)floorf(sqrtf(rr - yy));
}

pr_rect_t pr_inscribe(int16_t r, int16_t aspect_w, int16_t aspect_h)
{
    pr_rect_t out = { 0, 0, 0, 0 };
    if (aspect_w <= 0 || aspect_h <= 0 || r <= 0) {
        return out;
    }
    /* A centred w x h rect fits inside radius r when (w/2)^2 + (h/2)^2 <= r^2.
     * With w = k*aw and h = k*ah, k = 2r / sqrt(aw^2 + ah^2). */
    const float aw = (float)aspect_w;
    const float ah = (float)aspect_h;
    const float k = (2.0f * (float)r) / sqrtf(aw * aw + ah * ah);
    out.w = (int16_t)floorf(aw * k);
    out.h = (int16_t)floorf(ah * k);
    out.x = (int16_t)(PR_CX - out.w / 2);
    out.y = (int16_t)(PR_CY - out.h / 2);
    return out;
}

bool pr_text_fits(int16_t text_w, int16_t text_h, int16_t dy, int16_t r)
{
    /* Both horizontal edges of the text box must clear the circle, so test
     * the corner furthest from centre: the one on the far side of dy. */
    int16_t far_dy = (int16_t)((dy >= 0) ? (dy + text_h / 2) : (dy - text_h / 2));
    if (far_dy < 0) {
        far_dy = (int16_t)(-far_dy);
    }
    const int16_t half = pr_chord_halfwidth(r, far_dy);
    return (text_w / 2) <= half;
}

static float norm_deg(float d)
{
    while (d < 0.0f)     d += 360.0f;
    while (d >= 360.0f)  d -= 360.0f;
    return d;
}

bool pr_wedge_hit(int16_t x, int16_t y, int16_t r0, int16_t r1, float a0, float sweep)
{
    const float r = pr_radius_of(x, y);
    if (r < (float)r0 || r > (float)r1) {
        return false;
    }
    if (sweep <= 0.0f) {
        return false;
    }
    if (sweep >= 360.0f) {
        return true;
    }
    const float rel = norm_deg(pr_bearing_of(x, y) - norm_deg(a0));
    return rel < sweep;
}

float pr_min_wedge_deg(int16_t r)
{
    if (r <= 0) {
        return 360.0f;
    }
    /* arc length = r * theta(rad); solve for theta at PR_TOUCH_MIN pixels. */
    const float deg = ((float)PR_TOUCH_MIN / (float)r) / PR_DEG2RAD;
    return (deg > 360.0f) ? 360.0f : deg;
}

float pr_value_to_deg(int32_t value, int32_t lo, int32_t hi, float a0, float sweep)
{
    if (hi == lo) {
        return a0;
    }
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    const float t = (float)(value - lo) / (float)(hi - lo);
    return norm_deg(a0 + t * sweep);
}

void pr_burnin_offset(uint64_t now_ms, int16_t *dx, int16_t *dy)
{
    /* Two incommensurate periods so the path does not repeat on a short
     * cycle: 7 minutes and 11 minutes, amplitude 3 px. Slow enough to be
     * invisible, wide enough that no subpixel sits on one value for long. */
    const float t = (float)(now_ms % 3600000ull) / 1000.0f;
    const float ax = 3.0f * sinf((2.0f * 3.14159265f / 420.0f) * t);
    const float ay = 3.0f * sinf((2.0f * 3.14159265f / 660.0f) * t);
    if (dx) *dx = (int16_t)lrintf(ax);
    if (dy) *dy = (int16_t)lrintf(ay);
}

float pr_dial_angle(unsigned i, unsigned n, float start_deg)
{
    if (n == 0) {
        return start_deg;
    }
    return norm_deg(start_deg + (360.0f * (float)i) / (float)n);
}
