/* Pharos host tests, part three: probe-request privacy and the power planner. */
#include <string.h>

#include "pharos_event.h"
#include "pharos_power.h"
#include "pharos_probe.h"
#include "test_support.h"

/* ----------------------------------------------------------------- probe */

static pp_probe_t mk_probe(const uint8_t *addr, const char *ssid, uint16_t seq,
                           uint32_t fp, uint64_t t_us)
{
    pp_probe_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.addr, addr, 6);
    if (ssid) {
        p.ssid_len = (uint8_t)strlen(ssid);
        memcpy(p.ssid, ssid, p.ssid_len);
    }
    p.seq = seq;
    p.fingerprint = fp;
    p.rssi = -55;
    p.t_us = t_us;
    return p;
}

static void feed(pp_engine_t *e, const uint8_t *addr, const char *const *ssids,
                 uint16_t *seq, uint32_t fp)
{
    for (unsigned i = 0; ssids[i]; i++) {
        /* Read the sequence number once, then advance it: reading *seq twice
         * in one expression (once bumped, once for the timestamp) is a
         * sequence-point UB that gcc rightly rejects even though clang allows
         * it. Separate statements, defined behaviour. */
        const uint16_t s = (*seq)++;
        pp_probe_t p = mk_probe(addr, ssids[i], s, fp, 1000000ull * s);
        pp_observe(e, &p);
    }
}

void test_probe_classify(void)
{
    banner("probe: what a network name gives away");

    CHECK_EQ(pp_classify("Starbucks WiFi", 14), PP_PLACE_RETAIL);
    CHECK_EQ(pp_classify("eduroam", 7), PP_PLACE_EDUCATION);
    CHECK_EQ(pp_classify("Premier Inn Guest", 17), PP_PLACE_HOSPITALITY);
    CHECK_EQ(pp_classify("Heathrow Airport WiFi", 21), PP_PLACE_TRANSIT);
    CHECK_EQ(pp_classify("BTWiFi-with-FON", 15), PP_PLACE_TELECOM);
    CHECK_EQ(pp_classify("Tesla Model 3", 13), PP_PLACE_VEHICLE);
    CHECK_EQ(pp_classify("BTHub6-K9WQ", 11), PP_PLACE_HOME);
    CHECK_EQ(pp_classify("Guest", 5), PP_PLACE_GENERIC);
    CHECK_EQ(pp_classify("", 0), PP_PLACE_UNKNOWN);
    CHECK_EQ(pp_classify("kZq7", 4), PP_PLACE_UNKNOWN);

    /* Case insensitivity, because SSIDs are typed by humans. */
    CHECK_EQ(pp_classify("STARBUCKS", 9), PP_PLACE_RETAIL);
    CHECK_EQ(pp_classify("eduROAM", 7), PP_PLACE_EDUCATION);

    /* When a name matches two tables, the honest answer is the one that
     * narrows a person down the most. */
    CHECK_EQ(pp_classify("NHS Guest WiFi", 14), PP_PLACE_HEALTHCARE);
    CHECK_EQ(pp_classify("Acme Corp Guest", 15), PP_PLACE_WORKPLACE);

    /* Weights must be ordered by how revealing a place is. */
    CHECK(PP_PLACE_HEALTHCARE > PP_PLACE_RETAIL, "healthcare outranks a cafe");
    CHECK(PP_PLACE_HOME > PP_PLACE_TRANSIT, "a home outranks an airport");

    /* No read past a short buffer when the needle is longer than the name. */
    CHECK_EQ(pp_classify("a", 1), PP_PLACE_UNKNOWN);
    CHECK_EQ(pp_classify("Starbucks", 4), PP_PLACE_UNKNOWN); /* only "Star" visible */
}

void test_probe_link_needs_recency(void)
{
    banner("probe: a link across a long gap is a coincidence, not a rotation");

    /* THE FALSE LINK.
     *
     * The de-randomisation test was a fingerprint match plus a sequence
     * counter landing within 64 of where the old address stopped - with no
     * bound on WHEN. A 12-bit counter has 4096 values, so an unrelated device
     * lands in that window 1.6% of the time by chance alone.
     *
     * And the physics runs the other way too: a device that last probed ten
     * minutes ago has been counting all the while, so its counter is now
     * thousands of steps on. A near-match after that gap cannot be the same
     * device continuing.
     *
     * A false link asserts that two people are one person, which is a worse
     * error than missing a link. */
    const uint8_t a[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    const uint8_t b[6] = { 0x06, 0x99, 0x88, 0x77, 0x66, 0x55 };
    const uint32_t fp = 0xABCDEF01u;

    pp_engine_t e;
    pp_reset(&e);
    pp_probe_t p1 = mk_probe(a, "Cafe", 100, fp, 1000000ull);
    pp_observe(&e, &p1);

    /* Ten minutes later, a new address whose counter happens to land close. */
    pp_probe_t late = mk_probe(b, "Cafe", 140, fp,
                               1000000ull + 600ull * 1000000ull);
    pp_observe(&e, &late);
    CHECK(e.n_devices == 2,
          "a ten-minute gap is two devices, not one rotating (%u)",
          e.n_devices);

    /* THE REAL ROTATION, which must still be caught. */
    pp_engine_t r;
    pp_reset(&r);
    pp_probe_t q1 = mk_probe(a, "Cafe", 100, fp, 1000000ull);
    pp_observe(&r, &q1);
    pp_probe_t q2 = mk_probe(b, "Cafe", 140, fp, 1000000ull + 3000000ull);
    pp_observe(&r, &q2);

    CHECK(r.n_devices == 1, "a fast rotation is still linked (%u)", r.n_devices);
    CHECK(r.devices[0].identities == 2, "and both addresses counted (%u)",
          r.devices[0].identities);
}

void test_probe_grading(void)
{
    banner("probe: grading a device's exposure");
    pp_engine_t e;
    pp_verdict_t v;
    uint16_t seq;

    /* A quiet device: wildcard probes only. The only route to an A+. */
    pp_reset(&e);
    const uint8_t quiet[6] = { 0x9A, 0x11, 0x22, 0x33, 0x44, 0x55 }; /* LA bit set */
    seq = 100;
    for (unsigned i = 0; i < 8; i++) {
        pp_probe_t p = mk_probe(quiet, NULL, seq++, 0xAAAA1111u, 1000000ull * i);
        pp_observe(&e, &p);
    }
    pp_grade_device(&e.devices[0], &v);
    CHECK_EQ(v.grade, PP_GRADE_A_PLUS);
    CHECK_EQ(v.networks, 0);
    CHECK_EQ(v.exposure, 0);
    CHECK_EQ(e.wildcards_seen, 8);

    /* Silence is not a grade if we barely listened. */
    pp_reset(&e);
    pp_probe_t one = mk_probe(quiet, NULL, 1, 0xAAAA1111u, 0);
    pp_observe(&e, &one);
    pp_grade_device(&e.devices[0], &v);
    CHECK_EQ(v.grade, PP_GRADE_UNGRADED);
    CHECK(v.notes & PP_NOTE_THIN, "thin observation disclosed");

    /* Volume of harmless names is not the same as exposure. */
    pp_reset(&e);
    seq = 200;
    static const char *const harmless[] = {
        "Starbucks WiFi", "Costa Free WiFi", "BTWiFi-with-FON", "Guest", NULL
    };
    feed(&e, quiet, harmless, &seq, 0xAAAA1111u);
    pp_grade_device(&e.devices[0], &v);
    CHECK_EQ(v.networks, 4);
    CHECK(v.grade <= PP_GRADE_B, "four coffee shops is not a bad grade (got %s)",
          pp_grade_name(v.grade));

    /* Specificity is. Three names, and the room now knows where this person
     * works, sleeps and was treated. */
    pp_reset(&e);
    seq = 300;
    static const char *const revealing[] = {
        "NHS Guest", "Acme Corp Internal", "The Robinsons 5GHz", "Marriott_GUEST", NULL
    };
    feed(&e, quiet, revealing, &seq, 0xBBBB2222u);
    pp_grade_device(&e.devices[0], &v);
    CHECK_EQ(v.narrowest, PP_PLACE_HEALTHCARE);
    CHECK(v.grade >= PP_GRADE_C, "revealing names must grade badly (got %s, exposure %u)",
          pp_grade_name(v.grade), v.exposure);
    CHECK(strstr(v.headline, "identifies its owner") != NULL, "headline: %s", v.headline);
    CHECK(strstr(pp_grade_advice(&v), "Forget") != NULL, "advice is actionable");

    /* Randomisation defeated: a new address, the same chipset fingerprint,
     * and a sequence counter that carried straight on. */
    pp_reset(&e);
    const uint8_t first[6]  = { 0x9A, 0x01, 0x02, 0x03, 0x04, 0x05 };
    const uint8_t second[6] = { 0x9E, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
    seq = 400;
    static const char *const some[] = { "Starbucks WiFi", "Guest", NULL };
    feed(&e, first, some, &seq, 0xC0FFEEu);
    seq += 3; /* a few frames we did not hear */
    static const char *const more[] = { "Costa Free WiFi", "eduroam", NULL };
    feed(&e, second, more, &seq, 0xC0FFEEu);

    CHECK_EQ(e.n_devices, 1);
    pp_grade_device(&e.devices[0], &v);
    CHECK_EQ(v.identities, 2);
    CHECK(v.notes & PP_NOTE_RELINKED, "the address change was followed through");
    CHECK_EQ(v.networks, 4);
    CHECK(strstr(pp_grade_advice(&v), "names are the leak") != NULL,
          "advice explains that forgetting networks is the real fix");

    /* A different chipset must not be folded in, however close the sequence. */
    pp_reset(&e);
    seq = 500;
    feed(&e, first, some, &seq, 0x11111111u);
    feed(&e, second, more, &seq, 0x22222222u);
    CHECK_EQ(e.n_devices, 2);

    /* Nor a matching fingerprint whose counter went nowhere near. */
    pp_reset(&e);
    seq = 600;
    feed(&e, first, some, &seq, 0x33333333u);
    seq = 3000;
    feed(&e, second, more, &seq, 0x33333333u);
    CHECK_EQ(e.n_devices, 2);

    /* A device that never randomises cannot grade above D, however little it
     * says: the names are optional, the address is not. */
    pp_reset(&e);
    const uint8_t fixed[6] = { 0x3C, 0x22, 0xFB, 0x01, 0x02, 0x03 }; /* no LA bit */
    seq = 700;
    static const char *const trivial[] = { "Guest", "Starbucks WiFi", NULL };
    feed(&e, fixed, trivial, &seq, 0xD00Du);
    pp_probe_t pad1 = mk_probe(fixed, NULL, seq++, 0xD00Du, 9000000ull);
    pp_probe_t pad2 = mk_probe(fixed, NULL, seq++, 0xD00Du, 9100000ull);
    pp_observe(&e, &pad1);
    pp_observe(&e, &pad2);
    pp_grade_device(&e.devices[0], &v);
    CHECK(v.notes & PP_NOTE_NO_RANDOM, "fixed address disclosed");
    CHECK(v.grade >= PP_GRADE_D, "a fixed address caps the grade (got %s)",
          pp_grade_name(v.grade));
    CHECK(strstr(pp_grade_advice(&v), "randomised") != NULL, "advice names the setting");

    /* Table limits are hit gracefully, never overrun. */
    pp_reset(&e);
    for (unsigned i = 0; i < PP_MAX_DEVICES + 8; i++) {
        uint8_t a[6] = { 0x9A, 0x00, 0x00, 0x00, (uint8_t)(i >> 8), (uint8_t)i };
        pp_probe_t p = mk_probe(a, "Guest", (uint16_t)(i * 7), 0x1000u + i, i * 1000ull);
        pp_observe(&e, &p);
    }
    CHECK_EQ(e.n_devices, PP_MAX_DEVICES);
    CHECK_EQ(e.probes_seen, PP_MAX_DEVICES + 8);

    pp_reset(&e);
    seq = 800;
    for (unsigned i = 0; i < PP_MAX_NETWORKS + 10; i++) {
        char name[16];
        snprintf(name, sizeof(name), "net-%02u", i);
        pp_probe_t p = mk_probe(quiet, name, seq++, 0x4444u, i * 1000ull);
        pp_observe(&e, &p);
    }
    CHECK_EQ(e.devices[0].n_networks, PP_MAX_NETWORKS);

    /* Vocabulary. */
    for (int g = PP_GRADE_UNGRADED; g <= PP_GRADE_F; g++) {
        const char *nm = pp_grade_name((pp_grade_t)g);
        CHECK(nm && *nm, "grade %d named", g);
    }
    for (int p = PP_PLACE_UNKNOWN; p < PP_PLACE_COUNT; p++) {
        const char *nm = pp_place_name((pp_place_t)p);
        CHECK(nm && *nm, "place %d named", p);
    }
}

/* ----------------------------------------------------------------- power */

void test_power(void)
{
    banner("power: predicting runtime before you launch");
    pwr_battery_t b = { .capacity_mah = 500, .soc_pct = 100, .mv = 4100,
                        .charging = false, .present = true };
    pwr_plan_t p;

    pwr_plan(&b, 135, PWR_SCREEN_ACTIVE, &p);
    CHECK_EQ(p.draw_ma, PWR_BASE_MA + 135 + PWR_SCREEN_ACTIVE_MA);
    CHECK(p.minutes > 0 && p.minutes < 300, "plausible active runtime: %u min", p.minutes);
    CHECK(p.estimated, "flagged as an estimate until the baseline is measured");
    CHECK(!p.insufficient, "a full pack is sufficient");

    /* Turning the screen off must always buy time, never cost it. */
    uint32_t prev = 0;
    const pwr_screen_t order[] = { PWR_SCREEN_ACTIVE, PWR_SCREEN_DIM, PWR_SCREEN_OFF };
    for (unsigned i = 0; i < 3; i++) {
        pwr_plan(&b, 135, order[i], &p);
        CHECK(p.minutes >= prev, "dimming never shortens runtime");
        prev = p.minutes;
    }

    /* Runtime falls with state of charge, monotonically. */
    prev = 0xFFFFFFFFu;
    for (int soc = 100; soc >= 0; soc -= 10) {
        b.soc_pct = (uint8_t)soc;
        pwr_plan(&b, 130, PWR_SCREEN_DIM, &p);
        CHECK(p.minutes <= prev, "runtime falls with charge at %d%%", soc);
        prev = p.minutes;
    }
    CHECK(p.insufficient, "an empty pack is reported insufficient");

    /* The prediction is deliberately pessimistic: never above nameplate. */
    b.soc_pct = 100;
    pwr_plan(&b, 0, PWR_SCREEN_OFF, &p);
    const uint32_t nameplate = ((uint32_t)b.capacity_mah * 60u) / PWR_BASE_MA;
    CHECK(p.minutes < nameplate, "estimate stays under the datasheet figure");

    /* No battery, or a nonsense pack, must not divide by zero. */
    pwr_battery_t none = { 0, 0, 0, false, false, false };
    pwr_plan(&none, 130, PWR_SCREEN_ACTIVE, &p);
    CHECK(p.insufficient, "absent battery reported");
    CHECK_EQ(p.minutes, 0);
    pwr_plan(NULL, 130, PWR_SCREEN_ACTIVE, &p);
    CHECK(p.insufficient, "null battery handled");

    /* Screen selection gives the brightest mode that still reaches a target. */
    b.capacity_mah = 500;
    b.soc_pct = 100;
    CHECK_EQ(pwr_best_screen(&b, 130, 30), PWR_SCREEN_ACTIVE);
    CHECK_EQ(pwr_best_screen(&b, 130, 8 * 60), PWR_SCREEN_OFF);
    CHECK_EQ(pwr_best_screen(&b, 130, 1000000), PWR_SCREEN_OFF);

    /* Formatting. */
    char buf[16];
    CHECK(strcmp(pwr_format(0, buf, sizeof(buf)), "00m") == 0, "%s", buf);
    CHECK(strcmp(pwr_format(9, buf, sizeof(buf)), "09m") == 0, "%s", buf);
    CHECK(strcmp(pwr_format(60, buf, sizeof(buf)), "1h 00m") == 0, "%s", buf);
    CHECK(strcmp(pwr_format(260, buf, sizeof(buf)), "4h 20m") == 0, "%s", buf);
    CHECK(strcmp(pwr_format(60 * 100 + 5, buf, sizeof(buf)), "100h 05m") == 0, "%s", buf);
    /* A short buffer truncates safely rather than overrunning. */
    char tiny[4];
    pwr_format(260, tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny), "truncated within the buffer: '%s'", tiny);
    CHECK(pwr_format(1, NULL, 0) == NULL, "null buffer handled");

    banner("power: a gauge the pack voltage contradicts is not evidence");
    {
        /* The curve itself: monotonic, clamped, and linear between. */
        CHECK(pwr_soc_from_mv(3300) == 0, "3.30 V is the empty end");
        CHECK(pwr_soc_from_mv(4200) == 100, "4.20 V is the full end");
        CHECK(pwr_soc_from_mv(3000) == 0, "below empty must clamp, not wrap");
        CHECK(pwr_soc_from_mv(4500) == 100, "above full must clamp");
        CHECK(pwr_soc_from_mv(3750) == 50, "the midpoint is 50%%, got %u",
              (unsigned)pwr_soc_from_mv(3750));
        for (uint16_t mv = 3300; mv < 4200; mv += 25) {
            CHECK(pwr_soc_from_mv(mv) <= pwr_soc_from_mv((uint16_t)(mv + 25)),
                  "the curve went backwards at %u mV", (unsigned)mv);
        }

        /* THE READING THAT STARTED THIS, measured on the device: the gauge
         * said 9% while the pack sat at 3.62 V, which the curve puts near 35.
         * A 26-point gap is the coulomb counter having restarted at zero after
         * a reset, not a battery about to die - and the row drew it in alarm
         * red. */
        CHECK(pwr_soc_is_approx(9, 3620),
              "9%% at 3.62 V must be flagged; the curve says %u",
              (unsigned)pwr_soc_from_mv(3620));
        /* The 0% that a charging pack reported straight after a reflash,
         * twenty minutes after the same pack read 43%. */
        CHECK(pwr_soc_is_approx(0, 3620), "0%% at 3.62 V must be flagged");

        /* A gauge that agrees with the pack is left alone. The bound is wide
         * on purpose: charging lifts the curve and load drops it, and neither
         * of those is a broken gauge. */
        CHECK(!pwr_soc_is_approx(35, 3620), "an agreeing gauge must not be flagged");
        CHECK(!pwr_soc_is_approx(50, 3750), "an exact agreement must not be flagged");
        CHECK(!pwr_soc_is_approx(30, 3750), "a 20-point gap is still within tolerance");
        CHECK(pwr_soc_is_approx(70, 3400), "70%% on a nearly flat pack must be flagged");

        /* No voltage is no second opinion. Doubt has to be earned. */
        CHECK(!pwr_soc_is_approx(0, 0), "without a voltage there is nothing to contradict");
        CHECK(!pwr_soc_is_approx(100, 0), "without a voltage there is nothing to contradict");
    }

    banner("event: a BLE length may never exceed the buffer that holds it");
    {
        /* The radio clamps on the way in, so a well-formed event is untouched. */
        pharos_ev_ble_t b;
        memset(&b, 0, sizeof b);
        b.data_len = 0;
        CHECK(pharos_ev_ble_len(&b) == 0, "an empty advertisement stays empty");
        b.data_len = 17;
        CHECK(pharos_ev_ble_len(&b) == 17, "an ordinary length is passed through");
        b.data_len = PHAROS_BLE_ADV_MAX;
        CHECK(pharos_ev_ble_len(&b) == PHAROS_BLE_ADV_MAX,
              "a full advertisement is passed through");

        /* THE ONE THAT MATTERS. A torn event across the payload union can put
         * a byte of an attacker's 802.11 frame in this field; every walker
         * then indexes a 31-byte array with it. Three lenses shared one
         * single-producer ring between the Wi-Fi task and the NimBLE task,
         * which is how such an event could be built. The ring is fixed - this
         * is the belt to that pair of braces. */
        b.data_len = (uint8_t)(PHAROS_BLE_ADV_MAX + 1);
        CHECK(pharos_ev_ble_len(&b) == PHAROS_BLE_ADV_MAX,
              "one past the buffer must clamp");
        b.data_len = 255;
        CHECK(pharos_ev_ble_len(&b) == PHAROS_BLE_ADV_MAX,
              "the widest possible byte must clamp, got %u",
              (unsigned)pharos_ev_ble_len(&b));
        for (unsigned v = 0; v <= 255u; v++) {
            b.data_len = (uint8_t)v;
            CHECK(pharos_ev_ble_len(&b) <= PHAROS_BLE_ADV_MAX,
                  "length %u escaped the buffer", v);
        }
    }
}
