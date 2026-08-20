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

/* THE COLOUR ASKED FOR IS THE COLOUR SHOWN.
 *
 * The panel is RGB565: five bits of red, six of green, five of blue. Anything
 * else is rounded on the way to the glass, and the rounding is not uniform -
 * it lands differently on each channel, so a colour meant to be neutral comes
 * out tinted. That is exactly what happened: the Mono theme, whose entire
 * point is having no hue at all, was rendering 0xD8D8D8 as #dedbde - magenta -
 * and 0x161616 as #101410 - green. A grey theme was quietly the only themed
 * thing on the screen.
 *
 * Every palette value now sits ON the 565 lattice, so what is written here is
 * bit-for-bit what the panel emits, and a future colour that does not is a
 * test failure rather than a faint tint nobody can name. */
static uint32_t as_shown(uint32_t c)
{
    const uint32_t r5 = ((c >> 16) & 0xFF) >> 3;
    const uint32_t g6 = ((c >> 8) & 0xFF) >> 2;
    const uint32_t b5 = (c & 0xFF) >> 3;
    return (((r5 << 3) | (r5 >> 2)) << 16) | (((g6 << 2) | (g6 >> 4)) << 8) |
           ((b5 << 3) | (b5 >> 2));
}

static void test_theme_survives_the_panel(void)
{
    for (unsigned i = 0; i < pharos_theme_count(); i++) {
        const pharos_theme_t *t = pharos_theme_at(i);
        const uint32_t all[] = { t->accent, t->rim,    t->track,  t->track2,
                                 t->pip_on, t->pip_up, t->text,   t->dim,
                                 t->dimmer, t->denied, t->ghost };
        for (unsigned k = 0; k < sizeof(all) / sizeof(all[0]); k++) {
            CHECK(as_shown(all[k]) == all[k],
                  "%s colour %u survives RGB565 unchanged", t->name, k);
        }

        /* And the achromatic theme must actually BE achromatic once the panel
         * has had its way with it - which is a stricter thing than having been
         * written as a grey. */
        if (strcmp(t->name, "Mono") == 0) {
            for (unsigned k = 0; k < sizeof(all) / sizeof(all[0]); k++) {
                const uint32_t c = as_shown(all[k]);
                const int r = (int)((c >> 16) & 0xFF);
                const int g = (int)((c >> 8) & 0xFF);
                const int b = (int)(c & 0xFF);
                const int hi = (r > g ? (r > b ? r : b) : (g > b ? g : b));
                const int lo = (r < g ? (r < b ? r : b) : (g < b ? g : b));
                CHECK(hi - lo <= 3, "Mono colour %u is neutral on the glass", k);
            }
        }
    }
}

void test_theme(void)
{
    test_theme_survives_the_panel();
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
