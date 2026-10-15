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

#include "pharos_lens.h"
#include "pharos_probe.h"
#include "pharos_watch.h"
#include "pharos_rival.h"
#include "pharos_vigil.h"
#include "pharos_roster.h"
#include "pharos_twin.h"
#include "pharos_karma.h"
#include "pharos_flood.h"
#include "pharos_acoustic.h"
#include "pharos_census.h"
#include "pharos_style.h"
#include "test_support.h"

static void test_tone_limit_is_not_a_verdict(void)
{
    banner("tone: the instrument talking about itself is not a verdict");

    /* THE CONTRACT THIS EXISTS TO PROTECT.
     *
     * Green, amber, orange and red are the verdict, and they mean the same
     * thing on every page - that is the whole reason a theme may not recolour
     * them. A permanent limit of the hardware ("this device cannot hear
     * classic Bluetooth") is not a verdict about the air, and spending a
     * verdict colour on it had a compounding cost: the same two ambers on
     * every Rival page in every session, until a warning that is always there
     * is a warning nobody reads.
     *
     * So LIMIT must be visible and must NOT be any of the four. */
    CHECK(ps_tone_verdict_colour(PHAROS_TONE_LIMIT) == 0u,
          "LIMIT is entitled to no verdict colour");

    /* And the four that ARE verdicts still get theirs, so this cannot be
     * satisfied by making everything chrome. */
    CHECK(ps_tone_verdict_colour(PHAROS_TONE_GOOD) == PS_GOOD, "GOOD is green");
    CHECK(ps_tone_verdict_colour(PHAROS_TONE_WARN) == PS_WARN, "WARN is amber");
    CHECK(ps_tone_verdict_colour(PHAROS_TONE_BAD)  == PS_BAD,  "BAD is red");
    CHECK(ps_tone_verdict_colour(PHAROS_TONE_DIM)  == 0u,
          "and chrome tones borrow nothing either");
}


/* EVERY LINE OF ADVICE MUST FIT THE PAGE THAT SHOWS ALL OF IT.
 *
 * The live face carries a GLIMPSE of the advice on two labels - 27 characters
 * at PS_Y_ACTION and 20 at PS_Y_ACTION2, because on a round panel the lower
 * row sits on a shorter chord - and ellipsizes what will not fit. That is the
 * design: the whole of it is one tap away on the advice page, which wraps to
 * HUD_ADVICE_LINES rows of HUD_ADVICE_CAP.
 *
 * So the contract worth asserting is not that advice fits the face. It is
 * that the page which promises the WHOLE text can hold it, because an engine
 * whose advice overflows there has written something nobody can ever read in
 * full.
 *
 * (An earlier version of this test asserted the two-row fit and flagged
 * twenty-seven strings. It was measuring the glimpse against a contract that
 * belongs to the page, and would have forced every engine to write worse
 * advice to satisfy a rule that does not exist.) */
#define ADVICE_LINES 7
#define ADVICE_CAP   40

static void check_advice(const char *who, int band, const char *s)
{
    CHECK(s != NULL, "%s band %d has no advice", who, band);
    if (!s) return;

    /* The page wraps with the same routine the firmware uses, so this is the
     * real layout rather than a restatement of it. */
    char lines[ADVICE_LINES][ADVICE_CAP + 4];
    const unsigned n = ps_wrap(s, ADVICE_CAP, &lines[0][0], ADVICE_LINES,
                               (unsigned)sizeof lines[0]);
    CHECK(n > 0, "%s band %d: advice wrapped to nothing", who, band);
    if (n == 0) return;

    unsigned kept = 0;
    for (unsigned i = 0; i < n; i++) {
        kept += (unsigned)strlen(lines[i]);
        CHECK(strlen(lines[i]) <= ADVICE_CAP,
              "%s band %d: wrapped line %u is %u characters, the page holds %u",
              who, band, i, (unsigned)strlen(lines[i]), (unsigned)ADVICE_CAP);
    }
    /* Every character must survive the wrap. The spaces at the breaks are
     * not kept, so one per boundary is allowed for. Counting them back out
     * is what catches a string whose tail the page silently drops. */
    const unsigned len = (unsigned)strlen(s);
    CHECK(kept + (n - 1u) >= len,
          "%s band %d: the page shows %u of %u characters - \"%s\"",
          who, band, kept + (n - 1u), len, s);
}

static void test_style_advice_fits_the_advice_page(void)
{
    banner("style: every line of advice fits the page that shows all of it");

    /* Measured from the layout rather than written as 27 and 20, so the test
     * follows the contract if the panel or the type ever changes. */
    /* The face's glimpse is the narrower of two rows, which is why the whole
     * text needs somewhere else to live. Asserted so the relationship is
     * recorded rather than assumed. */
    const unsigned cap_a = ps_capacity(PS_TYPE_LABEL, PS_Y_ACTION, PR_SAFE_R);
    const unsigned cap_b = ps_capacity(PS_TYPE_LABEL, PS_Y_ACTION2, PR_SAFE_R);
    CHECK(cap_a > 0 && cap_b > 0, "the action rows have no capacity");
    CHECK(cap_b <= cap_a, "the lower row should be the narrower one");

    for (int b = 0; b <= (int)PW_BAND_LIKELY; b++) {
        check_advice("watch/advice", b, pw_band_advice((pw_band_t)b));
        check_advice("watch/hint", b, pw_band_hint((pw_band_t)b));
    }
    for (int b = 0; b <= (int)PT_BAND_TWIN_LIKELY; b++) {
        check_advice("twin", b, pt_band_advice((pt_band_t)b));
    }
    for (int b = 0; b <= (int)PK_BAND_KARMA_LIKELY; b++) {
        check_advice("karma", b, pk_band_advice((pk_band_t)b));
    }
    for (int b = 0; b <= (int)PF_BAND_FLOOD_LIKELY; b++) {
        check_advice("flood", b, pf_band_advice((pf_band_t)b));
    }
    for (int b = 0; b < (int)PAC_BAND_COUNT; b++) {
        check_advice("acoustic", b, pac_band_hint((pac_verdict_band_t)b));
    }
    /* Census advises from the whole verdict, so every grade and every cap
     * combination it can reach has to be walked. */
    for (int g = 0; g <= (int)PC_GRADE_A_PLUS; g++) {
        for (unsigned caps = 0; caps < 128u; caps++) {
            pc_verdict_t v;
            memset(&v, 0, sizeof(v));
            v.grade = (pc_grade_t)g;
            v.caps_applied = (uint8_t)caps;
            check_advice("census", g, pc_grade_advice(&v));
        }
    }
}

/* THE WHOLE VOCABULARY, NOT THE ONE STRING SOMEBODY NOTICED.
 *
 * The block above checks Watch's forgery reasons against the why band, and
 * states the principle: truncation is the right BEHAVIOUR for an unbounded
 * string and the wrong OUTCOME for a fixed vocabulary somebody chose. That
 * principle was being applied to exactly one table.
 *
 * A survey of every engine found seven more vocabularies with strings past
 * the narrowest field they are drawn in, including every one of the eleven
 * CWE references - and a CWE cut in half names a weakness the operator
 * cannot look up, which is worse than printing nothing.
 *
 * The caps below are not invented here. Each is either measured from the
 * layout, or is the precision the lens actually writes with, named at the
 * file and line that imposes it. Where a lens had slack - Vigil was cutting
 * at 13 in a row that holds 25 with a 9-character suffix - the precision was
 * widened rather than the words being butchered. */
static void test_style_every_vocabulary_fits_its_field(void)
{
    banner("style: every engine vocabulary fits the field it is drawn in");

    /* The why band, measured. Shared by prv_kind_note and pw_forgery_name. */
    const unsigned why_cap = ps_capacity(PS_TYPE_LABEL, PS_Y_WHY, PS_INNER_R);
    /* A detail row's two columns, taken from the type. */
    const size_t row_left  = sizeof(((struct pharos_lens_row *)0)->left) - 1u;
    const size_t row_right = sizeof(((struct pharos_lens_row *)0)->right) - 1u;

    CHECK(why_cap >= 20u, "the why band holds only %u characters", why_cap);
    CHECK(row_left == 25u && row_right == 11u,
          "the row columns are %u and %u", (unsigned)row_left,
          (unsigned)row_right);

    /* lens_roster.c: "%.11s" into right[12] - the "looks like" row. */
    for (int c = 0; c < RD_CLASS_COUNT; c++) {
        const char *n = rd_class_name((rd_class_t)c);
        CHECK(n != NULL, "class %d has no name", c);
        if (!n) continue;
        CHECK(strlen(n) <= row_right,
              "rd_class_name \"%s\" is %u, the column holds %u",
              n, (unsigned)strlen(n), (unsigned)row_right);
    }

    /* lens_roster.c: "%.25s" into left[26] - the CWE checklist rows. */
    for (int c = 0; c < RD_CLASS_COUNT; c++) {
        const char *cwes[8];
        const unsigned n = rd_class_cwes((rd_class_t)c, cwes,
                                         sizeof(cwes) / sizeof(cwes[0]));
        for (unsigned i = 0; i < n; i++) {
            CHECK(cwes[i] != NULL, "class %d cwe %u is NULL", c, i);
            if (!cwes[i]) continue;
            CHECK(strlen(cwes[i]) <= row_left,
                  "CWE line \"%s\" is %u, the row holds %u - a reference cut "
                  "in half cannot be looked up",
                  cwes[i], (unsigned)strlen(cwes[i]), (unsigned)row_left);
        }
    }

    /* lens_vigil.c: "%.16s" plus a 9-character address suffix in left[26]. */
    for (int k = 0; k < PV_KIND_COUNT; k++) {
        const char *n = pv_kind_name((pv_kind_t)k);
        CHECK(n != NULL, "vigil kind %d has no name", k);
        if (!n) continue;
        CHECK(strlen(n) <= 16u,
              "pv_kind_name \"%s\" is %u, the slot holds 16",
              n, (unsigned)strlen(n));
    }

    /* lens_rival.c: "%.13s %.11s" in left[26], and the why band. */
    for (int k = 0; k < PRV_KIND_COUNT; k++) {
        const char *n = prv_kind_name((prv_kind_t)k);
        const char *w = prv_kind_note((prv_kind_t)k);
        CHECK(n != NULL && w != NULL, "rival kind %d is missing a string", k);
        if (!n || !w) continue;
        CHECK(strlen(n) <= 13u,
              "prv_kind_name \"%s\" is %u, the slot holds 13",
              n, (unsigned)strlen(n));
        CHECK(strlen(w) <= why_cap,
              "prv_kind_note \"%s\" is %u, the why band holds %u",
              w, (unsigned)strlen(w), why_cap);
    }
}

void test_style(void)
{
    test_style_every_vocabulary_fits_its_field();
    test_style_advice_fits_the_advice_page();
    test_tone_limit_is_not_a_verdict();
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

        /* And the row of them has to stay inside THE GAUGE, not merely
         * inside the glass. Fitted against PR_SAFE_R the outer two chips were
         * drawn on top of the score arc. */
        const int16_t total = (int16_t)(w4 * 4 + PS_CARD_GAP * 3);
        const int16_t half = pr_chord_halfwidth(PS_INNER_R, PS_Y_CHIPS + PS_CARD_GAP);
        CHECK(total <= half * 2, "four chips overflow the chord at their own offset");
        CHECK(PS_INNER_R < PR_SAFE_R, "the gauge page's working radius must be inside the glass");

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
        const int16_t pad = 12, val_w = 112;
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
            /* WHICH COLUMN LOSES, WHEN ONE HAS TO.
             *
             * The narrowest card - the bottom one, where the chord has shrunk
             * toward the rim - has about 310 px for both columns. The value
             * takes 112 of it, because the value is the JUDGEMENT and it is
             * what somebody is scanning the page for; a grade that reads "A" 
             * when it meant "A+" is a wrong answer, not a cosmetic loss.
             *
             * So the LABEL is the column that degrades. 19 characters covers
             * every row the firmware currently produces ("popup models / advs"
             * and "hardware identified" are the longest, at 19); anything
             * longer gets an ellipsis, which is a visible, honest truncation
             * of a field that is only ever context. */
            CHECK(lab_w >= ps_text_w(PS_TYPE_LABEL, 19),
                  "row %u fits only %d px of label, needs 19 chars (%d px)",
                  i, (int)lab_w, (int)ps_text_w(PS_TYPE_LABEL, 19));
            /* The value column must draw the FULL 11-character contract -
             * see the place-name suite below for why this is not 8. */
            CHECK(val_w >= ps_text_w(PS_TYPE_LABEL, 11),
                  "the value column cannot hold the full right[] contract");
            /* The two columns plus padding may not exceed the card. */
            CHECK(lab_w + val_w + pad * 2 + 8 <= w,
                  "row %u columns overflow their card", i);
        }
    }

    banner("style: every word a lens can put in a row column actually fits");
    {
        /* THE CONTRACT IS ELEVEN CHARACTERS, AND THREE NAMES BROKE IT.
         *
         * struct pharos_lens_row.right[12] holds 11 printable characters.
         * pp_place_name() was returning "shop or cafe", "carrier hotspot" and
         * "unclassified" - 12, 15 and 12 - so snprintf silently cut them on
         * the way into the row, and the label then WRAPPED the remainder onto
         * a second line. On a PROBE page that read "unclassifi" with a lone
         * "e" floating outside the card.
         *
         * Checking the widest name against the column is the cheap half. The
         * valuable half is checking the WHOLE TABLE, because the next place
         * name somebody adds will be long too and nothing else would notice. */
        /* Taken from the type itself rather than written as 11, so the test
         * follows the contract if the contract ever changes. */
        const size_t cap = sizeof(((struct pharos_lens_row *)0)->right) - 1u;

        for (int p = 0; p <= PP_PLACE_HOME; p++) {
            const char *n = pp_place_name((pp_place_t)p);
            CHECK(n != NULL, "place %d has no name", p);
            if (!n) continue;
            CHECK(strlen(n) <= cap,
                  "place name \"%s\" is %u chars, the row column holds %u",
                  n, (unsigned)strlen(n), (unsigned)cap);
        }
        /* And the default arm, which is the one that was worst. */
        {
            const char *n = pp_place_name((pp_place_t)999);
            CHECK(strlen(n) <= cap,
                  "the fallback place name \"%s\" does not fit either", n);
        }

        /* The column has to be able to DRAW the full contract, or the fix
         * above just moves the truncation from snprintf into LVGL. */
        CHECK(112 >= ps_text_w(PS_TYPE_LABEL, (unsigned)cap),
              "the value column cannot draw %u characters at PS_TYPE_LABEL",
              (unsigned)cap);
    }

    banner("style: the why line's own vocabulary fits the why band");
    {
        /* The "why" line is the single most valuable string on a live face -
         * the one specific finding, as opposed to the band word's generality.
         * It sits at PS_Y_WHY inside the gauge, which is about 26 characters,
         * and pw_forgery_name() was returning 28 to 31. The cut landed on
         * exactly the word carrying the meaning: "sequence counter went
         * backwards" displayed as "sequence counter went."
         *
         * Truncation is the right BEHAVIOUR for an unbounded string and the
         * wrong OUTCOME for a fixed vocabulary somebody chose. So the
         * vocabulary is checked. */
        const unsigned cap = ps_capacity(PS_TYPE_LABEL, PS_Y_WHY, PS_INNER_R);
        CHECK(cap >= 20u, "the why band holds only %u characters", cap);

        for (int f = 0; f < 8; f++) {
            const char *n = pw_forgery_name((uint8_t)(1u << f));
            if (!n || !*n) continue;
            CHECK(strlen(n) <= cap,
                  "forgery reason \"%s\" is %u chars, the band holds %u",
                  n, (unsigned)strlen(n), cap);
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

    banner("style: the ceiling tick lives in the arc's own angle space");
    {
        /* THE TICK USED TO ADD PS_GAUGE_START A SECOND TIME.
         *
         * The arc carries the start as its LVGL rotation, so an angle handed
         * to it afterwards is already relative. Adding the start again moved
         * every ceiling marker 135 degrees - and since the arc opens at the
         * bottom, a mid-range ceiling landed in that opening, printed across
         * the advice line. Measured on the glass before the fix: a ceiling of
         * 56 drew at 62 degrees absolute when it belonged at 286.
         *
         * These assertions are what stop it being written as a sum again. */
        CHECK(ps_gauge_tick(0) == 0, "0%% must sit at the arc's own origin");
        CHECK(ps_gauge_tick(100) == PS_GAUGE_SWEEP,
              "100%% must sit at the far end of the sweep");
        CHECK(ps_gauge_tick(50) == PS_GAUGE_SWEEP / 2, "50%% must halve the sweep");

        /* The specific value that was wrong on the device. */
        CHECK(ps_gauge_tick(56) == 151, "a ceiling of 56 belongs at 151, got %d",
              (int)ps_gauge_tick(56));

        /* No output may carry the start offset: the whole range must stay
         * inside the sweep, which is the property the bug violated. */
        for (unsigned pct = 0; pct <= 100u; pct++) {
            const int16_t at = ps_gauge_tick((uint8_t)pct);
            CHECK(at >= 0 && at <= PS_GAUGE_SWEEP,
                  "tick for %u%% escaped the sweep at %d", pct, (int)at);
        }
        /* Out of range clamps rather than wrapping past the sweep. */
        CHECK(ps_gauge_tick(200) == PS_GAUGE_SWEEP, "an impossible pct must clamp");

        /* The opening at the bottom is what made the bug visible. Keep the
         * geometry that makes that true: a 270-degree sweep from 135 leaves
         * the bottom 90 degrees clear. */
        CHECK(PS_GAUGE_SWEEP == 270, "the sweep is no longer 270 degrees");
        CHECK(PS_GAUGE_START == 135, "the arc no longer opens at the bottom");
    }

    banner("style: wrapping the advice across the wide middle");
    {
        enum { NL = 7, SZ = 44 };
        char out[NL][SZ];
        /* Short text takes one line and leaves the rest cleared. */
        unsigned n = ps_wrap("Preserve the log.", 40, &out[0][0], NL, SZ);
        CHECK(n == 1, "a short line should use one line, used %u", n);
        CHECK(strcmp(out[0], "Preserve the log.") == 0, "got '%s'", out[0]);
        CHECK(out[1][0] == '\0', "unused lines must be cleared");

        /* It breaks on spaces, never mid-word, and no line exceeds the cap. */
        const char *real =
            "Radios here answer only for networks they also announce. That is "
            "what an ordinary access point does, so there is nothing to act on.";
        n = ps_wrap(real, 40, &out[0][0], NL, SZ);
        CHECK(n >= 3, "a 130-character string needs several lines, used %u", n);
        for (unsigned i = 0; i < n; i++) {
            CHECK(strlen(out[i]) <= 40, "line %u is %u chars, cap is 40", i,
                  (unsigned)strlen(out[i]));
            CHECK(out[i][0] != ' ', "line %u starts with a space", i);
        }
        /* Every word survives, in order: join the lines and compare. */
        char join[320];
        join[0] = '\0';
        for (unsigned i = 0; i < n; i++) {
            if (i) { strcat(join, " "); }
            strcat(join, out[i]);
        }
        CHECK(strcmp(join, real) == 0, "wrapping lost or reordered text:\n  '%s'",
              join);

        /* THE TAIL IS MARKED, NOT DROPPED. Showing half a sentence as though
         * it were the whole one is the bug this page exists to fix - it must
         * not come back in the page's own wrapper. */
        char small[2][12];
        n = ps_wrap("one two three four five six seven", 10, &small[0][0], 2, 12);
        CHECK(n == 2, "should fill both lines, used %u", n);
        CHECK(strstr(small[1], "..") != NULL,
              "an overflowing wrap must mark its last line, got '%s'", small[1]);
        for (unsigned i = 0; i < 2; i++) {
            CHECK(strlen(small[i]) <= 10, "line %u exceeded the cap: '%s'", i,
                  small[i]);
        }

        /* A word longer than a line is cut rather than allowed to run off the
         * circle, and the cap still holds. */
        n = ps_wrap("supercalifragilisticexpialidocious", 10, &small[0][0], 2, 12);
        CHECK(n > 0, "a single long word must still produce a line");
        CHECK(strlen(small[0]) <= 10, "a long word escaped the cap: '%s'",
              small[0]);

        /* Degenerate inputs must not write anything. */
        CHECK(ps_wrap(NULL, 40, &out[0][0], NL, SZ) == 0, "NULL text");
        CHECK(ps_wrap("", 40, &out[0][0], NL, SZ) == 0, "empty text");
        CHECK(ps_wrap("x", 0, &out[0][0], NL, SZ) == 0, "a zero cap");
        CHECK(ps_wrap("x", 40, &out[0][0], 0, SZ) == 0, "no lines");

        /* THE PAGE MUST HOLD WHAT THE CONTRACT CAN CARRY.
         *
         * Seven lines of forty is 280 against a 255-character field, but word
         * wrapping loses the ragged end of every line, so the arithmetic alone
         * proves nothing. Wrap a genuinely full field of realistic words and
         * require that NOTHING is marked as dropped. */
        {
            struct pharos_lens_display probe;
            char full[sizeof probe.advice];
            const char *w[] = { "the", "radio", "answered", "a", "network",
                                "name", "nobody", "announced", "which", "is",
                                "how", "a", "phone", "gets", "captured" };
            unsigned k = 0;
            full[0] = '\0';
            for (unsigned i = 0; k + 1u < sizeof full - 1u; i++) {
                const char *word = w[i % (sizeof w / sizeof w[0])];
                const unsigned wl = (unsigned)strlen(word);
                if (k + wl + 2u >= sizeof full) { break; }
                if (k) { full[k++] = ' '; }
                memcpy(full + k, word, wl);
                k += wl;
                full[k] = '\0';
            }
            CHECK(k > sizeof(probe.advice) - 12u,
                  "the probe string should nearly fill the field, got %u", k);

            char page[7][44];
            const unsigned used = ps_wrap(full, 40, &page[0][0], 7, 44);
            CHECK(used > 0, "a full field must wrap to something");
            CHECK(strstr(page[used - 1u], "..") == NULL,
                  "a full advice field did not fit the page - last line '%s'",
                  page[used - 1u]);
        }
    }

    banner("style: text that is not on the axis must be fitted where it sits");
    {
        /* THE DETAIL PAGE'S COLUMN HEADERS ARE DRAWN AT -72 AND +72.
         *
         * Fitted with the on-axis capacity, a header using its full allowance
         * at dy=-164 spans -143..+143; centred at -72 that reaches -215, a
         * radius of 275 on a panel that stops at 224. Measured on the glass:
         * the left header ran off the edge on four different detail pages. */
        const int16_t DY = -164, DX = -72;

        const unsigned on_axis = ps_capacity(PS_TYPE_MICRO, DY, PR_SAFE_R);
        const unsigned at_dx = ps_capacity_at(PS_TYPE_MICRO, DX, DY, PR_SAFE_R);
        CHECK(at_dx < on_axis,
              "an off-axis box must get LESS room than an on-axis one (%u vs %u)",
              at_dx, on_axis);

        /* The property that actually matters: a string of exactly the allowed
         * length, centred at dx, keeps all four corners inside the glass. */
        for (int16_t dx = -140; dx <= 140; dx = (int16_t)(dx + 20)) {
            const unsigned cap = ps_capacity_at(PS_TYPE_MICRO, dx, DY, PR_SAFE_R);
            if (cap == 0u) {
                continue; /* refused outright, which is also safe */
            }
            const int16_t w = ps_text_w(PS_TYPE_MICRO, cap);
            const int16_t h = PS_TYPE_PX[PS_TYPE_MICRO];
            const int16_t x0 = (int16_t)(dx - w / 2), x1 = (int16_t)(dx + w / 2);
            const int16_t y0 = (int16_t)(DY - h / 2), y1 = (int16_t)(DY + h / 2);
            const int16_t cx[4] = { x0, x1, x0, x1 };
            const int16_t cy[4] = { y0, y0, y1, y1 };
            for (unsigned k = 0; k < 4u; k++) {
                const long rr = (long)cx[k] * cx[k] + (long)cy[k] * cy[k];
                CHECK(rr <= (long)PR_SAFE_R * PR_SAFE_R,
                      "a full-width header at dx=%d escaped: corner r=%d",
                      (int)dx, (int)(long)(rr > 0 ? (long)(rr) : 0) / 1000);
            }
        }

        /* On the axis the two must agree exactly - this is a generalisation,
         * not a replacement. */
        for (int16_t dy = -200; dy <= 200; dy = (int16_t)(dy + 25)) {
            CHECK(ps_capacity_at(PS_TYPE_LABEL, 0, dy, PR_SAFE_R) ==
                  ps_capacity(PS_TYPE_LABEL, dy, PR_SAFE_R),
                  "on-axis capacity disagreed with itself at dy=%d", (int)dy);
        }


        /* EVERY DETAIL HEADER MUST FIT WHERE IT IS DRAWN.
         *
         * At dx=-72, dy=-164 the glass allows sixteen characters at
         * PS_TYPE_MICRO. Six headers in the tree were longer than that, and
         * four of them were measured running off the panel on real captures.
         * They were shortened rather than truncated - the backstop exists for
         * a string nobody anticipated, not as a licence to write long ones. */
        {
            const unsigned hdr = ps_capacity_at(PS_TYPE_MICRO, -72, -164,
                                                PR_SAFE_R);
            CHECK(hdr >= 16u, "the header slot shrank to %u characters", hdr);
            /* The longest header shipped, as a literal, so adding a longer one
             * fails here rather than on the glass. */
            CHECK(strlen("CHANNEL ACTIVITY") <= hdr,
                  "the longest detail header no longer fits (%u allowed)", hdr);
        }
        /* A box pushed past the chord gets nothing rather than a negative. */
        CHECK(ps_capacity_at(PS_TYPE_MICRO, 300, 0, PR_SAFE_R) == 0,
              "a box outside the circle must get no room");
    }
}
