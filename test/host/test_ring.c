/* Host tests for the home ring's label layout.
 *
 * Photographing the screen to find out whether the names fit is a slow and
 * unreliable way to answer a question that is pure arithmetic - and it was the
 * only way this had been checked, which is why thirteen labels shipped
 * overlapping. Two of them ran into each other and read as one long word.
 *
 * The interesting part is WHERE they collided. Spacing along the arc was fine;
 * the text is horizontal, so the pair nearest the top of the dial and the pair
 * nearest the bottom end up side by side with only the arc gap between them.
 * A layout that is checked only at "does it look about right" will pass that
 * every time.
 */
#include "pharos_dial.h"
#include "pharos_round.h"
#include "test_support.h"

/* The widest a ring label can be. Seven characters of montserrat_12: the
 * uppercase advance runs to about 8.8 px on the wide letters, so 62 px is the
 * honest worst case rather than the average one. Measuring with the average
 * is how a layout passes arithmetic and fails on the glass. */
#define LBL_W 62
#define LBL_H 14

/* Clear space required between two labels. Not overlapping is not the same as
 * legible: thirteen names at one radius cleared each other by five pixels and
 * read as a single long word. */
#define LBL_GAP 12

static void test_ring_every_count_fits(void)
{
    banner("ring: every count the operator can configure lays out cleanly");

    /* The ring is something the operator edits, so this has to hold for every
     * count it can be left at - not merely for whatever ships as the default. */
    for (unsigned n = 2; n <= 16; n++) {
        pd_ring_t r;
        pd_ring_layout(n, LBL_W, LBL_H, LBL_GAP, &r);
        CHECK(r.r_even >= 100 && r.r_even <= 156, "%u: even radius sane", n);
        CHECK(r.r_odd >= 100 && r.r_odd <= 156, "%u: odd radius sane", n);
        /* Labels sit inside the dots, always - a label drawn outside them
         * would be cut by the bezel on the left and right of the circle. */
        CHECK(r.r_even < r.r_dot && r.r_odd < r.r_dot,
              "%u: labels stay inside the dot ring", n);
    }
}

/* A tidy ring is preferable; two radii are a cost paid only when needed. */
static void test_ring_prefers_one_radius(void)
{
    banner("ring: it only staggers when a single radius will not do");
    pd_ring_t r;

    for (unsigned n = 2; n <= 7; n++) {
        pd_ring_layout(n, LBL_W, LBL_H, LBL_GAP, &r);
        CHECK(!r.staggered, "%u labels sit on one radius", n);
    }

    /* And the count that was actually shipping broken does need the stagger. */
    pd_ring_layout(13, LBL_W, LBL_H, LBL_GAP, &r);
    CHECK(r.staggered, "thirteen needs two radii");
    CHECK(r.r_even != r.r_odd, "which are genuinely different");
}

/* THE BUG, STATED AS A TEST. The old layout put every label on one radius at
 * 140 px; at thirteen watches that overlapped, and nothing said so. */
static void test_ring_catches_the_shipped_bug(void)
{
    banner("ring: the layout that shipped is provably an overlap");
    pd_ring_t broken = { .r_even = 140, .r_odd = 140, .r_dot = 168,
                         .staggered = false };
    CHECK(!pd_ring_fits(&broken, 13, LBL_W, LBL_H, LBL_GAP),
          "thirteen labels on one 140 px radius are too close to read");
    /* Eight on the same radius were fine, which is why it was not noticed
     * until more watches were added. */
    CHECK(pd_ring_fits(&broken, 8, LBL_W, LBL_H, LBL_GAP),
          "eight on the same radius did not, which is how it got through");
}

static void test_ring_labels_stay_on_the_glass(void)
{
    banner("ring: no label corner leaves the safe radius");
    /* A label centred inside the safe radius can still have a corner outside
     * it - the check has to be on the box, not the anchor. */
    pd_ring_t wide = { .r_even = 210, .r_odd = 210, .r_dot = 168,
                       .staggered = false };
    CHECK(!pd_ring_fits(&wide, 4, LBL_W, LBL_H, LBL_GAP),
          "a radius that puts corners past the bezel is refused");

    for (unsigned n = 2; n <= 16; n++) {
        pd_ring_t r;
        pd_ring_layout(n, LBL_W, LBL_H, LBL_GAP, &r);
        const int16_t far = (r.r_even > r.r_odd) ? r.r_even : r.r_odd;
        /* Worst case is a label at the far left or right, where its own width
         * pushes the corner outward. */
        const double corner_x = (double)far + (double)LBL_W / 2.0;
        CHECK(corner_x <= (double)PR_SAFE_R,
              "%u: the widest label still clears the bezel", n);
    }
}

static void test_ring_degenerate(void)
{
    banner("ring: nonsense in, something sane out");
    pd_ring_t r;
    pd_ring_layout(0, LBL_W, LBL_H, LBL_GAP, &r);
    CHECK(r.r_dot > 0, "a ring of nothing still has a sane radius");
    pd_ring_layout(1, LBL_W, LBL_H, LBL_GAP, &r);
    CHECK(pd_ring_fits(&r, 1, LBL_W, LBL_H, LBL_GAP), "one label cannot collide");
    pd_ring_layout(2, LBL_W, LBL_H, LBL_GAP, &r);
    CHECK(pd_ring_label_r(&r, 0) == r.r_even, "even items take the even radius");
    CHECK(pd_ring_label_r(&r, 1) == r.r_odd, "odd items take the odd one");
    CHECK(pd_ring_label_r(NULL, 0) == 0, "a NULL layout is survivable");
    CHECK(pd_ring_fits(NULL, 4, LBL_W, LBL_H, LBL_GAP), "and so is a NULL check");
}

/* THE NUMBER THAT DECIDES THE SHIPPED DEFAULT.
 *
 * "Make it nicely packed like it was before" is a real requirement and it has
 * a number behind it: how much clear space there is between two names. Eight
 * watches leave 60-odd pixels, which is the ring that looked right. Thirteen
 * leave ten, which reads as one long word. The default is chosen from this
 * table rather than from a photograph. */
static void test_ring_spacing_at_each_count(void)
{
    banner("ring: how much room each count actually leaves");
    /* What the layout actually delivers. It stops searching once a single
     * radius clears the requirement - a tidy ring beats a marginally wider
     * gap - so these are the chosen arrangement, not the theoretical best. */
    struct { unsigned n; int16_t at_least; } want[] = {
        { 8,  38 }, { 9,  29 }, { 10, 21 }, { 11, 14 }, { 12, 12 },
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        pd_ring_t r;
        pd_ring_layout(want[i].n, LBL_W, LBL_H, LBL_GAP, &r);
        CHECK(r.gap_px >= want[i].at_least,
              "%u watches leave at least %dpx (got %d)", want[i].n,
              (int)want[i].at_least, (int)r.gap_px);
    }

    /* Twelve is the last count that reaches comfortable spacing, and it only
     * just does. Ten leaves half as much again, which is why the shipped
     * default is ten and not thirteen. */
    for (unsigned n = 2; n <= 12; n++) {
        pd_ring_t r;
        pd_ring_layout(n, LBL_W, LBL_H, LBL_GAP, &r);
        CHECK(r.gap_px >= LBL_GAP, "%u watches are comfortably spaced", n);
    }

    /* And the honest upper bound: past twelve the dial is genuinely full, so
     * the default must not be set there and the operator adding a thirteenth
     * is making a knowing trade. */
    pd_ring_t r13;
    pd_ring_layout(13, LBL_W, LBL_H, LBL_GAP, &r13);
    CHECK(r13.gap_px < LBL_GAP,
          "thirteen cannot reach comfortable spacing (%d px)", (int)r13.gap_px);
    CHECK(r13.gap_px > 0, "but it is still drawn without overlapping");
}

void test_ring(void)
{
    test_ring_spacing_at_each_count();
    test_ring_every_count_fits();
    test_ring_prefers_one_radius();
    test_ring_catches_the_shipped_bug();
    test_ring_labels_stay_on_the_glass();
    test_ring_degenerate();
}
