/* Host tests for the session Survey.
 *
 * The failures worth testing here are all about honesty of counting: an
 * address counted twice inflates a number somebody will quote, a table that
 * silently fills up under-reports a busy place, and a summary that calls a
 * badly configured neighbour an attack is the scare-sheet failure this engine
 * exists to avoid.
 */
#include <string.h>

#include "pharos_census.h"
#include "pharos_survey.h"
#include "test_support.h"

#define T0 1000000000ull
#define MIN 60000000ull

static void mac(uint8_t out[6], uint8_t last)
{
    out[0] = 0x02; out[1] = 0x11; out[2] = 0x22;
    out[3] = 0x33; out[4] = 0x44; out[5] = last;
}

static void test_survey_counts_addresses_once(void)
{
    banner("survey: a network heard fifty times is one network");
    psv_t s;
    psv_reset(&s, T0);
    uint8_t a[6];
    mac(a, 1);

    for (unsigned i = 0; i < 50; i++) {
        psv_note_network(&s, a, 3, PSV_NET_NO_MFP, T0 + i * 1000000ull);
    }
    psv_report_t r;
    psv_summarise(&s, T0 + MIN, &r);
    CHECK_EQ(r.networks, 1);
    CHECK_EQ(r.no_mfp, 1);

    /* A second, different network. */
    uint8_t b[6];
    mac(b, 2);
    psv_note_network(&s, b, 1, PSV_NET_MODERN, T0);
    psv_summarise(&s, T0 + MIN, &r);
    CHECK_EQ(r.networks, 2);
    CHECK_EQ(r.no_mfp, 1);
    CHECK_EQ(r.modern, 1);
}

/* A network cannot talk its way to a better grade by being heard again on a
 * sweep that happened to miss its RSN element. */
static void test_survey_keeps_the_worst_view(void)
{
    banner("survey: the worst thing seen about a network is what stands");
    psv_t s;
    psv_reset(&s, T0);
    uint8_t a[6];
    mac(a, 1);

    /* THIS TEST USED TO ENCODE THE BUG.
     *
     * It passed 5 then 1, called the 1 "a rosier later look", and asserted the
     * engine kept the 5. But these are pc_grade_t values, which count UPWARD to
     * better - 1 is F and 5 is B - so the "rosier" look was the network getting
     * WORSE, and the assertion was that the engine discards bad news.
     *
     * The engine agreed with the test, the test agreed with the engine, and
     * between them they shipped a Survey page that displayed every grade
     * inverted: an open network as a green A. A test written from the same
     * misunderstanding as the code cannot catch it, which is the whole reason
     * this now spells the grades out by name. */
    psv_note_network(&s, a, PC_GRADE_B, PSV_NET_OPEN, T0);
    psv_note_network(&s, a, PC_GRADE_A_PLUS, 0, T0 + 1000000ull);

    psv_report_t r;
    psv_summarise(&s, T0 + MIN, &r);
    CHECK_EQ(r.networks, 1);
    CHECK(r.worst_grade == PC_GRADE_B,
          "a genuinely rosier later look does not raise the grade");
    CHECK_EQ(r.open, 1);

    /* And hearing something WORSE does lower it - new evidence, not a mood. */
    psv_note_network(&s, a, PC_GRADE_F, 0, T0 + 1500000ull);
    psv_summarise(&s, T0 + MIN, &r);
    CHECK(r.worst_grade == PC_GRADE_F, "and worse news is taken");

    /* And flags accumulate rather than replace: two sweeps that each saw one
     * fault should end with both faults known. */
    psv_note_network(&s, a, PC_GRADE_F, PSV_NET_WPS, T0 + 2000000ull);
    psv_summarise(&s, T0 + MIN, &r);
    CHECK_EQ(r.open, 1);
    CHECK_EQ(r.wps, 1);
}

static void test_survey_admits_its_limit(void)
{
    banner("survey: a full table says so rather than under-counting quietly");
    psv_t s;
    psv_reset(&s, T0);
    uint8_t a[6];
    for (unsigned i = 0; i < PSV_MAX_NETWORKS + 20u; i++) {
        a[0] = 0x02; a[1] = 0x11; a[2] = 0x22;
        a[3] = (uint8_t)(i >> 8); a[4] = (uint8_t)i; a[5] = 0x01;
        psv_note_network(&s, a, 2, PSV_NET_NO_MFP, T0);
    }
    psv_report_t r;
    psv_summarise(&s, T0 + MIN, &r);
    CHECK_EQ(r.networks, PSV_MAX_NETWORKS);
    CHECK(r.truncated, "and it says the count is a floor");

    char buf[64];
    bool said = false;
    for (unsigned i = 0; i < 16 && psv_line(&r, i, buf, sizeof(buf)); i++) {
        if (strstr(buf, "minimum")) {
            said = true;
        }
    }
    CHECK(said, "the plain-English lines carry the caveat too");
}

/* THE SCARE-SHEET TEST. Most of what this engine counts is somebody else's
 * badly configured network, which is not an attack and must never be worded
 * as one. */
static void test_survey_never_calls_a_neighbour_an_attacker(void)
{
    banner("survey: a weak network is not an attack, and is not worded as one");
    psv_t s;
    psv_reset(&s, T0);
    uint8_t a[6];
    for (unsigned i = 0; i < 20; i++) {
        mac(a, (uint8_t)i);
        psv_note_network(&s, a, 5, PSV_NET_OPEN | PSV_NET_NO_MFP | PSV_NET_WPS,
                         T0);
    }
    psv_report_t r;
    psv_summarise(&s, T0 + MIN, &r);

    static const char *k_banned[] = {
        "attack", "attacking", "under attack", "intrusion", "hacked",
        "compromised", "breach", "malicious",
    };
    char buf[64];
    for (unsigned i = 0; i < 16 && psv_line(&r, i, buf, sizeof(buf)); i++) {
        for (unsigned b = 0; b < sizeof(k_banned) / sizeof(k_banned[0]); b++) {
            CHECK(strstr(buf, k_banned[b]) == NULL,
                  "line %u does not use the word \"%s\"", i, k_banned[b]);
        }
    }
    CHECK(strstr(r.headline, "attack") == NULL,
          "and neither does the headline");

    /* EVERY LINE HAS TO FIT THE GLASS, and the bound has to be the real one.
     *
     * A detail row's left column is char[26] - twenty-five characters and a
     * terminator. This test originally allowed thirty-three, and the slack hid
     * a live truncation: "2 would drop clients if flooded" reached the screen
     * as "2 would drop clients if f", which is not plain English, it is a
     * sentence cut in half.
     *
     * Checked with three-digit counts, because a busy place produces them and
     * the length that matters is the longest one that can occur. */
    for (unsigned i = 0; i < 16 && psv_line(&r, i, buf, sizeof(buf)); i++) {
        CHECK(strlen(buf) <= 25, "line %u fits a detail row: \"%s\"", i, buf);
    }
}

/* The longest each line can ever be, which is with counts in three digits. */
static void test_survey_lines_fit_at_full_scale(void)
{
    banner("survey: every line still fits when the numbers get big");
    psv_t s;
    psv_reset(&s, T0);
    uint8_t a[6];
    for (unsigned i = 0; i < PSV_MAX_NETWORKS; i++) {
        a[0] = 0x02; a[1] = 0x11; a[2] = 0x22;
        a[3] = (uint8_t)(i >> 8); a[4] = (uint8_t)i; a[5] = 0x01;
        psv_note_network(&s, a, 5,
                         PSV_NET_OPEN | PSV_NET_NO_MFP | PSV_NET_WPS |
                             PSV_NET_WEAK | PSV_NET_MODERN,
                         T0);
    }
    for (unsigned i = 0; i < PSV_MAX_DEVICES; i++) {
        a[0] = 0x06; a[3] = (uint8_t)(i >> 8); a[4] = (uint8_t)i; a[5] = 0x02;
        psv_note_device(&s, a, 255, true, T0);
    }
    for (unsigned i = 1; i <= PSV_MAX_TOOLS; i++) {
        psv_note_tool(&s, (uint8_t)i, true, T0);
    }
    psv_report_t r;
    psv_summarise(&s, T0 + 999ull * MIN, &r);

    char buf[64];
    unsigned lines = 0;
    for (unsigned i = 0; i < 16 && psv_line(&r, i, buf, sizeof(buf)); i++) {
        CHECK(strlen(buf) <= 25, "full-scale line %u fits: \"%s\"", i, buf);
        lines++;
    }
    CHECK(lines >= 8, "and a busy place produces a full page of them");
}

static void test_survey_reports_good_news(void)
{
    banner("survey: it reports what is right, not only what is wrong");
    psv_t s;
    psv_reset(&s, T0);
    uint8_t a[6];
    for (unsigned i = 0; i < 5; i++) {
        mac(a, (uint8_t)i);
        psv_note_network(&s, a, 0, PSV_NET_MODERN, T0);
    }
    psv_report_t r;
    psv_summarise(&s, T0 + MIN, &r);
    CHECK_EQ(r.modern, 5);
    CHECK_EQ(r.no_mfp, 0);
    CHECK(strstr(r.headline, "nothing alarming") != NULL,
          "a well-run neighbourhood is told it is one");

    char buf[64];
    bool good = false;
    for (unsigned i = 0; i < 16 && psv_line(&r, i, buf, sizeof(buf)); i++) {
        if (strstr(buf, "WPA3")) {
            good = true;
        }
    }
    CHECK(good, "and the good news gets a line of its own");
}

static void test_survey_devices_and_tools(void)
{
    banner("survey: leaky devices and hardware that came and went");
    psv_t s;
    psv_reset(&s, T0);
    uint8_t d[6];

    mac(d, 0x10);
    psv_note_device(&s, d, 4, true, T0);
    psv_note_device(&s, d, 4, true, T0 + 1000000ull); /* same device again */
    mac(d, 0x11);
    psv_note_device(&s, d, 7, false, T0);
    /* A device that named nothing is not a leaky device. */
    mac(d, 0x12);
    psv_note_device(&s, d, 0, true, T0);

    psv_report_t r;
    psv_summarise(&s, T0 + MIN, &r);
    CHECK_EQ(r.devices, 2);
    CHECK_EQ(r.names_leaked, 11);
    CHECK_EQ(r.trackable, 1);

    /* Hardware: seen, then gone. The survey remembers it was here, which is
     * the whole reason a session log beats a live reading. */
    psv_note_tool(&s, 6 /* Flipper */, true, T0);
    psv_summarise(&s, T0 + MIN, &r);
    CHECK_EQ(r.tools, 1);
    CHECK_EQ(r.tools_present, 1);
    CHECK(strstr(r.headline, "pentest") != NULL, "and it leads the headline");

    psv_note_tool(&s, 6, false, T0 + 2ull * MIN);
    psv_summarise(&s, T0 + 3ull * MIN, &r);
    CHECK_EQ(r.tools, 1);
    CHECK_EQ(r.tools_present, 0);

    char buf[64];
    bool earlier = false;
    for (unsigned i = 0; i < 16 && psv_line(&r, i, buf, sizeof(buf)); i++) {
        if (strstr(buf, "was here") || strstr(buf, "earlier")) {
            earlier = true;
        }
    }
    CHECK(earlier, "a tool that left is reported in the past tense");
}

static void test_survey_empty_and_null(void)
{
    banner("survey: an empty survey says so and survives NULLs");
    psv_t s;
    psv_reset(&s, T0);
    psv_report_t r;
    psv_summarise(&s, T0, &r);
    CHECK_EQ(r.networks, 0);
    CHECK(strstr(r.headline, "nothing surveyed") != NULL, "it says so");

    char buf[64];
    CHECK(psv_line(&r, 0, buf, sizeof(buf)), "and offers one line of guidance");
    CHECK(strstr(buf, "listening") != NULL, "which says what is happening");
    CHECK(!psv_line(&r, 1, buf, sizeof(buf)), "and only the one");

    psv_summarise(NULL, T0, &r);
    CHECK_EQ(r.networks, 0);
    psv_note_network(NULL, NULL, 0, 0, T0);
    psv_note_device(NULL, NULL, 0, false, T0);
    psv_note_tool(NULL, 0, false, T0);
    CHECK(!psv_line(NULL, 0, buf, sizeof(buf)), "a NULL report yields nothing");
}

/* THE WORD HAS TO MATCH THE NUMBER.
 *
 * The headline said "most networks are floodable" when one network in four
 * was. The count was right on the line underneath, which makes it worse: a
 * device that overstates in the headline and corrects itself in the detail is
 * one nobody reads the headline of twice. */
static void test_survey_quantifiers_are_honest(void)
{
    banner("survey: \"most\" means most, and \"every\" means every");
    uint8_t a[6];

    struct { unsigned bad, total; const char *want; } cases[] = {
        { 1, 4,  "some" },
        { 2, 4,  "some" },   /* exactly half is not most */
        { 3, 4,  "most" },
        { 4, 4,  "every" },
        { 1, 10, "some" },
        { 9, 10, "most" },
    };

    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        psv_t s;
        psv_reset(&s, T0);
        for (unsigned i = 0; i < cases[c].total; i++) {
            mac(a, (uint8_t)i);
            psv_note_network(&s, a, 2,
                             (i < cases[c].bad) ? PSV_NET_NO_MFP : PSV_NET_MODERN,
                             T0);
        }
        psv_report_t r;
        psv_summarise(&s, T0 + MIN, &r);
        CHECK_EQ(r.no_mfp, cases[c].bad);
        CHECK(strstr(r.headline, cases[c].want) != NULL,
              "%u of %u floodable reads as \"%s\", got \"%s\"", cases[c].bad,
              cases[c].total, cases[c].want, r.headline);
    }
}

/* THE GRADE ORDERING, WHICH WAS EXACTLY BACKWARDS.
 *
 * pc_grade_t counts UPWARD to better: UNGRADED, F, E, D, C, B, A, A+. The
 * survey engine kept the LARGER value under a comment promising it kept the
 * worst, so "worst_grade" was the best grade in the room - and the Survey page
 * then rendered it through a hand-rolled ladder with the comparisons the wrong
 * way round too. An open network displayed as a green A; a hardened one as a
 * red F. On the one screen whose entire job is naming the weakest network.
 *
 * Two independent spellings of one rule, both wrong, cancelling into something
 * that looked plausible. */
static void test_survey_worst_grade_is_the_worst(void)
{
    banner("survey: the worst grade is the WORST one, not the best");
    psv_t s;
    psv_reset(&s, 0);

    const uint8_t a[6] = { 0, 0, 0, 0, 0, 1 };
    const uint8_t b[6] = { 0, 0, 0, 0, 0, 2 };
    const uint8_t c[6] = { 0, 0, 0, 0, 0, 3 };

    psv_note_network(&s, a, PC_GRADE_A_PLUS, 0, 1000);
    psv_note_network(&s, b, PC_GRADE_F, 0, 1000);
    psv_note_network(&s, c, PC_GRADE_B, 0, 1000);

    psv_report_t v;
    psv_summarise(&s, 2000, &v);
    CHECK_EQ(v.networks, 3);
    CHECK(v.worst_grade == PC_GRADE_F,
          "F is worse than B and A+ (got %u)", (unsigned)v.worst_grade);

    /* And a single network cannot talk its way UP by being heard again more
     * favourably - the whole point of keeping the worst. */
    psv_t one;
    psv_reset(&one, 0);
    psv_note_network(&one, a, PC_GRADE_D, 0, 1000);
    psv_note_network(&one, a, PC_GRADE_A_PLUS, 0, 1100);
    psv_summarise(&one, 2000, &v);
    CHECK_EQ(v.networks, 1);
    CHECK(v.worst_grade == PC_GRADE_D, "it keeps the worse of the two");

    /* But it CAN be revised downward, because hearing something worse is new
     * evidence rather than a better mood. */
    psv_note_network(&one, a, PC_GRADE_F, 0, 1200);
    psv_summarise(&one, 2000, &v);
    CHECK(v.worst_grade == PC_GRADE_F, "and drops when something worse is heard");

    /* UNGRADED is "not enough heard", not "terrible". It must never win, or a
     * single newly-appeared access point would report the whole estate as
     * unassessable. */
    psv_t u;
    psv_reset(&u, 0);
    psv_note_network(&u, a, PC_GRADE_B, 0, 1000);
    psv_note_network(&u, b, PC_GRADE_UNGRADED, 0, 1000);
    psv_summarise(&u, 2000, &v);
    CHECK(v.worst_grade == PC_GRADE_B, "ungraded does not count as worst");

    /* Nothing graded at all reports UNGRADED rather than a letter. */
    psv_t none;
    psv_reset(&none, 0);
    psv_note_network(&none, a, PC_GRADE_UNGRADED, 0, 1000);
    psv_summarise(&none, 2000, &v);
    CHECK(v.worst_grade == PC_GRADE_UNGRADED, "and nothing graded says so");

    /* THE RENDERING, which was the second half of the bug. Every grade must
     * come back as itself. */
    CHECK(strcmp(pc_grade_name(PC_GRADE_F), "F") == 0, "F prints as F");
    CHECK(strcmp(pc_grade_name(PC_GRADE_A_PLUS), "A+") == 0, "A+ prints as A+");
    CHECK(strcmp(pc_grade_name(PC_GRADE_F), pc_grade_name(PC_GRADE_A_PLUS)) != 0,
          "and the two ends of the scale are not the same letter");
}

void test_survey(void)
{
    test_survey_worst_grade_is_the_worst();
    test_survey_quantifiers_are_honest();
    test_survey_counts_addresses_once();
    test_survey_keeps_the_worst_view();
    test_survey_admits_its_limit();
    test_survey_never_calls_a_neighbour_an_attacker();
    test_survey_lines_fit_at_full_scale();
    test_survey_reports_good_news();
    test_survey_devices_and_tools();
    test_survey_empty_and_null();
}
