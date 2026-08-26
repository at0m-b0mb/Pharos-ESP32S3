/* Attribution: who actually sent the forged frames, and what were they running.
 *
 * The negative tests are the point of this suite. An engine that names a
 * device is an engine that can name the WRONG device, and pointing at an
 * innocent radio is a worse failure than saying nothing at all - so most of
 * what follows asserts a refusal rather than an answer.
 */
#include <string.h>

#include "pharos_attrib.h"
#include "test_support.h"

static void mk(uint8_t out[6], uint8_t last)
{
    const uint8_t base[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x00 };
    memcpy(out, base, 6);
    out[5] = last;
}

static void feed(pat_engine_t *e, uint8_t last, int8_t rssi, unsigned n)
{
    uint8_t m[6];
    mk(m, last);
    for (unsigned i = 0; i < n; i++) {
        pat_observe(e, m, rssi);
    }
}

static void forge(pat_engine_t *e, int8_t rssi, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        pat_observe_forged(e, rssi, (uint16_t)(100 + i), (uint16_t)(0x2C + i), 7);
    }
}

static void test_attrib_names_the_only_match(void)
{
    banner("attrib: one radio at the forgery's level is a lead");

    pat_engine_t e;
    pat_reset(&e);

    /* A room: the AP itself, a laptop far away, and one device close. */
    uint8_t ap[6];
    mk(ap, 0xAA);
    feed(&e, 0xAA, -70, 40);  /* the AP, which the frames will impersonate */
    feed(&e, 0xBB, -80, 20);  /* something distant */
    feed(&e, 0xCC, -45, 30);  /* something close - the attacker's own radio */

    forge(&e, -45, 16);       /* forged deauths arrive at the close level */

    pat_verdict_t v;
    pat_evaluate(&e, ap, &v);

    CHECK(v.have_lead, "a unique level match is reported");
    uint8_t want[6];
    mk(want, 0xCC);
    CHECK(memcmp(v.lead_mac, want, 6) == 0, "and it is the radio that matches");
    CHECK(v.candidates == 1, "exactly one candidate");
    CHECK(!v.ambiguous, "not ambiguous");
    CHECK(v.confidence > 0 && v.confidence <= PAT_MAX_CONFIDENCE,
          "confidence is inside its ceiling");
    CHECK(v.confidence < 100, "and never certain");
}

static void test_attrib_refuses_a_crowded_room(void)
{
    banner("attrib: several radios at one level is not evidence");

    pat_engine_t e;
    pat_reset(&e);

    uint8_t ap[6];
    mk(ap, 0xAA);
    feed(&e, 0xAA, -70, 40);

    /* Four devices all sitting around the same level. Every one of them is
     * an equally good "match", which is the same as none of them being one. */
    feed(&e, 0xC1, -45, 30);
    feed(&e, 0xC2, -46, 30);
    feed(&e, 0xC3, -44, 30);
    feed(&e, 0xC4, -45, 30);

    forge(&e, -45, 16);

    pat_verdict_t v;
    pat_evaluate(&e, ap, &v);

    CHECK(!v.have_lead, "a crowded room produces no accusation");
    CHECK(v.ambiguous, "and says so");
    CHECK(v.candidates == 4, "reporting how many it could not choose between");
}

static void test_attrib_ignores_the_spoofed_address(void)
{
    banner("attrib: matching the address being impersonated proves nothing");

    pat_engine_t e;
    pat_reset(&e);

    /* The only radio in the room is the AP, and the forgery arrives at the
     * AP's own level. Naming the AP would be rediscovering the forgery and
     * calling it a lead. */
    uint8_t ap[6];
    mk(ap, 0xAA);
    feed(&e, 0xAA, -50, 40);
    forge(&e, -50, 16);

    pat_verdict_t v;
    pat_evaluate(&e, ap, &v);

    CHECK(!v.have_lead, "the impersonated address is never the answer");
    CHECK(v.candidates == 0, "and is not counted as a candidate");
}

static void test_attrib_ignores_a_radio_heard_once(void)
{
    banner("attrib: one frame is not a level profile");

    pat_engine_t e;
    pat_reset(&e);

    uint8_t ap[6];
    mk(ap, 0xAA);
    feed(&e, 0xAA, -70, 40);
    feed(&e, 0xCC, -45, 1); /* heard exactly once */

    forge(&e, -45, 16);

    pat_verdict_t v;
    pat_evaluate(&e, ap, &v);

    CHECK(!v.have_lead, "a single frame cannot identify anybody");
}

static void test_attrib_a_wandering_forgery_is_weak(void)
{
    banner("attrib: a level that wanders is not something to match against");

    pat_engine_t tight, wide;
    pat_reset(&tight);
    pat_reset(&wide);

    uint8_t ap[6];
    mk(ap, 0xAA);
    for (pat_engine_t *e = &tight; ; e = &wide) {
        feed(e, 0xAA, -70, 40);
        feed(e, 0xCC, -45, 30);
        if (e == &wide) break;
    }

    for (unsigned i = 0; i < 16; i++) {
        pat_observe_forged(&tight, -45, (uint16_t)i, (uint16_t)(0x2C + i), 7);
        /* the same mean, spread across 14 dB */
        const int8_t r = (int8_t)((i & 1u) ? -38 : -52);
        pat_observe_forged(&wide, r, (uint16_t)i, (uint16_t)(0x2C + i), 7);
    }

    pat_verdict_t a, b;
    pat_evaluate(&tight, ap, &a);
    pat_evaluate(&wide, ap, &b);

    CHECK(a.confidence > b.confidence,
          "a steady level is worth more than a wandering one");
    CHECK(b.forged_spread > a.forged_spread, "and the spread is reported");
}

static void test_attrib_tools(void)
{
    banner("attrib: what the attacker is running");

    /* The published Marauder / Evil-M5 pair. */
    {
        pat_engine_t e;
        pat_reset(&e);
        for (unsigned i = 0; i < 8; i++) {
            pat_observe_forged(&e, -45, 0xFFFu, 0x013Au, 7);
        }
        pat_verdict_t v;
        pat_evaluate(&e, 0, &v);
        CHECK(v.tool == PAT_TOOL_MARAUDER, "seq 0xFFF with duration 0x013A");
    }

    /* A constant duration where a computed value belongs. */
    {
        pat_engine_t e;
        pat_reset(&e);
        for (unsigned i = 0; i < 8; i++) {
            pat_observe_forged(&e, -45, (uint16_t)(200 + i), 0x0000u, 7);
        }
        pat_verdict_t v;
        pat_evaluate(&e, 0, &v);
        CHECK(v.tool == PAT_TOOL_TEMPLATED, "a constant duration is a template");
    }

    /* An injector that never sets the sequence number. */
    {
        pat_engine_t e;
        pat_reset(&e);
        for (unsigned i = 0; i < 8; i++) {
            pat_observe_forged(&e, -45, 0u, (uint16_t)(0x2C + i), 7);
        }
        pat_verdict_t v;
        pat_evaluate(&e, 0, &v);
        CHECK(v.tool == PAT_TOOL_NO_SEQUENCE, "sequence never set");
    }

    /* A REAL access point: both fields vary. This is the negative test the
     * others exist to be contrasted with. */
    {
        pat_engine_t e;
        pat_reset(&e);
        for (unsigned i = 0; i < 12; i++) {
            pat_observe_forged(&e, -45, (uint16_t)(300 + i),
                               (uint16_t)(0x2C + i * 3u), 7);
        }
        pat_verdict_t v;
        pat_evaluate(&e, 0, &v);
        CHECK(v.tool == PAT_TOOL_UNKNOWN,
              "varying fields are not accused of being a template");
    }

    /* One frame is constant by definition; it must not be a finding. */
    {
        pat_engine_t e;
        pat_reset(&e);
        pat_observe_forged(&e, -45, 500u, 0x0000u, 7);
        pat_verdict_t v;
        pat_evaluate(&e, 0, &v);
        CHECK(v.tool == PAT_TOOL_UNKNOWN, "one frame proves nothing constant");
    }
}

static void test_attrib_empty(void)
{
    banner("attrib: nothing forged, nothing claimed");

    pat_engine_t e;
    pat_reset(&e);
    feed(&e, 0xCC, -45, 30);

    pat_verdict_t v;
    pat_evaluate(&e, 0, &v);
    CHECK(!v.have_lead, "no forgery means no lead");
    CHECK(v.tool == PAT_TOOL_UNKNOWN, "and no tool");
    CHECK(v.candidates == 0, "and no candidates");
}

void test_attrib(void)
{
    test_attrib_names_the_only_match();
    test_attrib_refuses_a_crowded_room();
    test_attrib_ignores_the_spoofed_address();
    test_attrib_ignores_a_radio_heard_once();
    test_attrib_a_wandering_forgery_is_weak();
    test_attrib_tools();
    test_attrib_empty();
}
