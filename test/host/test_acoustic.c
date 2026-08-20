/* Pharos host tests - the acoustic engine.
 *
 * Written to attack it. Every positive has a benign twin that must NOT fire:
 *
 *   a loud room       -> broadband energy must not read as a beacon
 *   a one-off clatter -> a tone that does not repeat must not reach the top
 *   a dead microphone -> silence must be reported as DEAF, never as QUIET,
 *                        because "I heard nothing" and "I cannot hear" are
 *                        different answers and only one of them is reassuring
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "pharos_acoustic.h"
#include "test_support.h"

#define RATE 48000u
#define WIN  480u /* 10 ms */

/* A window of one pure tone at `hz`, amplitude 0..32767. */
static void tone(int16_t *buf, unsigned n, double hz, double amp, double *phase)
{
    for (unsigned i = 0; i < n; i++) {
        buf[i] = (int16_t)(amp * sin(*phase));
        *phase += 2.0 * M_PI * hz / (double)RATE;
        if (*phase > 2.0 * M_PI) *phase -= 2.0 * M_PI;
    }
}

/* Deterministic broadband noise - a room, a fan, a hand on a desk. */
static void noise(int16_t *buf, unsigned n, double amp, uint32_t *rng)
{
    for (unsigned i = 0; i < n; i++) {
        *rng ^= *rng << 13; *rng ^= *rng >> 17; *rng ^= *rng << 5;
        const double u = ((double)(*rng % 20001u) - 10000.0) / 10000.0;
        buf[i] = (int16_t)(amp * u);
    }
}

static void silence(int16_t *buf, unsigned n) { memset(buf, 0, n * sizeof(int16_t)); }

/* ---------------------------------------------------------------------- */

static void test_acoustic_quiet_room(void)
{
    banner("acoustic: a quiet room is quiet, not a finding");
    pac_engine_t e; pac_verdict_t v;
    pac_reset(&e);
    int16_t buf[WIN];
    uint32_t rng = 1;
    double ph = 0;
    /* Faint audible life, nothing ultrasonic. */
    for (int i = 0; i < 24; i++) {
        noise(buf, WIN, 300.0, &rng);
        tone(buf, WIN, 1000.0, 0.0, &ph); /* keeps phase advancing */
        pac_observe(&e, buf, WIN, RATE);
    }
    pac_evaluate(&e, &v);
    CHECK(v.band <= PAC_BAND_TRACE, "a quiet room must not alarm (got %s/%u)",
          pac_band_name(v.band), v.score);
    CHECK((v.families & PAC_FAM_PERSISTENT) == 0, "nothing persistent here");
}

/* THE ONE THAT MATTERS. A real 19 kHz beacon, repeating. */
static void test_acoustic_beacon(void)
{
    banner("acoustic: a repeating 19 kHz tone is a beacon");
    pac_engine_t e; pac_verdict_t v;
    pac_reset(&e);
    int16_t buf[WIN];
    uint32_t rng = 7;
    double ph = 0;

    /* Learn the room first, so the tone has a floor to stand out from. */
    for (int i = 0; i < 12; i++) {
        noise(buf, WIN, 200.0, &rng);
        pac_observe(&e, buf, WIN, RATE);
    }
    /* Then the beacon, present in most windows the way a repeating tone is. */
    for (int i = 0; i < 28; i++) {
        noise(buf, WIN, 200.0, &rng);
        if (i % 4 != 3) {
            int16_t t[WIN];
            tone(t, WIN, 19000.0, 6000.0, &ph);
            for (unsigned k = 0; k < WIN; k++) buf[k] = (int16_t)(buf[k] + t[k]);
        }
        pac_observe(&e, buf, WIN, RATE);
    }
    pac_evaluate(&e, &v);

    CHECK(v.strongest == PAC_BAND_19K, "the right band was picked (got %s)",
          pac_probe_name(v.strongest));
    CHECK(v.band >= PAC_BAND_PRESENT, "a real beacon must register (got %s/%u)",
          pac_band_name(v.band), v.score);
    CHECK(v.families & PAC_FAM_NARROW, "and it must read as a TONE");
    CHECK(v.duty_pct >= 40, "and as repeating (duty %u%%)", v.duty_pct);
    CHECK(v.score <= v.ceiling, "never above its own ceiling");
}

/* THE NEGATIVE THAT JUSTIFIES THE NARROW FAMILY. Broadband noise lifts every
 * probe at once. That is a room, not a beacon, and calling it one would make
 * the whole lens worthless in any real building. */
static void test_acoustic_broadband_is_not_a_beacon(void)
{
    banner("acoustic: a loud room is not an inaudible beacon");
    pac_engine_t e; pac_verdict_t v;
    pac_reset(&e);
    int16_t buf[WIN];
    uint32_t rng = 99;

    for (int i = 0; i < 12; i++) { noise(buf, WIN, 150.0, &rng); pac_observe(&e, buf, WIN, RATE); }
    for (int i = 0; i < 28; i++) { noise(buf, WIN, 9000.0, &rng); pac_observe(&e, buf, WIN, RATE); }
    pac_evaluate(&e, &v);

    CHECK(v.band < PAC_BAND_BEACON,
          "broadband noise must never reach BEACON (got %s/%u, narrow=%u)",
          pac_band_name(v.band), v.score, v.c_narrow);
}

/* A tone that happens once. Persistence is what separates a beacon from a
 * chair leg, so one burst must not reach the top band however loud it is. */
static void test_acoustic_one_shot_is_not_persistent(void)
{
    banner("acoustic: a tone that does not repeat is not a beacon");
    pac_engine_t e; pac_verdict_t v;
    pac_reset(&e);
    int16_t buf[WIN];
    uint32_t rng = 3;
    double ph = 0;

    for (int i = 0; i < 12; i++) { noise(buf, WIN, 200.0, &rng); pac_observe(&e, buf, WIN, RATE); }
    for (int i = 0; i < 28; i++) {
        noise(buf, WIN, 200.0, &rng);
        if (i == 5) { /* exactly one window carries the tone */
            int16_t t[WIN];
            tone(t, WIN, 19000.0, 12000.0, &ph);
            for (unsigned k = 0; k < WIN; k++) buf[k] = (int16_t)(buf[k] + t[k]);
        }
        pac_observe(&e, buf, WIN, RATE);
    }
    pac_evaluate(&e, &v);
    CHECK(v.band < PAC_BAND_BEACON, "one burst is not a beacon (got %s/%u)",
          pac_band_name(v.band), v.score);
    CHECK(v.duty_pct < 40, "and its duty is low (%u%%)", v.duty_pct);
}

/* A dead microphone must say DEAF. "I heard nothing" and "I cannot hear" are
 * different answers, and reporting the second as the first is the single most
 * dishonest thing a detector can do. */
static void test_acoustic_deaf_is_not_quiet(void)
{
    banner("acoustic: silence is reported as deaf, never as all-clear");
    pac_engine_t e; pac_verdict_t v;
    pac_reset(&e);
    int16_t buf[WIN];
    for (int i = 0; i < 20; i++) { silence(buf, WIN); pac_observe(&e, buf, WIN, RATE); }
    pac_evaluate(&e, &v);
    CHECK(v.notes & PAC_NOTE_DEAF, "the dead capture path is disclosed");
    CHECK_EQ(v.ceiling, 0);
    CHECK_EQ(v.score, 0);
}

/* Above Nyquist a probe cannot mean anything, and the engine must not pretend
 * otherwise - the 21 kHz probe is near the edge of what this path can hear. */
static void test_acoustic_edge_of_hearing_is_capped(void)
{
    banner("acoustic: the top probe cannot carry a verdict on its own");
    pac_engine_t e; pac_verdict_t v;
    pac_reset(&e);
    int16_t buf[WIN];
    uint32_t rng = 5;
    double ph = 0;

    for (int i = 0; i < 12; i++) { noise(buf, WIN, 200.0, &rng); pac_observe(&e, buf, WIN, RATE); }
    for (int i = 0; i < 28; i++) {
        noise(buf, WIN, 200.0, &rng);
        int16_t t[WIN];
        tone(t, WIN, 21000.0, 9000.0, &ph);
        for (unsigned k = 0; k < WIN; k++) buf[k] = (int16_t)(buf[k] + t[k]);
        pac_observe(&e, buf, WIN, RATE);
    }
    pac_evaluate(&e, &v);
    if (v.strongest == PAC_BAND_21K) {
        CHECK(v.notes & PAC_NOTE_EDGE_OF_HEARING, "the limit is disclosed");
        CHECK(v.score <= 62, "and it is capped there (got %u)", v.score);
    }
}

static void test_acoustic_vocabulary(void)
{
    banner("acoustic: the vocabulary promises nothing it cannot keep");
    for (int b = PAC_BAND_QUIET; b <= PAC_BAND_BEACON; b++) {
        const char *n = pac_band_name((pac_verdict_band_t)b);
        const char *h = pac_band_hint((pac_verdict_band_t)b);
        CHECK(n && *n, "band %d named", b);
        CHECK(h && *h, "band %d has a hint", b);
        CHECK(strlen(h) <= 34, "hint fits the glass: \"%s\" (%u)", h,
              (unsigned)strlen(h));
        CHECK(strstr(n, "SAFE") == NULL, "no band claims safety");
        CHECK(strstr(n, "CLEAR") == NULL, "no band claims all-clear");
    }
    for (int b = 0; b < PAC_BAND_COUNT; b++) {
        CHECK(pac_probe_hz((pac_band_t)b) > 0, "probe %d has a frequency", b);
        CHECK(pac_probe_name((pac_band_t)b)[0] != '?', "probe %d is named", b);
    }
}

/* No verdict, in any configuration, may exceed its own stated ceiling. */
static void test_acoustic_ceiling_never_exceeded(void)
{
    banner("acoustic: no verdict exceeds its own ceiling");
    uint32_t rng = 11;
    for (int amp = 0; amp <= 16000; amp += 1700) {
        for (int hz = 18000; hz <= 21000; hz += 1000) {
            pac_engine_t e; pac_verdict_t v;
            pac_reset(&e);
            int16_t buf[WIN];
            double ph = 0;
            for (int i = 0; i < 40; i++) {
                noise(buf, WIN, 200.0, &rng);
                int16_t t[WIN];
                tone(t, WIN, (double)hz, (double)amp, &ph);
                for (unsigned k = 0; k < WIN; k++) buf[k] = (int16_t)(buf[k] + t[k]);
                pac_observe(&e, buf, WIN, RATE);
            }
            pac_evaluate(&e, &v);
            CHECK(v.score <= v.ceiling, "score %u > ceiling %u at %d Hz amp %d",
                  v.score, v.ceiling, hz, amp);
            CHECK(v.score <= 100, "in range");
        }
    }
}

void test_acoustic(void)
{
    test_acoustic_quiet_room();
    test_acoustic_beacon();
    test_acoustic_broadband_is_not_a_beacon();
    test_acoustic_one_shot_is_not_persistent();
    test_acoustic_deaf_is_not_quiet();
    test_acoustic_edge_of_hearing_is_capped();
    test_acoustic_vocabulary();
    test_acoustic_ceiling_never_exceeded();
}
