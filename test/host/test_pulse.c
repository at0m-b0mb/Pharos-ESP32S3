/* The shared activity ribbon. */
#include <string.h>

#include "pharos_pulse.h"
#include "test_support.h"

#define SEC 1000000ull

static void test_pulse_shape(void)
{
    banner("pulse: the ribbon carries shape, not rate");

    pharos_pulse_t p;
    pharos_pulse_reset(&p);

    uint8_t out[PHAROS_PULSE_SLOTS];
    CHECK(!pharos_pulse_fill(&p, 10 * SEC, out),
          "a lens that measured nothing draws no ribbon at all");

    /* A burst in one second, silence around it. */
    pharos_pulse_add(&p, 5 * SEC, 40);
    CHECK(pharos_pulse_fill(&p, 5 * SEC, out), "once fed, it draws");

    /* The newest slot is the last one. */
    CHECK(out[PHAROS_PULSE_SLOTS - 1] == 255,
          "the busiest second is full scale");
    CHECK(out[0] == 0, "and the empty ones are empty");

    /* SCALED TO ITSELF. Forty events and four hundred produce the same
     * picture, because the ribbon answers "what shape" and the score already
     * answers "how much". */
    pharos_pulse_t q;
    pharos_pulse_reset(&q);
    pharos_pulse_add(&q, 5 * SEC, 400);
    uint8_t out2[PHAROS_PULSE_SLOTS];
    pharos_pulse_fill(&q, 5 * SEC, out2);
    CHECK(memcmp(out, out2, sizeof(out)) == 0,
          "ten times the volume draws the same shape");
}

static void test_pulse_silence_is_a_reading(void)
{
    banner("pulse: measured-and-quiet is not the same as never-measured");

    pharos_pulse_t p;
    pharos_pulse_reset(&p);
    pharos_pulse_add(&p, 1 * SEC, 5);

    uint8_t out[PHAROS_PULSE_SLOTS];
    /* Long enough later that the burst has rolled out of the window. */
    CHECK(pharos_pulse_fill(&p, 400 * SEC, out),
          "a lens that HAS measured still draws its ribbon when quiet");
    for (unsigned i = 0; i < PHAROS_PULSE_SLOTS; i++) {
        CHECK_EQ(out[i], 0);
    }
}

static void test_pulse_rolls(void)
{
    banner("pulse: seconds that passed in silence show as silence");

    pharos_pulse_t p;
    pharos_pulse_reset(&p);

    /* Steady one-per-second for the whole window. */
    for (unsigned i = 0; i < PHAROS_PULSE_SLOTS; i++) {
        pharos_pulse_add(&p, (uint64_t)(100 + i) * SEC, 10);
    }
    uint8_t out[PHAROS_PULSE_SLOTS];
    pharos_pulse_fill(&p, (uint64_t)(100 + PHAROS_PULSE_SLOTS - 1) * SEC, out);
    for (unsigned i = 0; i < PHAROS_PULSE_SLOTS; i++) {
        CHECK_EQ(out[i], 255);
    }

    /* Now let half the window pass with nothing. The old traffic must slide
     * out rather than sit there being redrawn as if it were current. */
    const unsigned half = PHAROS_PULSE_SLOTS / 2u;
    pharos_pulse_fill(&p, (uint64_t)(100 + PHAROS_PULSE_SLOTS - 1 + half) * SEC,
                      out);
    unsigned quiet = 0;
    for (unsigned i = 0; i < PHAROS_PULSE_SLOTS; i++) {
        if (out[i] == 0) quiet++;
    }
    CHECK(quiet >= half, "the silent seconds are silent (%u)", quiet);
    CHECK(out[PHAROS_PULSE_SLOTS - 1] == 0, "and the newest is one of them");
}

static void test_pulse_long_gap(void)
{
    banner("pulse: an idle lens does not roll four billion slots");

    pharos_pulse_t p;
    pharos_pulse_reset(&p);
    pharos_pulse_add(&p, 10 * SEC, 99);

    uint8_t out[PHAROS_PULSE_SLOTS];
    /* An hour later. This must terminate promptly and show nothing. */
    pharos_pulse_fill(&p, 3600 * SEC, out);
    for (unsigned i = 0; i < PHAROS_PULSE_SLOTS; i++) {
        CHECK_EQ(out[i], 0);
    }

    /* And it must still work afterwards. */
    pharos_pulse_add(&p, 3600 * SEC, 7);
    CHECK(pharos_pulse_fill(&p, 3600 * SEC, out), "still live");
    CHECK(out[PHAROS_PULSE_SLOTS - 1] == 255, "and counting again");
}

static void test_pulse_backwards_clock(void)
{
    banner("pulse: a clock that goes backwards restarts rather than hangs");

    pharos_pulse_t p;
    pharos_pulse_reset(&p);
    pharos_pulse_add(&p, 500 * SEC, 10);
    pharos_pulse_add(&p, 5 * SEC, 20); /* backwards */

    uint8_t out[PHAROS_PULSE_SLOTS];
    CHECK(pharos_pulse_fill(&p, 5 * SEC, out), "it survives");
    CHECK(out[PHAROS_PULSE_SLOTS - 1] == 255, "and takes the new time as now");
}

static void test_pulse_peak(void)
{
    banner("pulse: the busiest second is reportable");

    pharos_pulse_t p;
    pharos_pulse_reset(&p);
    pharos_pulse_add(&p, 10 * SEC, 3);
    pharos_pulse_add(&p, 11 * SEC, 31);
    pharos_pulse_add(&p, 12 * SEC, 8);
    CHECK_EQ(pharos_pulse_peak(&p, 12 * SEC), 31);
    CHECK_EQ(pharos_pulse_peak(&p, 4000 * SEC), 0);
}

static void test_pulse_saturates(void)
{
    banner("pulse: a very loud second saturates instead of wrapping");

    pharos_pulse_t p;
    pharos_pulse_reset(&p);
    for (unsigned i = 0; i < 10u; i++) {
        pharos_pulse_add(&p, 9 * SEC, 60000u);
    }
    CHECK_EQ(pharos_pulse_peak(&p, 9 * SEC), 0xFFFFu);
}

void test_pulse(void)
{
    test_pulse_shape();
    test_pulse_silence_is_a_reading();
    test_pulse_rolls();
    test_pulse_long_gap();
    test_pulse_backwards_clock();
    test_pulse_peak();
    test_pulse_saturates();
}
