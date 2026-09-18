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

/* THE STAGGER IS GONE, AND ITS ABSENCE IS THE CONTRACT NOW.
 *
 * Two radii were a way to fit more names by pulling alternate labels toward
 * the middle. On hardware that reads as a misaligned dial - and at the counts
 * where it mattered it was also putting labels over the headline. When the
 * circle cannot carry every name, it carries fewer. */
static void test_ring_never_staggers(void)
{
    banner("ring: it reduces the count rather than moving labels inward");
    pd_ring_t r;

    for (unsigned n = 2; n <= 16; n++) {
        pd_ring_layout(n, LBL_W, LBL_H, 12, &r);
        CHECK(!r.staggered, "%u labels stay on one radius", n);
    }

    /* At the counts that used to stagger, the honest answer is a smaller
     * capacity - and it must still be a useful number, not zero. */
    pd_ring_layout(13, LBL_W, LBL_H, 12, &r);
    CHECK(r.r_even == r.r_odd, "thirteen is still a circle");
    CHECK(r.capacity >= 8u, "and still carries a useful number of names (%u)",
          r.capacity);
    CHECK(r.capacity <= 13u, "without claiming more than it was asked for");
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
    /* MEASURED ON THE ARC THE RING IS ACTUALLY DRAWN ON.
     *
     * These used to read { 8, 38 } ... { 12, 12 }, and they were measured
     * against a layout that spread labels evenly round a FULL circle. The home
     * ring follows the gauge's 270 degrees and leaves the bottom notch for the
     * clock, so it has three quarters of the circumference and the same names
     * are correspondingly tighter. The old table was not a stricter standard
     * that we have relaxed; it was a measurement of a dial that does not
     * exist. */
    struct { unsigned n; int16_t at_least; } want[] = {
        { 6,  60 }, { 7,  36 }, { 8,  26 }, { 9,  16 }, { 10, 6 },
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
    /* Comfortable spacing is available up to NINE names on this arc, not
     * twelve. Past that the layout reports a capacity below the count, which
     * is the honest answer - the caller then names the watches that matter and
     * leaves the rest as bare dots. */
    for (unsigned n = 2; n <= 9; n++) {
        pd_ring_t r;
        pd_ring_layout(n, LBL_W, LBL_H, LBL_GAP, &r);
        CHECK(r.gap_px >= LBL_GAP, "%u watches are comfortably spaced", n);
    }

    /* THE INVARIANT THAT MATTERS, AND IT IS MODEL-INDEPENDENT.
     *
     * Whatever the arc, whatever the count: every arrangement the layout
     * declares usable must actually be usable - no two label boxes touching,
     * none through the headline, none off the glass. The numbers above are
     * calibration and may move again; this may not. */
    for (unsigned n = 2; n <= 16; n++) {
        pd_ring_t r;
        pd_ring_layout(n, LBL_W, LBL_H, LBL_GAP, &r);
        if (r.capacity >= 2u) {
            CHECK(pd_ring_fits(&r, r.capacity, LBL_W, LBL_H, LBL_GAP),
                  "%u watches: the %u it says it can name really do fit",
                  n, r.capacity);
        }
    }

    /* And the honest upper bound: past twelve the dial is genuinely full, so
     * the default must not be set there and the operator adding a thirteenth
     * is making a knowing trade. */
    /* `gap_px` now describes the labels that will ACTUALLY BE DRAWN rather
     * than a hypothetical full set - once the layout has decided to name only
     * some of them, the spacing it reports is theirs. So the honest test of
     * "thirteen is too many" is that it refuses to name all thirteen, and that
     * the subset it does name is properly spaced. */
    pd_ring_t r13;
    pd_ring_layout(13, LBL_W, LBL_H, LBL_GAP, &r13);
    CHECK(r13.capacity < 13u,
          "thirteen is more names than the arc carries (names %u)",
          r13.capacity);
    CHECK(r13.capacity >= 2u, "but it still names what it can");
    CHECK(pd_ring_fits(&r13, r13.capacity, LBL_W, LBL_H, LBL_GAP),
          "and the ones it names are drawn without overlapping");
}

/* THE SWEEP: EVERY COUNT THE OPERATOR CAN CONFIGURE, CHECKED.
 *
 * Asked to "start with one sensor, check the text, then two, then three" -
 * which is exactly the right method and a slow one through a camera. It is
 * arithmetic, so it runs here instead, at every count, on every build.
 *
 * It found the bug it was written for. The layout searched for the widest
 * space BETWEEN labels and had no opinion about the middle of the dial, so it
 * bought elbow room by pushing the inner ring onto the headline: at eleven,
 * thirteen, fourteen and fifteen watches the labels sat 29 px INSIDE the core,
 * and on the glass that read as CENSUS and SQUALL written through "worth a
 * look". Nothing caught it because nothing was looking there. */
static void test_ring_sweep_every_count(void)
{
    banner("ring: every count from 2 to 16 is checked for text collisions");

    for (unsigned n = 2; n <= 16u; n++) {
        pd_ring_t g;
        pd_ring_layout(n, PD_RING_LABEL_W, 14, 12, &g);

        for (unsigned i = 0; i < n; i++) {
            const float step = 360.0f / (float)n;
            const float a = -90.0f + step * (float)i;
            const float rad = (float)pd_ring_label_r(&g, i);
            const float cx = rad * cosf(a * 3.14159265f / 180.0f);
            const float cy = rad * sinf(a * 3.14159265f / 180.0f);
            const float x0 = cx - (float)PD_RING_LABEL_W / 2.0f;
            const float x1 = cx + (float)PD_RING_LABEL_W / 2.0f;
            const float y0 = cy - 7.0f;
            const float y1 = cy + 7.0f;

            /* Clear of the headline. This is the one that was failing. */
            const float nx = (x0 > 0.0f) ? x0 : ((x1 < 0.0f) ? -x1 : 0.0f);
            const float ny = (y0 > 0.0f) ? y0 : ((y1 < 0.0f) ? -y1 : 0.0f);
            CHECK(sqrtf(nx * nx + ny * ny) >= (float)PD_RING_CORE_R,
                  "n=%u label %u clears the core", n, i);

            /* And on the glass. */
            const float far = sqrtf((x1 > -x0 ? x1 : -x0) * (x1 > -x0 ? x1 : -x0) +
                                    (y1 > -y0 ? y1 : -y0) * (y1 > -y0 ? y1 : -y0));
            CHECK(far <= (float)PR_SAFE_R, "n=%u label %u stays on the glass", n, i);
        }

        /* And the capacity it reports must be honest: labelling that many at
         * one radius really does clear the requested gap. */
        CHECK(g.capacity <= n, "n=%u: capacity never exceeds the count", n);
        if (g.capacity >= 2u) {
            pd_ring_t c;
            pd_ring_layout(g.capacity, PD_RING_LABEL_W, 14, 12, &c);
            CHECK(pd_ring_fits(&c, g.capacity, PD_RING_LABEL_W, 14, 12),
                  "n=%u: the %u it claims to carry genuinely fit", n,
                  g.capacity);
        }
    }
}

/* THE RING IS A CIRCLE, AND EVERY DOT ON IT IS NAMED.
 *
 * Two things were reported from a photograph and both were real. Alternating
 * radii bought clear space by pulling every other label inward, which reads as
 * misalignment rather than cleverness - a dial's labels belong on a circle.
 * And sizing the ring for the longest name in the project rather than the
 * longest name ON IT cost one label at eleven watches, leaving a dot with no
 * name among dots that had them, which reads as a fault. */
static void test_ring_is_a_circle(void)
{
    banner("ring: one radius, always - a dial's labels belong on a circle");
    for (unsigned n = 2; n <= 16u; n++) {
        pd_ring_t g;
        pd_ring_layout(n, PD_RING_LABEL_W, 14, 12, &g);
        CHECK(!g.staggered, "n=%u sits on a single radius", n);
        CHECK(g.r_even == g.r_odd, "n=%u: both radii agree", n);
    }
}

static void test_ring_names_the_default_set(void)
{
    banner("ring: the watches armed by default all get a name");
    /* HARVEST, seven characters, is the longest of the eleven armed out of the
     * box - not FOOTPRINT, which is off by default. Sizing for the project's
     * longest name instead of the ring's cost a label. */
    /* Sized at the face the names are drawn in, PS_TYPE_LABEL. The old
     * numbers used 7.6 px per character, which belongs to a 12 px font. */
    /* PD_RING_NAME_MAX is the contract: the ring truncates to seven, so seven
     * is the width the layout is entitled to assume. */
    const int16_t harvest_w = (int16_t)(PD_RING_NAME_MAX * 10 + 6);
    CHECK(pd_ring_capacity(harvest_w, 16, 12) >= 8u,
          "eight seven-character names fit (capacity %u)",
          pd_ring_capacity(harvest_w, 16, 12));

    /* AND THE REASON THE CONTRACT EXISTS.
     *
     * Untruncated, a nine-character name can be placed at only two of the
     * eleven positions once the headline's footprint is respected - which is
     * why the ring truncates rather than pretending otherwise. This asserts
     * the gap between the two, so nobody quietly raises PD_RING_NAME_MAX
     * without discovering what it costs. */
    const int16_t longest_w = (int16_t)(9 * 10 + 6);
    const unsigned untruncated = pd_ring_capacity(longest_w, 16, 12);
    CHECK(untruncated < pd_ring_capacity(harvest_w, 16, 12),
          "nine characters would cost capacity (%u vs %u)",
          untruncated, pd_ring_capacity(harvest_w, 16, 12));
}

void test_ring(void)
{
    test_ring_sweep_every_count();
    test_ring_is_a_circle();
    test_ring_names_the_default_set();
    test_ring_spacing_at_each_count();
    test_ring_every_count_fits();
    test_ring_never_staggers();
    test_ring_catches_the_shipped_bug();
    test_ring_labels_stay_on_the_glass();
    test_ring_degenerate();
}
