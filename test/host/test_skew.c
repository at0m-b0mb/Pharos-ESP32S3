/* Clock skew: which physical radio is behind a name.
 *
 * The refusals matter as much as the findings here. This engine can accuse an
 * access point of having been replaced, and a false accusation sends somebody
 * looking for an attacker who does not exist.
 */
#include <string.h>

#include "pharos_skew.h"
#include "test_support.h"

#define SEC 1000000ull

static void mk(uint8_t out[6], uint8_t last)
{
    const uint8_t b[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x00 };
    memcpy(out, b, 6);
    out[5] = last;
}

/* Feed `secs` seconds of beacons from a radio whose clock runs `ppm` fast,
 * starting at local time `t0`, with `jitter_us` of one-sided reception delay
 * on all but the first beacon of each second. */
/* Returns the local time AND the AP's own clock where it left off, through
 * `tsf_io`. Continuing a run means continuing BOTH: a first version restarted
 * the TSF from a recomputed value that was 560 us behind where the previous
 * segment actually ended, and that discontinuity read as an enormous skew.
 * The engine was right and the fixture was lying to it. */
static uint64_t feed(psk_engine_t *e, const uint8_t bssid[6], uint64_t t0,
                     unsigned secs, int ppm, uint64_t *tsf_io,
                     unsigned jitter_us)
{
    uint64_t local = t0;
    uint64_t tsf = *tsf_io;
    for (unsigned s = 0; s < secs; s++) {
        for (unsigned k = 0; k < 10u; k++) { /* ten beacons a second */
            /* The first beacon of each second arrives clean; the rest are
             * taxed, which is what reception delay actually looks like. */
            const uint64_t seen = local + (k ? jitter_us : 0u);
            psk_observe(e, bssid, tsf, seen);
            local += 100000ull; /* 100 ms */
            tsf += 100000ull + (uint64_t)(100000ll * ppm / 1000000);
        }
    }
    *tsf_io = tsf;
    return local;
}

static void test_skew_measures_a_crystal(void)
{
    banner("skew: a stable radio has a stable rate");

    psk_engine_t e;
    psk_reset(&e);
    uint8_t ap[6];
    mk(ap, 0x01);

    uint64_t tsf = 5000000ull;
    feed(&e, ap, 10 * SEC, 40, 30, &tsf, 400);

    psk_verdict_t v;
    psk_evaluate(&e, ap, &v);

    CHECK(v.have_skew, "a long enough baseline produces a reading");
    CHECK(!v.changed, "and a steady crystal is never accused");
    CHECK(v.ppm > 20 && v.ppm < 40, "the rate is about right (%d ppm)", v.ppm);
    CHECK(v.span_s >= 20, "over a real span (%us)", v.span_s);
}

static void test_skew_refuses_a_short_baseline(void)
{
    banner("skew: ten parts per million over five seconds is jitter");

    /* THE HONEST REFUSAL. Fifty microseconds of drift is well inside the
     * reception delay, so a verdict here would be a measurement of how busy
     * the CPU was. */
    psk_engine_t e;
    psk_reset(&e);
    uint8_t ap[6];
    mk(ap, 0x02);

    uint64_t tsf = 5000000ull;
    feed(&e, ap, 10 * SEC, 5, 30, &tsf, 400);

    psk_verdict_t v;
    psk_evaluate(&e, ap, &v);
    CHECK(!v.have_skew, "no reading from a short baseline");
    CHECK(v.measuring, "but it says it is still measuring");
    CHECK(!v.changed, "and accuses nobody");
}

static void test_skew_catches_a_swapped_radio(void)
{
    banner("skew: the crystal behind a name changed");

    /* THE FINDING. One BSSID, two oscillators - which is what an evil twin
     * that has cloned a BSSID actually is. */
    psk_engine_t e;
    psk_reset(&e);
    uint8_t ap[6];
    mk(ap, 0x03);

    uint64_t tsf = 5000000ull;
    uint64_t t = feed(&e, ap, 10 * SEC, 40, 12, &tsf, 300);

    psk_verdict_t before;
    psk_evaluate(&e, ap, &before);
    CHECK(before.have_skew && !before.changed, "the real AP is not accused");

    /* Now somebody else starts beaconing as it, with their own crystal. */
    psk_reset(&e);
    tsf = 5000000ull;
    t = feed(&e, ap, 10 * SEC, 40, 12, &tsf, 300);
    /* A DIFFERENT radio: its own crystal, and its own uptime, so the clock
     * jumps as well as changing rate. That is what a clone looks like. */
    uint64_t other = 900000000ull;
    feed(&e, ap, t + SEC, 40, 70, &other, 300);

    psk_verdict_t v;
    psk_evaluate(&e, ap, &v);
    CHECK(v.changed, "a different oscillator is noticed");
    CHECK(v.to_ppm != v.from_ppm, "and both rates are reported");
}

static void test_skew_thermal_drift_is_not_an_attack(void)
{
    banner("skew: a couple of ppm of warming is not a second radio");

    /* THE NEGATIVE THIS EXISTS FOR. Crystals drift with temperature - an AP
     * that has just been switched on warms up over minutes. Accusing it would
     * mean every device in the world is an evil twin shortly after boot. */
    psk_engine_t e;
    psk_reset(&e);
    uint8_t ap[6];
    mk(ap, 0x04);

    uint64_t tsf = 5000000ull;
    uint64_t t = feed(&e, ap, 10 * SEC, 40, 14, &tsf, 300);
    /* The SAME radio, warming: the rate shifts a little, the clock does not
     * jump, because it never stopped counting. */
    feed(&e, ap, t, 40, 18, &tsf, 300);

    psk_verdict_t v;
    psk_evaluate(&e, ap, &v);
    CHECK(!v.changed, "four ppm of drift is not an accusation");
}

static void test_skew_unknown_and_empty(void)
{
    banner("skew: nothing seen, nothing claimed");

    psk_engine_t e;
    psk_reset(&e);
    uint8_t ap[6];
    mk(ap, 0x05);

    psk_verdict_t v;
    psk_evaluate(&e, ap, &v);
    CHECK(!v.have_skew, "an unseen BSSID has no skew");
    CHECK(!v.measuring, "and is not even measuring");
    CHECK(!v.changed, "and is not accused");
    CHECK(!psk_any_changed(&e), "and neither is anybody else");

    /* A zero timestamp is a frame we could not parse, not a clock at zero. */
    psk_observe(&e, ap, 0, 12345678ull);
    psk_evaluate(&e, ap, &v);
    CHECK(!v.measuring, "an unparsed timestamp is not a sample");
}

static void test_skew_table_is_bounded(void)
{
    banner("skew: a busy site does not overrun the table");

    psk_engine_t e;
    psk_reset(&e);
    for (unsigned i = 0; i < PSK_MAX_APS * 3u; i++) {
        uint8_t ap[6];
        mk(ap, (uint8_t)i);
        psk_observe(&e, ap, 5000000ull + i, 10 * SEC + i);
    }
    CHECK(e.n <= PSK_MAX_APS, "the table holds (%u)", e.n);
}

void test_skew(void)
{
    test_skew_measures_a_crystal();
    test_skew_refuses_a_short_baseline();
    test_skew_catches_a_swapped_radio();
    test_skew_thermal_drift_is_not_an_attack();
    test_skew_unknown_and_empty();
    test_skew_table_is_bounded();
}
