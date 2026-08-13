/* Pharos host tests, part eight: the RSSI direction finder. */
#include "pharos_locate.h"
#include "test_support.h"

static const uint8_t TARGET[6] = { 0x02, 0x66, 0x6E, 0x00, 0x00, 0x02 };
static const uint8_t OTHER[6]  = { 0xAC, 0x11, 0x22, 0x33, 0x44, 0x55 };

/* Feed n samples ramping linearly from rssi0 to rssi1, with optional jitter. */
static void walk(pl_engine_t *e, int rssi0, int rssi1, int n, int jitter, uint64_t *t)
{
    for (int i = 0; i < n; i++) {
        const int base = rssi0 + (rssi1 - rssi0) * i / (n > 1 ? n - 1 : 1);
        const int j = jitter ? ((i * 7) % (2 * jitter + 1)) - jitter : 0;
        int r = base + j;
        if (r > 0) r = 0;
        if (r < -110) r = -110;
        pl_observe(e, TARGET, (int8_t)r, *t);
        *t += 200000; /* 5 Hz */
    }
}

void test_locate(void)
{
    banner("locate: hotter / colder direction finding");
    pl_engine_t e;
    pl_verdict_t v;
    uint64_t t = 1000000;

    /* No target, no samples. */
    pl_reset(&e, NULL);
    pl_evaluate(&e, &v);
    CHECK(strstr(v.headline, "No target") != NULL, "no target disclosed");

    /* Only the target's frames count; another MAC is ignored. */
    pl_reset(&e, TARGET);
    for (int i = 0; i < 20; i++) {
        pl_observe(&e, OTHER, -40, t);
        t += 100000;
    }
    pl_evaluate(&e, &v);
    CHECK_EQ(v.samples, 0);
    CHECK(!v.locked, "no target frames yet, not locked");

    /* Walk IN: signal rises from far to near. Must end HOTTER or HERE, with
     * closeness climbing. */
    pl_reset(&e, TARGET);
    t = 1000000;
    walk(&e, -85, -40, 40, 0, &t);
    pl_evaluate(&e, &v);
    CHECK(v.locked, "locked after enough samples");
    CHECK(v.trend == PL_TREND_HOTTER || v.trend == PL_TREND_HERE,
          "walking in reads warmer (got %s)", pl_trend_name(v.trend));
    CHECK(v.closeness >= 65, "closeness high near the source (got %u)", v.closeness);
    CHECK_EQ(v.rssi_peak, -40);

    /* Continue right up to the source and hold: HERE. */
    walk(&e, -34, -32, 20, 0, &t);
    pl_evaluate(&e, &v);
    CHECK_EQ(v.trend, PL_TREND_HERE);
    CHECK(v.closeness >= 90, "very close (got %u)", v.closeness);

    /* Walk OUT: signal falls. Must read COLDER. */
    walk(&e, -40, -85, 40, 0, &t);
    pl_evaluate(&e, &v);
    CHECK_EQ(v.trend, PL_TREND_COLDER);
    CHECK(v.closeness < 40, "closeness low far away (got %u)", v.closeness);

    /* Stationary but NOISY: must stay STEADY, not flip back and forth. This is
     * the whole point of the hysteresis. */
    pl_reset(&e, TARGET);
    t = 1000000;
    walk(&e, -60, -60, 60, 6, &t); /* +/-6 dB jitter around a fixed level */
    pl_evaluate(&e, &v);
    CHECK_EQ(v.trend, PL_TREND_STEADY);
    CHECK(v.closeness >= 45 && v.closeness <= 55, "mid closeness (got %u)", v.closeness);

    /* Closeness maps the fixed scale: -90 -> ~0, -30 -> 100. */
    pl_reset(&e, TARGET);
    t = 1000000;
    walk(&e, -90, -90, 12, 0, &t);
    pl_evaluate(&e, &v);
    CHECK(v.closeness <= 3, "floor of the scale (got %u)", v.closeness);
    pl_reset(&e, TARGET);
    t = 1000000;
    walk(&e, -30, -30, 12, 0, &t);
    pl_evaluate(&e, &v);
    CHECK(v.closeness >= 97, "top of the scale (got %u)", v.closeness);

    /* Confidence is docked right after a deep fade below the peak (a body
     * stepped in front of the antenna). */
    pl_reset(&e, TARGET);
    t = 1000000;
    walk(&e, -45, -45, 20, 0, &t);   /* establish a strong peak */
    walk(&e, -75, -75, 6, 0, &t);    /* sudden 30 dB drop */
    pl_evaluate(&e, &v);
    CHECK(v.confidence < 80, "confidence docked after a deep fade (got %u)", v.confidence);
    CHECK_EQ(v.rssi_peak, -45);

    /* A brief dip does not instantly flip a committed HOTTER to COLDER: the
     * confirmation window absorbs one or two bad samples. */
    pl_reset(&e, TARGET);
    t = 1000000;
    walk(&e, -80, -45, 30, 0, &t);   /* clearly warming */
    pl_evaluate(&e, &v);
    const pl_trend_t before = v.trend;
    pl_observe(&e, TARGET, -70, t); t += 200000; /* one bad sample */
    pl_observe(&e, TARGET, -46, t); t += 200000; /* back on track */
    pl_evaluate(&e, &v);
    CHECK(before == PL_TREND_HOTTER, "was warming");
    CHECK(v.trend != PL_TREND_COLDER, "one dip does not flip to colder");

    /* Vocabulary. */
    for (int tr = PL_TREND_COLDER; tr <= PL_TREND_HERE; tr++) {
        CHECK(pl_trend_name((pl_trend_t)tr)[0] != '?', "trend %d named", tr);
        CHECK(pl_trend_advice((pl_trend_t)tr)[0] != '\0', "trend %d advised", tr);
    }
    /* Honesty: the advice must never promise distance in metres. */
    for (int tr = PL_TREND_COLDER; tr <= PL_TREND_HERE; tr++) {
        const char *a = pl_trend_advice((pl_trend_t)tr);
        CHECK(strstr(a, "metre") == NULL && strstr(a, "meter") == NULL,
              "advice never claims a distance");
    }
    CHECK(strstr(pl_trend_advice(PL_TREND_HERE), "not distance") != NULL,
          "the HERE advice states RSSI is not distance");
}
