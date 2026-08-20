/* Host tests for the Watchtower rotation.
 *
 * The interesting failures here are not crashes. They are the comfortable
 * lies a home screen can tell: six green dots implying six live receivers,
 * "all quiet" said on the strength of watches that have not looked, and a
 * reading from a minute ago drawn in the same ink as one from now. Each of
 * those is a test.
 */
#include <string.h>

#include "pharos_tower.h"
#include "test_support.h"

#define T0 1000000000ull
#define SEC 1000000ull

static void arm_six(ptw_state_st *s)
{
    ptw_reset(s, 5000);
    ptw_arm(s, "wifi.watch", "WATCH");
    ptw_arm(s, "wifi.census", "CENSUS");
    ptw_arm(s, "wifi.twin", "TWIN");
    ptw_arm(s, "wifi.karma", "KARMA");
    ptw_arm(s, "wifi.mirage", "MIRAGE");
    ptw_arm(s, "ble.rival", "RIVAL");
}

static void test_tower_arming(void)
{
    banner("tower: a watch is armed once, and the table is bounded");
    ptw_state_st s;
    arm_six(&s);
    CHECK_EQ(s.n, 6);
    CHECK(ptw_find(&s, "wifi.watch") == 0, "ids resolve to slots");
    CHECK(ptw_find(&s, "nope") < 0, "and an unknown id does not");

    /* Arming the same lens twice would give it two dots on one ring and two
     * turns in one rotation. */
    CHECK(ptw_arm(&s, "wifi.watch", "WATCH") < 0, "no lens is armed twice");
    CHECK_EQ(s.n, 6);

    ptw_state_st full;
    ptw_reset(&full, 5000);
    char id[8];
    for (unsigned i = 0; i < PTW_MAX_WATCHES + 4u; i++) {
        snprintf(id, sizeof(id), "l%u", i);
        ptw_arm(&full, id, id);
    }
    CHECK_EQ(full.n, PTW_MAX_WATCHES);

    /* The dwell has a floor: a rotation faster than the engines' own windows
     * would hand every watch a slice too short to conclude anything, and six
     * lenses stuck at UNKNOWN looks exactly like six broken lenses. */
    ptw_state_st fast;
    ptw_reset(&fast, 10);
    CHECK(fast.dwell_ms >= 2000u, "the dwell cannot be set uselessly short");
}

/* NOBODY IS TOLD "ALL QUIET" ON THE STRENGTH OF A WATCH THAT NEVER LOOKED. */
static void test_tower_unknown_is_not_quiet(void)
{
    banner("tower: not having looked is not the same as having found nothing");
    ptw_state_st s;
    arm_six(&s);

    ptw_summary_t sum;
    ptw_summarise(&s, T0, &sum);
    CHECK_EQ(sum.armed, 6);
    CHECK_EQ(sum.unknown, 6);
    CHECK_EQ(sum.quiet, 0);
    CHECK_EQ(sum.reporting, 0);
    CHECK(strcmp(sum.headline, "all quiet") != 0,
          "a ring that has not looked does not claim to be clear");
    CHECK(strcmp(sum.headline, "still listening") == 0, "it says so instead");

    /* One watch reports quiet. Five still have no opinion, so the picture is
     * partial and the headline must not round it up. */
    ptw_report(&s, "wifi.watch", PTW_QUIET, 4, 60, T0);
    ptw_summarise(&s, T0, &sum);
    CHECK_EQ(sum.reporting, 1);
    CHECK_EQ(sum.quiet, 1);
    CHECK_EQ(sum.unknown, 5);
    CHECK(strcmp(sum.headline, "all quiet") != 0,
          "one quiet watch out of six is not all quiet");

    /* All six, and only now. */
    ptw_report(&s, "wifi.census", PTW_QUIET, 2, 60, T0);
    ptw_report(&s, "wifi.twin", PTW_QUIET, 0, 60, T0);
    ptw_report(&s, "wifi.karma", PTW_QUIET, 0, 60, T0);
    ptw_report(&s, "wifi.mirage", PTW_QUIET, 1, 60, T0);
    ptw_report(&s, "ble.rival", PTW_QUIET, 0, 60, T0);
    ptw_summarise(&s, T0, &sum);
    CHECK_EQ(sum.reporting, 6);
    CHECK(strcmp(sum.headline, "all quiet") == 0,
          "and when every armed watch has looked, it may say so");
}

/* A READING HAS AN AGE, AND THE RING MUST SHOW IT. */
static void test_tower_freshness_fades(void)
{
    banner("tower: an old reading is never drawn as a live one");
    ptw_state_st s;
    arm_six(&s);
    ptw_report(&s, "wifi.watch", PTW_QUIET, 3, 60, T0);

    const uint64_t rot = 6ull * 5000ull * 1000ull; /* six watches, 5 s each */

    CHECK(ptw_freshness(&s, 0, T0) == PTW_FRESH, "just taken");
    CHECK(ptw_freshness(&s, 0, T0 + rot - SEC) == PTW_FRESH,
          "still fresh inside one rotation");
    CHECK(ptw_freshness(&s, 0, T0 + rot + SEC) == PTW_AGEING,
          "ageing once a rotation has gone by");
    CHECK(ptw_freshness(&s, 0, T0 + rot * 4ull) == PTW_EXPIRED,
          "expired after three");

    /* A watch that has never reported has nothing to stand behind either. */
    CHECK(ptw_freshness(&s, 1, T0) == PTW_EXPIRED, "never looked reads as stale");

    /* AND THE ONE THAT MATTERS: an expired reading stops counting as quiet.
     * Otherwise the ring would keep saying "all quiet" forever on the strength
     * of a scan from ten minutes ago, which is the exact comfort this engine
     * exists to refuse. */
    ptw_report(&s, "wifi.census", PTW_QUIET, 0, 60, T0);
    ptw_report(&s, "wifi.twin", PTW_QUIET, 0, 60, T0);
    ptw_report(&s, "wifi.karma", PTW_QUIET, 0, 60, T0);
    ptw_report(&s, "wifi.mirage", PTW_QUIET, 0, 60, T0);
    ptw_report(&s, "ble.rival", PTW_QUIET, 0, 60, T0);
    ptw_summary_t sum;
    ptw_summarise(&s, T0, &sum);
    CHECK(strcmp(sum.headline, "all quiet") == 0, "fresh: all quiet");
    ptw_summarise(&s, T0 + rot * 4ull, &sum);
    CHECK_EQ(sum.reporting, 0);
    CHECK_EQ(sum.quiet, 0);
    CHECK(strcmp(sum.headline, "all quiet") != 0,
          "stale: it stops claiming to be clear");
}

/* ARMING ANOTHER WATCH SLOWS EVERY OTHER WATCH DOWN - AND THE RULE KNOWS. */
static void test_tower_freshness_scales_with_ring(void)
{
    banner("tower: freshness is measured in rotations, not seconds");
    ptw_state_st small, big;
    ptw_reset(&small, 5000);
    ptw_arm(&small, "a", "A");
    ptw_arm(&small, "b", "B");
    ptw_report(&small, "a", PTW_QUIET, 0, 60, T0);

    ptw_reset(&big, 5000);
    for (unsigned i = 0; i < 8; i++) {
        char id[4];
        snprintf(id, sizeof(id), "w%u", i);
        ptw_arm(&big, id, id);
    }
    ptw_report(&big, "w0", PTW_QUIET, 0, 60, T0);

    /* Twenty seconds later. On a two-watch ring that is two full rotations,
     * so the reading is old. On an eight-watch ring it is less than one, so it
     * is still the freshest thing available - and pretending otherwise would
     * make a bigger ring look permanently broken. */
    const uint64_t t = T0 + 20ull * SEC;
    CHECK(ptw_freshness(&small, 0, t) == PTW_AGEING, "two-watch ring: ageing");
    CHECK(ptw_freshness(&big, 0, t) == PTW_FRESH, "eight-watch ring: still fresh");
}

static void test_tower_worst_leads(void)
{
    banner("tower: the middle of the dial names the worst thing on it");
    ptw_state_st s;
    arm_six(&s);
    ptw_report(&s, "wifi.census", PTW_NOTED, 30, 70, T0);
    ptw_report(&s, "wifi.watch", PTW_ALARM, 77, 88, T0);
    ptw_report(&s, "ble.rival", PTW_ELEVATED, 55, 60, T0);

    ptw_summary_t sum;
    ptw_summarise(&s, T0, &sum);
    CHECK(sum.worst == PTW_ALARM, "the alarm wins");
    CHECK(sum.worst_index == ptw_find(&s, "wifi.watch"), "and is named");
    CHECK_EQ(sum.alarms, 1);
    CHECK(strcmp(sum.headline, "ALERT") == 0, "one alarm reads as ALERT");

    ptw_report(&s, "wifi.karma", PTW_ALARM, 80, 88, T0);
    ptw_summarise(&s, T0, &sum);
    CHECK_EQ(sum.alarms, 2);
    CHECK(strcmp(sum.headline, "ALERTS") == 0, "two read as ALERTS");

    /* An alarm that has gone stale stops being counted as current - it is
     * Aegis' job to remember it, not the ring's to keep asserting it. */
    ptw_summarise(&s, T0 + 6ull * 5ull * SEC * 4ull, &sum);
    CHECK_EQ(sum.alarms, 0);
}

/* THE ROTATION ITSELF. */
static void test_tower_rotation(void)
{
    banner("tower: every armed watch gets the radio, in turn");
    ptw_state_st s;
    arm_six(&s);

    bool changed = false;
    uint64_t t = T0;
    int who = ptw_turn(&s, t, false, &changed);
    CHECK_EQ(who, 0);
    CHECK(changed, "the first call hands the radio to somebody");

    /* Inside the slice, nothing moves. */
    who = ptw_turn(&s, t + 1ull * SEC, false, &changed);
    CHECK_EQ(who, 0);
    CHECK(!changed, "a watch keeps its slice");

    /* At the end of it, the next one takes over. */
    t += 5ull * SEC;
    who = ptw_turn(&s, t, false, &changed);
    CHECK_EQ(who, 1);
    CHECK(changed, "and hands on when the slice is up");

    /* A full pass visits all six and comes home. */
    unsigned seen[PTW_MAX_WATCHES];
    memset(seen, 0, sizeof(seen));
    seen[0]++; seen[1]++;
    for (unsigned i = 0; i < 4; i++) {
        t += 5ull * SEC;
        who = ptw_turn(&s, t, false, &changed);
        CHECK(who >= 0 && who < 6, "the cursor stays in range");
        seen[who]++;
    }
    for (unsigned i = 0; i < 6; i++) {
        CHECK(seen[i] == 1, "watch %u held the radio exactly once", i);
    }
    t += 5ull * SEC;
    who = ptw_turn(&s, t, false, &changed);
    CHECK_EQ(who, 0);
    CHECK(s.rotations >= 1u, "and a completed pass is counted");
}

/* THE RULE THAT MAKES THE ROTATION SAFE. */
static void test_tower_holds_on_evidence(void)
{
    banner("tower: it does not walk away from an attack to keep a rota tidy");
    ptw_state_st s;
    arm_six(&s);

    bool changed = false;
    uint64_t t = T0;
    int who = ptw_turn(&s, t, false, &changed);
    CHECK_EQ(who, 0);

    /* Watch is hearing something, so it keeps the radio past the end of its
     * slice - which is also what its confidence ceiling needs, since the
     * ceiling is a function of airtime. It may do so PTW_MAX_HOLD_SLICES
     * times; the cap is tested on its own in the starvation test below. */
    for (unsigned i = 0; i < PTW_MAX_HOLD_SLICES; i++) {
        t += 5ull * SEC;
        who = ptw_turn(&s, t, true, &changed);
        CHECK_EQ(who, 0);
        CHECK(!changed, "the radio stays where the evidence is");
    }

    /* When it goes quiet again the rotation resumes - and does not immediately
     * hand over to make up for lost turns, because a slice of debt would mean
     * the next watch gets no dwell at all. */
    who = ptw_turn(&s, t + 1ull * SEC, false, &changed);
    CHECK_EQ(who, 0);
    CHECK(!changed, "no accumulated debt is spent at the next watch's expense");
    CHECK_EQ(s.held, PTW_MAX_HOLD_SLICES);

    t += 6ull * SEC;
    who = ptw_turn(&s, t, false, &changed);
    CHECK_EQ(who, 1);
    CHECK(changed, "and then it moves on normally");
}

/* THE STARVATION BUG, WHICH SHIPPED.
 *
 * The hold above was unbounded, and on the first run on hardware the
 * microphone watch - which sits at ELEVATED in any ordinary room, because
 * there is always something at 19 kHz - took the radio and never gave it back.
 * Eleven of twelve watches had never run and the ring was eleven hollow dots:
 * the one thing it exists to show, gone, silently.
 *
 * A hold may extend a slice. It may not abolish it. */
static void test_tower_hold_cannot_starve_the_ring(void)
{
    banner("tower: a permanently noisy watch cannot keep the radio forever");
    ptw_state_st s;
    arm_six(&s);

    bool changed = false;
    uint64_t t = T0;
    int who = ptw_turn(&s, t, true, &changed);
    CHECK_EQ(who, 0);

    /* Watch 0 claims to be hearing something on every single call, forever -
     * exactly what Whisper did. Run a long way past the cap. */
    unsigned handovers = 0;
    int last = who;
    for (unsigned i = 0; i < 200; i++) {
        t += 5ull * SEC;
        who = ptw_turn(&s, t, true, &changed);
        if (who != last) {
            handovers++;
            last = who;
        }
    }
    CHECK(handovers > 0, "the radio does move on");

    /* And every watch got real turns, not just one or two. */
    for (unsigned i = 0; i < 6; i++) {
        CHECK(s.w[i].visits > 0, "watch %u has held the radio", i);
    }
    CHECK(s.rotations > 0, "the ring completed passes");

    /* The hold is still worth something: a watch that is hearing something
     * gets more airtime than one that is not. */
    ptw_state_st quiet;
    arm_six(&quiet);
    uint64_t qt = T0;
    ptw_turn(&quiet, qt, false, &changed);
    for (unsigned i = 0; i < 200; i++) {
        qt += 5ull * SEC;
        ptw_turn(&quiet, qt, false, &changed);
    }
    CHECK(quiet.rotations > s.rotations,
          "a quiet ring turns faster than one that keeps stopping to listen");

    /* The hold counter resets on handover, so the NEXT watch gets its own full
     * allowance rather than inheriting a spent one. */
    CHECK(s.held <= PTW_MAX_HOLD_SLICES, "the hold counter stays bounded");
}

static void test_tower_empty(void)
{
    banner("tower: an empty ring says so rather than dividing by zero");
    ptw_state_st s;
    ptw_reset(&s, 5000);

    bool changed = true;
    CHECK(ptw_turn(&s, T0, false, &changed) < 0, "nobody's turn");
    CHECK(!changed, "and nothing changed");

    ptw_summary_t sum;
    ptw_summarise(&s, T0, &sum);
    CHECK_EQ(sum.armed, 0);
    CHECK(sum.worst_index == -1, "nothing to name");
    CHECK(strcmp(sum.headline, "no watches") == 0, "and it says so");

    /* Freshness on an empty ring must not divide by an armed count of zero. */
    CHECK(ptw_freshness(&s, 0, T0) == PTW_EXPIRED, "no slot, no confidence");
    ptw_summarise(NULL, T0, &sum);
    CHECK(sum.worst_index == -1, "a NULL state is survivable");
}

/* WEIGHTED TURNS: adding surveys must not slow the event detectors.
 *
 * Thirteen watches at five seconds is a sixty-five second lap, so a
 * deauthentication flood - which lasts seconds - would be missed while the
 * receiver re-counted the same access points for the fourth time. Events take
 * every lap; standing facts take every other one. */
static void test_tower_periods_protect_the_fast_watches(void)
{
    banner("tower: a survey every other lap, an event detector every lap");
    ptw_state_st s;
    ptw_reset(&s, 5000);
    ptw_arm_every(&s, "wifi.watch", "WATCH", 1);
    ptw_arm_every(&s, "wifi.karma", "KARMA", 1);
    ptw_arm_every(&s, "wifi.census", "CENSUS", 2);
    ptw_arm_every(&s, "wifi.probe", "PROBE", 2);

    bool changed = false;
    uint64_t t = T0;
    ptw_turn(&s, t, false, &changed);
    for (unsigned i = 0; i < 60; i++) {
        t += 5ull * SEC;
        ptw_turn(&s, t, false, &changed);
    }

    const uint32_t fast = s.w[0].visits;
    const uint32_t slow = s.w[2].visits;
    CHECK(fast > 0 && slow > 0, "everybody gets the radio eventually");
    CHECK(fast > slow, "but the event detector gets it more often");
    /* Roughly twice as often, allowing for where the run happened to stop. */
    CHECK(fast >= slow * 2u - 2u && fast <= slow * 2u + 2u,
          "about twice: fast=%u slow=%u", (unsigned)fast, (unsigned)slow);
    /* The two period-1 watches track each other, give or take wherever the
     * run happened to stop mid-lap. */
    const int32_t skew = (int32_t)s.w[0].visits - (int32_t)s.w[1].visits;
    CHECK(skew >= -1 && skew <= 1, "the two fast watches stay level");

    /* A period is clamped, because a watch nobody ever sees is a dot that
     * means nothing. */
    ptw_state_st c;
    ptw_reset(&c, 5000);
    ptw_arm_every(&c, "a", "A", 99);
    CHECK(c.w[0].period <= PTW_MAX_PERIOD, "an absurd period is clamped");
    ptw_arm_every(&c, "b", "B", 0);
    CHECK_EQ(c.w[1].period, 1);
}

/* A survey arriving exactly on schedule must not be called stale for it. */
static void test_tower_period_scales_freshness(void)
{
    banner("tower: a period-2 watch is not stale for being on time");
    ptw_state_st s;
    ptw_reset(&s, 5000);
    ptw_arm_every(&s, "fast", "FAST", 1);
    ptw_arm_every(&s, "slow", "SLOW", 2);

    ptw_report(&s, "fast", PTW_QUIET, 0, 60, T0);
    ptw_report(&s, "slow", PTW_QUIET, 0, 60, T0);

    /* One fast and one slow watch: lap 0 runs both (10 s of slices), lap 1
     * runs only the fast one (5 s), so the slow watch's own interval is 15 s
     * and the fast one's is 7.5 s. Fourteen seconds is past the fast watch's
     * window and inside the slow watch's - which is the whole point. */
    const uint64_t t = T0 + 14ull * SEC;
    CHECK(ptw_freshness(&s, 0, t) == PTW_AGEING, "the fast one has aged");
    CHECK(ptw_freshness(&s, 1, t) == PTW_FRESH, "the slow one is on schedule");

    /* And it does still go stale eventually. */
    CHECK(ptw_freshness(&s, 1, T0 + 300ull * SEC) == PTW_EXPIRED,
          "a period is not an excuse to never expire");
}

/* A ring made entirely of period-2 watches must not stall on an odd lap. */
static void test_tower_all_slow_still_turns(void)
{
    banner("tower: a ring with nothing due this lap still moves on");
    ptw_state_st s;
    ptw_reset(&s, 5000);
    ptw_arm_every(&s, "a", "A", 2);
    ptw_arm_every(&s, "b", "B", 2);
    ptw_arm_every(&s, "c", "C", 4);

    bool changed = false;
    uint64_t t = T0;
    int who = ptw_turn(&s, t, false, &changed);
    CHECK(who >= 0, "somebody starts");
    for (unsigned i = 0; i < 80; i++) {
        t += 5ull * SEC;
        who = ptw_turn(&s, t, false, &changed);
        CHECK(who >= 0 && who < 3, "the cursor never leaves the table");
    }
    for (unsigned i = 0; i < 3; i++) {
        CHECK(s.w[i].visits > 0, "watch %u ran at least once", i);
    }
}

/* CHOOSING WHAT THE RING WATCHES.
 *
 * Disarming is not just tidying the display: every watch left on is airtime the
 * ones you care about do not get, so switching one off must actually make the
 * lap shorter. */
static void test_tower_arming_is_customisable(void)
{
    banner("tower: switching a watch off shortens the lap for the rest");
    ptw_state_st s;
    arm_six(&s);

    const uint32_t before = ptw_lap_ms(&s);
    CHECK_EQ(before, 30000); /* six watches, five seconds each */

    CHECK(ptw_set_armed(&s, 5, false), "a watch can be switched off");
    const uint32_t after = ptw_lap_ms(&s);
    CHECK_EQ(after, 25000);
    CHECK(after < before, "and the others come round sooner for it");

    /* It stops taking turns. */
    bool changed = false;
    uint64_t t = T0;
    ptw_turn(&s, t, false, &changed);
    for (unsigned i = 0; i < 40; i++) {
        t += 5ull * SEC;
        const int who = ptw_turn(&s, t, false, &changed);
        CHECK(who != 5, "a disarmed watch never gets the radio");
    }
    CHECK_EQ(s.w[5].visits, 0);

    /* And it is not counted as armed, quiet, or unknown - it is simply not on
     * the ring, which is different from being on it and having nothing to say. */
    ptw_summary_t sum;
    ptw_summarise(&s, t, &sum);
    CHECK_EQ(sum.armed, 5);

    /* Back on again, with no memory of what it said before. A reading from
     * before it was switched off is not a reading about now. */
    ptw_report(&s, "ble.rival", PTW_ALARM, 90, 90, T0);
    ptw_set_armed(&s, 5, false);
    ptw_set_armed(&s, 5, true);
    CHECK(s.w[5].state == PTW_UNKNOWN, "a re-armed watch starts with no opinion");
    CHECK_EQ(s.w[5].seen_us, 0);
    CHECK(ptw_freshness(&s, 5, t) == PTW_EXPIRED, "and nothing to stand behind");
}

/* THE STATE WITH NO WAY OUT. The ring's own controls are reached through the
 * ring, so a watchtower watching nothing must not be reachable. */
static void test_tower_cannot_empty_itself(void)
{
    banner("tower: the last watch cannot be switched off");
    ptw_state_st s;
    ptw_reset(&s, 5000);
    ptw_arm(&s, "a", "A");
    ptw_arm(&s, "b", "B");

    CHECK(ptw_set_armed(&s, 0, false), "the first may go");
    CHECK(!ptw_set_armed(&s, 1, false), "the last may not");
    CHECK(s.w[1].armed, "and is still armed after the refusal");

    ptw_summary_t sum;
    ptw_summarise(&s, T0, &sum);
    CHECK_EQ(sum.armed, 1);

    /* Out of range is refused rather than clamped. */
    CHECK(!ptw_set_armed(&s, 99, true), "an index past the end is refused");
    CHECK(!ptw_set_period(&s, 99, 1), "for periods too");
    CHECK(!ptw_set_armed(NULL, 0, true), "and a NULL state is survivable");
    CHECK_EQ(ptw_lap_ms(NULL), 0);
}

static void test_tower_period_is_editable(void)
{
    banner("tower: a watch's period can be changed, within bounds");
    ptw_state_st s;
    arm_six(&s);
    const uint32_t base = ptw_lap_ms(&s);

    CHECK(ptw_set_period(&s, 0, 2), "a watch can be slowed down");
    CHECK_EQ(s.w[0].period, 2);
    CHECK(ptw_lap_ms(&s) < base, "which shortens the average lap");

    ptw_set_period(&s, 0, 99);
    CHECK_EQ(s.w[0].period, PTW_MAX_PERIOD);
    ptw_set_period(&s, 0, 0);
    CHECK_EQ(s.w[0].period, 1);
}

/* Disarming the watch that is holding the radio right now. */
static void test_tower_disarm_the_active_watch(void)
{
    banner("tower: switching off the watch that holds the radio");
    ptw_state_st s;
    arm_six(&s);

    bool changed = false;
    uint64_t t = T0;
    const int who = ptw_turn(&s, t, false, &changed);
    CHECK_EQ(who, 0);

    CHECK(ptw_set_armed(&s, 0, false), "it can be switched off mid-turn");
    const int next = ptw_turn(&s, t, false, &changed);
    CHECK(next != 0, "and the radio moves on rather than sitting on nothing");
    CHECK(s.w[next].armed, "onto something that is actually armed");
}

void test_tower(void)
{
    test_tower_arming_is_customisable();
    test_tower_cannot_empty_itself();
    test_tower_period_is_editable();
    test_tower_disarm_the_active_watch();
    test_tower_periods_protect_the_fast_watches();
    test_tower_period_scales_freshness();
    test_tower_all_slow_still_turns();
    test_tower_arming();
    test_tower_unknown_is_not_quiet();
    test_tower_freshness_fades();
    test_tower_freshness_scales_with_ring();
    test_tower_worst_leads();
    test_tower_rotation();
    test_tower_holds_on_evidence();
    test_tower_hold_cannot_starve_the_ring();
    test_tower_empty();
}
