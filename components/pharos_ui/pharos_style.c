/* Pharos - LUMEN tokens, as arithmetic.
 *
 * Deliberately free of LVGL and of ESP-IDF so that the layout can be proved on
 * a laptop. "Does this string fit on a circle" is a question with an exact
 * answer, and finding it out by flashing a device and photographing the glass
 * is how the old dial shipped with SENTINEL written through its own headline.
 */
#include "pharos_style.h"

const int16_t PS_TYPE_PX[PS_TYPE_N] = {
    [PS_TYPE_MICRO]  = 14,
    [PS_TYPE_LABEL]  = 16,
    [PS_TYPE_BODY]   = 20,
    [PS_TYPE_TITLE]  = 28,
    [PS_TYPE_HERO]   = 36,
    [PS_TYPE_METRIC] = 48,
};

int16_t ps_text_w(ps_type_t t, unsigned chars)
{
    if (t >= PS_TYPE_N) {
        return 0;
    }
    return (int16_t)(PS_EM_W(PS_TYPE_PX[t]) * (int)chars);
}

ps_type_t ps_fit(unsigned chars, int16_t dy, int16_t r)
{
    /* Largest first: the caller wants the biggest that fits, not the first
     * that does. */
    for (int t = PS_TYPE_METRIC; t >= PS_TYPE_MICRO; t--) {
        const int16_t w = ps_text_w((ps_type_t)t, chars);
        const int16_t h = PS_TYPE_PX[t];
        if (pr_text_fits(w, h, dy, r)) {
            return (ps_type_t)t;
        }
    }
    return PS_TYPE_N;
}

unsigned ps_capacity(ps_type_t t, int16_t dy, int16_t r)
{
    if (t >= PS_TYPE_N) {
        return 0;
    }
    const int16_t h = PS_TYPE_PX[t];
    /* The chord at the FAR edge of the text box is what clips it, so measure
     * at whichever of the two edges is further from centre. A line of text
     * below the middle is clipped by its own bottom edge, not its baseline -
     * getting this backwards is what lets a descender escape the glass. */
    const int16_t dy_far = (dy >= 0) ? (int16_t)(dy + h / 2) : (int16_t)(dy - h / 2);
    const int16_t half = pr_chord_halfwidth(r, dy_far);
    if (half <= 0) {
        return 0;
    }
    const int16_t cw = PS_EM_W(PS_TYPE_PX[t]);
    if (cw <= 0) {
        return 0;
    }
    return (unsigned)((half * 2) / cw);
}

uint32_t ps_tint(uint32_t rgb, uint8_t pct)
{
    if (pct > 100u) {
        pct = 100u;
    }
    const uint32_t r = (rgb >> 16) & 0xFFu;
    const uint32_t g = (rgb >> 8) & 0xFFu;
    const uint32_t b = rgb & 0xFFu;
    /* Mix towards black. On an AMOLED the bottom of a channel has very few
     * levels, so a tint below about 12% bands visibly across a 300 px disc -
     * the callers that want "barely there" should use opacity on a solid fill
     * instead of asking for a darker colour. */
    const uint32_t rr = (r * pct) / 100u;
    const uint32_t gg = (g * pct) / 100u;
    const uint32_t bb = (b * pct) / 100u;
    return (rr << 16) | (gg << 8) | bb;
}

uint32_t ps_score_colour(int score)
{
    /* The same four thresholds the engines score against, so that the colour
     * changes at the point the WORD changes. A palette whose boundaries sat
     * anywhere else would show an amber screen saying QUIET. */
    if (score >= 80) {
        return PS_BAD;
    }
    if (score >= 60) {
        return PS_HIGH;
    }
    if (score >= 35) {
        return PS_WARN;
    }
    return PS_GOOD;
}

uint32_t ps_alert_colour(uint8_t alert)
{
    switch (alert) {
    case 0:  return PS_GOOD;
    case 1:  return PS_WARN;
    case 2:  return PS_HIGH;
    default: return PS_BAD;
    }
}

bool ps_touchable(int16_t w, int16_t h)
{
    return w >= PS_TOUCH_MIN && h >= PS_TOUCH_MIN;
}

int16_t ps_chip_w(unsigned n)
{
    if (n == 0u) {
        return 0;
    }
    /* Chips sit on one line at PS_Y_CHIPS, so the room they have is the chord
     * there - not the screen width. Forgetting that is how a fourth chip ends
     * up half off the glass on a circle that looked fine as a rectangle. */
    const int16_t half = pr_chord_halfwidth(PS_INNER_R, PS_Y_CHIPS + PS_CARD_GAP);
    const int16_t avail = (int16_t)(half * 2 - 8);
    const int16_t gaps = (int16_t)(PS_CARD_GAP * (int)(n - 1u));
    const int16_t w = (int16_t)((avail - gaps) / (int)n);

    /* A chip has to hold its label. Four capitals at PS_TYPE_LABEL is the
     * shortest useful family name (RATE, SHAPE...), and below that the chip
     * is a coloured smudge that means nothing without the manual - which is
     * the exact failure the labelled pips were introduced to fix. Say no
     * instead, and let the caller draw fewer. */
    const int16_t min = (int16_t)(ps_text_w(PS_TYPE_LABEL, 5) + 10);
    return (w >= min) ? w : (int16_t)0;
}
