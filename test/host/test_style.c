/* Host tests for LUMEN, the design system.
 *
 * Two kinds of thing are checked here, and only one of them is about looks.
 *
 * The first is the CONTRACT: the four verdict colours are frozen, because a
 * red on the Census page has to mean what a red on the Watch page means. A
 * refresh that quietly warmed them, or a clever future theme that made them
 * configurable, would turn the whole honesty model into decoration. So they
 * are asserted byte-for-byte against the values the previous face shipped.
 *
 * The second is GEOMETRY, and it is here because the alternative is a flash
 * cycle and a photograph. Whether a string fits on a circle is arithmetic;
 * the old dial found out the other way and shipped with SENTINEL, CENSUS and
 * SQUALL written through the middle of its own headline. Anything the layout
 * promises - that the type scale fits its band, that four chips fit the chord
 * they sit on, that no touch target is smaller than a fingertip - is proved
 * on a laptop before a pixel is lit.
 */
#include <string.h>

#include "pharos_style.h"
#include "test_support.h"

void test_style(void)
{
    banner("style: the verdict palette is frozen");
    {
        /* These four are a contract with the operator, not a design choice.
         * If a refresh wants to change them it has to change this line too,
         * and that is the point: it becomes a decision somebody made on
         * purpose rather than a side effect of retouching a palette. */
        CHECK_EQ(PS_GOOD, 0x39DB84u);
        CHECK_EQ(PS_WARN, 0xFFC34Au);
        CHECK_EQ(PS_HIGH, 0xEF9239u);
        CHECK_EQ(PS_BAD,  0xE75142u);

        /* All four must be distinguishable from each other. Two verdicts that
         * look alike on glass are one verdict as far as the operator is
         * concerned. */
        const uint32_t v[4] = { PS_GOOD, PS_WARN, PS_HIGH, PS_BAD };
        for (unsigned i = 0; i < 4; i++) {
            for (unsigned j = i + 1; j < 4; j++) {
                CHECK(v[i] != v[j], "verdict colours %u and %u are identical", i, j);
            }
        }
    }

    banner("style: the colour changes where the word changes");
    {
        /* A screen showing an amber ring above the word QUIET is a screen
         * arguing with itself. The thresholds here have to be the ones the
         * bands use. */
        CHECK_EQ(ps_score_colour(0),   PS_GOOD);
        CHECK_EQ(ps_score_colour(34),  PS_GOOD);
        CHECK_EQ(ps_score_colour(35),  PS_WARN);
        CHECK_EQ(ps_score_colour(59),  PS_WARN);
        CHECK_EQ(ps_score_colour(60),  PS_HIGH);
        CHECK_EQ(ps_score_colour(79),  PS_HIGH);
        CHECK_EQ(ps_score_colour(80),  PS_BAD);
        CHECK_EQ(ps_score_colour(100), PS_BAD);

        /* Alert is a separate scale on purpose: a lens whose score is not a
         * threat measure reports here instead, which is what stops the home
         * ring shouting ALERT because a neighbour left WPS on. */
        CHECK_EQ(ps_alert_colour(0), PS_GOOD);
        CHECK_EQ(ps_alert_colour(1), PS_WARN);
        CHECK_EQ(ps_alert_colour(2), PS_HIGH);
        CHECK_EQ(ps_alert_colour(3), PS_BAD);
    }

    banner("style: a tint is the same hue, never a different one");
    {
        /* The aura behind the hero is the verdict colour mixed towards black.
         * If mixing shifted the hue, the glow would disagree with the word it
         * sits behind. Mixing towards black scales all three channels by the
         * same factor, so the RATIOS have to survive. */
        const uint32_t t = ps_tint(PS_BAD, 50);
        const uint32_t r = (t >> 16) & 0xFF, g = (t >> 8) & 0xFF, b = t & 0xFF;
        CHECK_EQ(r, ((PS_BAD >> 16) & 0xFF) / 2);
        CHECK_EQ(g, ((PS_BAD >> 8) & 0xFF) / 2);
        CHECK_EQ(b, (PS_BAD & 0xFF) / 2);

        CHECK_EQ(ps_tint(PS_GOOD, 100), PS_GOOD);
        CHECK_EQ(ps_tint(PS_GOOD, 0), 0x000000u);
        /* Out of range is clamped, not wrapped: a caller that computed 140%
         * is wrong, and wrapping would make it brighter than the source. */
        CHECK_EQ(ps_tint(PS_GOOD, 200), PS_GOOD);
    }

    banner("style: the scale is ordered and nothing is under 14px");
    {
        for (int t = 1; t < PS_TYPE_N; t++) {
            CHECK(PS_TYPE_PX[t] > PS_TYPE_PX[t - 1],
                  "type step %d is not larger than %d", t, t - 1);
        }
        /* The whole legibility argument in one assertion. 12 px on a 266 ppi
         * panel is about 1.1 mm of cap height and it reads as mush at arm's
         * length; half the old face was set in it. */
        CHECK(PS_TYPE_PX[PS_TYPE_MICRO] >= 14, "smallest type is under 14px");
    }

    banner("style: every band the screens use can hold its own type");
    {
        /* Each named offset in the vertical rhythm is checked against the
         * size the screens actually set there, at a realistic string length.
         * A band that cannot hold its content is a clipped label in the
         * field, and this is the cheapest possible place to find that out. */
        const struct { const char *what; int16_t dy; ps_type_t t; unsigned chars; } b[] = {
            { "clock",   PS_Y_CLOCK,   PS_TYPE_LABEL, 10 },
            { "context", PS_Y_CONTEXT, PS_TYPE_LABEL, 18 },
            { "hero",    PS_Y_HERO,    PS_TYPE_HERO,  12 },
            { "metric",  PS_Y_METRIC,  PS_TYPE_METRIC, 3 },
            { "detail",  PS_Y_DETAIL,  PS_TYPE_BODY,  20 },
            { "action",  PS_Y_ACTION,  PS_TYPE_LABEL, 22 },
            /* The guide's bands. The first draft of the tour put its body at
             * +132/+162 and its hint at +198 - fine on a rectangle, hopeless
             * on a circle, where the chord at +198 holds about thirteen
             * characters. Ten primitives escaped the glass. */
            { "guide title", PS_Y_GUIDE_TITLE, PS_TYPE_TITLE, 17 },
            { "guide l1",    PS_Y_GUIDE_L1,    PS_TYPE_BODY,  29 },
            { "guide l2",    PS_Y_GUIDE_L2,    PS_TYPE_BODY,  27 },
            { "guide hint",  PS_Y_GUIDE_HINT,  PS_TYPE_LABEL, 23 },
        };
        for (unsigned i = 0; i < sizeof(b) / sizeof(b[0]); i++) {
            const unsigned cap = ps_capacity(b[i].t, b[i].dy, PR_SAFE_R);
            CHECK(cap >= b[i].chars,
                  "band %s holds %u chars at its size, needs %u",
                  b[i].what, cap, b[i].chars);
        }
    }

    banner("style: ps_fit refuses rather than overflowing");
    {
        /* The important half of a fitting function is the half that says no.
         * A version that returned "smallest" instead of "does not fit" would
         * let a caller draw a 40-character string as a 14 px smear off the
         * edge of the glass, which is worse than not drawing it. */
        const ps_type_t big = ps_fit(3, 0, PR_SAFE_R);
        CHECK(big == PS_TYPE_METRIC, "3 chars across the middle should take the largest size");

        CHECK(ps_fit(200, 0, PR_SAFE_R) == PS_TYPE_N, "200 chars must not fit anywhere");
        /* Right at the top of the circle there is almost no chord at all. */
        CHECK(ps_fit(20, 220, PR_SAFE_R) == PS_TYPE_N,
              "a long line at the very bottom of the glass must be refused");

        /* Whatever ps_fit returns must actually fit - the function may not
         * disagree with the capacity function it is built on. */
        for (unsigned chars = 1; chars <= 24; chars++) {
            for (int16_t dy = -180; dy <= 180; dy = (int16_t)(dy + 20)) {
                const ps_type_t t = ps_fit(chars, dy, PR_SAFE_R);
                if (t < PS_TYPE_N) {
                    CHECK(ps_capacity(t, dy, PR_SAFE_R) >= chars,
                          "ps_fit chose a size that does not fit: %u chars at dy=%d",
                          chars, (int)dy);
                }
            }
        }
    }

    banner("style: chips fit the chord they sit on, or there are fewer");
    {
        /* Four is what the display contract carries, so four has to work. */
        const int16_t w4 = ps_chip_w(4);
        CHECK(w4 > 0, "four evidence chips must fit");
        CHECK(w4 >= ps_text_w(PS_TYPE_LABEL, 5),
              "a chip must be able to hold a five-character family name");

        /* Fewer chips are never narrower than more of them. */
        CHECK(ps_chip_w(2) >= ps_chip_w(4), "two chips should be wider than four");
        CHECK(ps_chip_w(0) == 0, "zero chips have no width");

        /* And the row of them has to stay inside the glass. */
        const int16_t total = (int16_t)(w4 * 4 + PS_CARD_GAP * 3);
        const int16_t half = pr_chord_halfwidth(PR_SAFE_R, PS_Y_CHIPS + PS_CARD_GAP);
        CHECK(total <= half * 2, "four chips overflow the chord at their own offset");

        /* Enough chips and it must give up rather than draw slivers. */
        CHECK_EQ(ps_chip_w(12), 0);
    }

    banner("style: a detail row's two columns fit its card");
    {
        /* The bug this guards: the row labels were children of the PAGE,
         * aligned by their bounding-box CENTRE at the card's left edge, so an
         * auto-sized label grew symmetrically and half of a long string ran
         * off the glass. Every sensor page lost the front of every row -
         * "Top models / advs" rendered as "p models / advs" on hardware.
         *
         * They are children of the card now, with explicit widths. This
         * asserts the budget those widths are drawn from actually holds the
         * longest row the display contract can carry. */
        const int16_t pad = 12, val_w = 104;
        for (unsigned i = 0; i < PS_CARDS; i++) {
            const int16_t stack =
                (int16_t)(PS_CARDS * PS_CARD_H + (PS_CARDS - 1) * PS_CARD_GAP);
            const int16_t dy = (int16_t)(-stack / 2 + (int)i * (PS_CARD_H + PS_CARD_GAP)
                                         + PS_CARD_H / 2 + 8);
            const int16_t far = (dy >= 0) ? (int16_t)(dy + PS_CARD_H / 2)
                                          : (int16_t)(dy - PS_CARD_H / 2);
            const int16_t w = (int16_t)((pr_chord_halfwidth(PR_SAFE_R, far) - 6) * 2);
            const int16_t lab_w = (int16_t)(w - val_w - pad * 2 - 8);

            CHECK(lab_w > 0, "row %u has no room for a label at all", i);
            /* struct pharos_lens_row carries left[26]; 25 printable chars at
             * PS_TYPE_LABEL must fit, or rows are silently ellipsised on
             * every page rather than only on the long ones. */
            CHECK(lab_w >= ps_text_w(PS_TYPE_LABEL, 20),
                  "row %u fits only %d px of label, needs 20 chars (%d px)",
                  i, (int)lab_w, (int)ps_text_w(PS_TYPE_LABEL, 20));
            /* And the value column must hold the widest thing right[12] can
             * be, or grades and counts get cut instead. */
            CHECK(val_w >= ps_text_w(PS_TYPE_BODY, 8),
                  "the value column cannot hold 8 characters");
            /* The two columns plus padding may not exceed the card. */
            CHECK(lab_w + val_w + pad * 2 + 8 <= w,
                  "row %u columns overflow their card", i);
        }
    }

    banner("style: nothing a finger must hit is smaller than a finger");
    {
        CHECK(ps_touchable(PS_CARD_H, PS_CARD_H), "a card must be tappable");
        CHECK(ps_touchable(PR_W, PS_CARD_H), "a full-width row must be tappable");
        CHECK(!ps_touchable(20, 20), "a 20px target must be rejected");
        /* 36 px rows were legible and untappable, which is precisely how "I
         * can't click them correctly" was reported. The card height exists to
         * be above this line. */
        CHECK(PS_CARD_H >= PS_TOUCH_MIN, "the card is smaller than a fingertip");
    }

    banner("style: the four cards fit the glass together");
    {
        const int16_t stack = (int16_t)(PS_CARDS * PS_CARD_H + (PS_CARDS - 1) * PS_CARD_GAP);
        CHECK(stack <= PR_SAFE_R * 2, "the card stack is taller than the safe area");
        /* Each card is centred in the stack, so the outermost ones are the
         * ones that can escape. Check the widest point of the top and bottom
         * card against the chord available there. */
        for (unsigned i = 0; i < PS_CARDS; i++) {
            const int16_t dy = (int16_t)(-stack / 2 + (int)i * (PS_CARD_H + PS_CARD_GAP)
                                         + PS_CARD_H / 2);
            const int16_t far = (dy >= 0) ? (int16_t)(dy + PS_CARD_H / 2)
                                          : (int16_t)(dy - PS_CARD_H / 2);
            CHECK(pr_chord_halfwidth(PR_SAFE_R, far) > 60,
                  "card %u has almost no width at dy=%d", i, (int)dy);
        }
    }
}
