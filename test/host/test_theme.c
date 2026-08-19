/* Host tests for the palette.
 *
 * The interesting failure a theme can cause is not "it looks bad" - that is
 * visible the moment anyone turns the device on. It is that an ACCENT lands
 * near amber or red, so an ordinary structural line - the arc, the row cursor,
 * the rim - is drawn in the colour reserved for "act on this". Somebody adding
 * a warm theme six months from now would have no reason to suspect it, and the
 * screen would not look broken; it would look urgent.
 *
 * So the rule is checked here rather than trusted: every accent is either far
 * from the warning hues or has no hue at all.
 */
#include <math.h>
#include <string.h>

#include "pharos_theme.h"
#include "test_support.h"

/* The four colours a theme may never take, from pharos_hud.c. */
#define V_AMBER  0xFFC34Au
#define V_ORANGE 0xEF9239u
#define V_RED    0xE75142u

static void to_hsv(uint32_t rgb, double *h, double *s, double *v)
{
    const double r = ((rgb >> 16) & 0xFF) / 255.0;
    const double g = ((rgb >> 8) & 0xFF) / 255.0;
    const double b = (rgb & 0xFF) / 255.0;
    const double mx = (r > g ? (r > b ? r : b) : (g > b ? g : b));
    const double mn = (r < g ? (r < b ? r : b) : (g < b ? g : b));
    const double d = mx - mn;
    *v = mx;
    *s = (mx <= 0.0) ? 0.0 : d / mx;
    if (d <= 0.0) {
        *h = 0.0;
        return;
    }
    double hh;
    if (mx == r) {
        hh = 60.0 * fmod((g - b) / d, 6.0);
    } else if (mx == g) {
        hh = 60.0 * (((b - r) / d) + 2.0);
    } else {
        hh = 60.0 * (((r - g) / d) + 4.0);
    }
    if (hh < 0.0) {
        hh += 360.0;
    }
    *h = hh;
}

static double hue_gap(double a, double b)
{
    double d = fabs(a - b);
    if (d > 180.0) {
        d = 360.0 - d;
    }
    return d;
}

/* Relative luminance, for the contrast floor against the black field. */
static double lum(uint32_t rgb)
{
    double c[3];
    c[0] = ((rgb >> 16) & 0xFF) / 255.0;
    c[1] = ((rgb >> 8) & 0xFF) / 255.0;
    c[2] = (rgb & 0xFF) / 255.0;
    for (int i = 0; i < 3; i++) {
        c[i] = (c[i] <= 0.04045) ? (c[i] / 12.92)
                                 : pow((c[i] + 0.055) / 1.055, 2.4);
    }
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

void test_theme(void)
{
    const unsigned n = pharos_theme_count();
    CHECK(n >= 2, "more than one theme, or the setting is pointless");

    for (unsigned i = 0; i < n; i++) {
        const pharos_theme_t *t = pharos_theme_at(i);
        CHECK(t != NULL, "every index below the count resolves");
        CHECK(t->name && t->name[0], "a theme has a name");
        CHECK(t->note && t->note[0], "a theme says what it is for");

        /* The right column of a settings row is 12 bytes. A name that does not
         * fit is not a cosmetic problem: it is cut mid-word on the glass. */
        CHECK(strlen(t->name) <= 11, "the name fits the settings row");
        /* And the note goes in the LEFT column of the expansion, 26 bytes. */
        CHECK(strlen(t->note) <= 25, "the note fits the expansion row");

        double h, sat, v;
        to_hsv(t->accent, &h, &sat, &v);

        /* THE RULE. An accent is either achromatic - in which case no warning
         * colour can be mistaken for it - or a long way round the wheel from
         * every one of them. */
        if (sat >= 0.15) {
            CHECK(hue_gap(h, 4.0) >= 75.0, "accent is not near red");
            double oh, os, ov;
            to_hsv(V_ORANGE, &oh, &os, &ov);
            CHECK(hue_gap(h, oh) >= 75.0, "accent is not near orange");
            to_hsv(V_AMBER, &oh, &os, &ov);
            CHECK(hue_gap(h, oh) >= 75.0, "accent is not near amber");
            to_hsv(V_RED, &oh, &os, &ov);
            CHECK(hue_gap(h, oh) >= 75.0, "accent is not near the verdict red");
        }

        /* Legibility on a black field. 3:1 is the large-text floor and every
         * one of these is 16 px or bigger. */
        const double bg = lum(0x000000);
        CHECK((lum(t->text) + 0.05) / (bg + 0.05) >= 3.0,
              "body text is legible on the field");
        CHECK((lum(t->dim) + 0.05) / (bg + 0.05) >= 3.0,
              "supporting numbers are legible on the field");
        CHECK((lum(t->dimmer) + 0.05) / (bg + 0.05) >= 1.6,
              "headings are at least visible on the field");

        /* Structure has to be dimmer than what sits on it, or the grooves
         * compete with the reading - which is what the blue-wash face did. */
        CHECK(lum(t->track) < lum(t->dim), "the groove is under the text");
        CHECK(lum(t->track2) <= lum(t->track), "the row rule is the quietest line");
        CHECK(lum(t->pip_up) < lum(t->pip_on), "an unlit pip reads as unlit");
    }

    /* Selection. Out of range is refused rather than wrapped: a caller that
     * computed one is wrong, and clamping would hide it. */
    const unsigned was = pharos_theme_index();
    CHECK(!pharos_theme_set(n), "an index past the end is refused");
    CHECK(!pharos_theme_set(n + 99u), "so is one well past it");
    CHECK(pharos_theme_index() == was, "and neither moved the choice");
    CHECK(!pharos_theme_set(was), "setting the one already in force is not a change");

    for (unsigned i = 0; i < n; i++) {
        pharos_theme_next();
    }
    CHECK(pharos_theme_index() == was, "a full cycle of next() returns home");

    CHECK(pharos_theme() != NULL, "there is always a theme in force");
    CHECK(pharos_theme() == pharos_theme_at(pharos_theme_index()),
          "and it is the one the index names");

    /* Brightness never reaches zero: a dark screen with no way back short of a
     * USB cable is a bricked screen to whoever is holding it. */
    pharos_theme_set_brightness(0);
    CHECK(pharos_theme_brightness() >= 10, "brightness has a floor");
    pharos_theme_set_brightness(200);
    CHECK(pharos_theme_brightness() == 100, "and a ceiling");
    pharos_theme_set_brightness(70);
    CHECK(pharos_theme_brightness() == 70, "and takes an ordinary value");
    CHECK(!pharos_theme_set_brightness(70), "setting it twice is not a change");
    pharos_theme_set_brightness(100);
}
