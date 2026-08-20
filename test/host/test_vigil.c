/* Pharos host tests, part fourteen: Vigil - is a tracker travelling with you?
 *
 * The two tests that matter most are refusals. A café full of other people's
 * AirTags must not alarm, and nothing may reach the top band until the tracker
 * has survived an actual change of place. For this subject a false positive
 * frightens somebody and a false negative reassures them wrongly, so both
 * directions are asserted explicitly.
 */
#include "pharos_vigil.h"
#include "test_support.h"

#define MIN (60ull * 1000000ull)

/* ---- payload builders ------------------------------------------------- */

/* Apple Find My. `separated` sets the status bit that means the owner is not
 * nearby - the state a planted tracker is always in. */
static uint8_t g_buf[32];

static uint8_t mk_findmy(bool separated)
{
    uint8_t i = 0;
    g_buf[i++] = 0x07;       /* length of this AD structure */
    g_buf[i++] = 0xFF;       /* manufacturer specific       */
    g_buf[i++] = 0x4C;       /* Apple, little-endian        */
    g_buf[i++] = 0x00;
    g_buf[i++] = 0x12;       /* Find My                     */
    g_buf[i++] = 0x19;       /* payload length              */
    g_buf[i++] = separated ? 0x04 : 0x00; /* status         */
    g_buf[i++] = 0x00;
    return i;
}

static uint8_t mk_service(uint16_t uuid)
{
    uint8_t i = 0;
    g_buf[i++] = 0x03;
    g_buf[i++] = 0x16;                    /* service data, 16-bit UUID */
    g_buf[i++] = (uint8_t)(uuid & 0xFF);
    g_buf[i++] = (uint8_t)(uuid >> 8);
    return i;
}

static uint8_t mk_phone(void)
{
    /* An ordinary Apple device that is not a tracker. */
    uint8_t i = 0;
    g_buf[i++] = 0x05;
    g_buf[i++] = 0xFF;
    g_buf[i++] = 0x4C;
    g_buf[i++] = 0x00;
    g_buf[i++] = 0x10;  /* nearby-info, not Find My */
    g_buf[i++] = 0x05;
    return i;
}

static void addr_of(uint8_t out[6], uint8_t last)
{
    const uint8_t a[6] = { 0xD0, 0x11, 0x22, 0x33, 0x44, 0x00 };
    memcpy(out, a, 6);
    out[5] = last;
}

/* Feed one tag n times in the current locale. */
static void see(pv_state_t *s, uint8_t last, uint8_t len, uint64_t t)
{
    uint8_t addr[6];
    addr_of(addr, last);
    pv_observe_adv(s, addr, 1, -60, g_buf, len, t);
}


/* The device list, and the ordering that makes it useful.
 *
 * A verdict saying "something is following you" that cannot name WHICH device
 * is a fright with no remedy, so the tag table is exposed - and it has to come
 * out worst-first, because the operator reads the top of the list and acts on
 * it. Presence in several PLACES ranks above everything else: that is the
 * whole difference between a tracker travelling with you and one that happened
 * to be in the cafe. */
static void test_vigil_tag_list(void)
{
    banner("vigil: the tag list names devices, worst first");
    pv_state_t s;
    pv_reset(&s);

    const uint64_t T = 1000000ull;
    /* A tag seen in one place only. */
    static const uint8_t near_addr[6] = { 0x11, 0, 0, 0, 0, 0x01 };
    /* A tag that survives a move - the one that matters. */
    static const uint8_t follow_addr[6] = { 0x22, 0, 0, 0, 0, 0x02 };

    /* Real advertisements: the engine only admits trackers it can NAME, and
     * refusing to track every passing phone is the point. A Tile announces
     * itself with service data for UUID 0xFEED. */
    const uint8_t tile_adv[] = { 0x03, 0x16, 0xED, 0xFE };

    pv_observe_locale(&s, 0x55555555u, T);
    for (int i = 0; i < 6; i++) {
        pv_observe_adv(&s, near_addr, 1, -50, tile_adv, sizeof(tile_adv),
                       T + (uint64_t)i * 1000000ull);
        pv_observe_adv(&s, follow_addr, 1, -60, tile_adv, sizeof(tile_adv),
                       T + (uint64_t)i * 1000000ull);
    }
    /* Move: a completely different Wi-Fi landscape. */
        /* Every bit different: PV_LOCALE_CHANGE_PERMIL wants the Wi-Fi landscape
     * to have genuinely turned over before it calls this a new PLACE, which is
     * what stops a device being called a follower for sitting on one desk. */
    pv_observe_locale(&s, 0xAAAAAAAAu, T + 60000000ull);
    for (int i = 0; i < 6; i++) {
        pv_observe_adv(&s, follow_addr, 1, -60, tile_adv, sizeof(tile_adv),
                       T + 61000000ull + (uint64_t)i * 1000000ull);
    }

    pv_tag_t t;
    uint32_t mins = 0;
    CHECK(pv_tag_at(&s, 0, T + 70000000ull, &t, &mins), "there is a first tag");
    CHECK(t.n_locales >= 2, "the follower ranks first (locales=%u)", t.n_locales);
    CHECK(memcmp(t.addr, follow_addr, 6) == 0, "and it is the right device");

    CHECK(pv_tag_at(&s, 1, T + 70000000ull, &t, &mins), "there is a second tag");
    CHECK(memcmp(t.addr, near_addr, 6) == 0, "the one-place tag ranks below it");

    /* Every index must be distinct and the list must terminate. */
    unsigned n = 0;
    while (pv_tag_at(&s, n, T + 70000000ull, &t, &mins) && n < PV_MAX_TAGS + 2u) {
        n++;
    }
    CHECK_EQ(n, 2);
}

/* The names have to survive being shown to somebody. */
static void test_vigil_classify_extended(void)
{
    banner("vigil: Flipper and serial bridges announce themselves by name");

    /* AD structure: [len][type][payload] - complete local name is 0x09. */
    uint8_t flip[16];
    const char *fn = "Flipper Xyz";
    flip[0] = (uint8_t)(1 + strlen(fn));
    flip[1] = 0x09;
    memcpy(&flip[2], fn, strlen(fn));
    CHECK_EQ(pv_classify(flip, (uint8_t)(2 + strlen(fn))), PV_KIND_FLIPPER);

    uint8_t hc[16];
    const char *hn = "HC-05";
    hc[0] = (uint8_t)(1 + strlen(hn));
    hc[1] = 0x09;
    memcpy(&hc[2], hn, strlen(hn));
    CHECK_EQ(pv_classify(hc, (uint8_t)(2 + strlen(hn))), PV_KIND_SERIAL);

    /* A normal device name must NOT be classified as either. Calling a
     * Bluetooth speaker a card skimmer would be the most damaging false
     * positive this project could produce. */
    uint8_t spk[24];
    const char *sn = "JBL Flip 5";
    spk[0] = (uint8_t)(1 + strlen(sn));
    spk[1] = 0x09;
    memcpy(&spk[2], sn, strlen(sn));
    const pv_kind_t k = pv_classify(spk, (uint8_t)(2 + strlen(sn)));
    CHECK(k != PV_KIND_FLIPPER && k != PV_KIND_SERIAL,
          "\"JBL Flip 5\" is not a Flipper and not a bridge (got %s)",
          pv_kind_name(k));

    /* Every kind must name itself for the screen. */
    for (int i = 0; i < PV_KIND_COUNT; i++) {
        const char *nm = pv_kind_name((pv_kind_t)i);
        CHECK(nm && *nm, "kind %d names itself", i);
    }
}

/* THE MOVEMENT GATE.
 *
 * A locale change is evidence of movement, not proof of it. Access points
 * switch off, routers reboot, and turning round in a large building drops half
 * the estate out of earshot - the signature turns over while somebody sat
 * still, and a tracker that was merely nearby starts looking like a stalker.
 *
 * On this subject that is the worst available way to be wrong, so the
 * accelerometer - which cannot be fooled by a rebooting router - can withhold
 * the accusation. */
static void feed_three_locales(pv_state_t *s, uint64_t base)
{
    static const uint8_t follow[6] = { 0x22, 0, 0, 0, 0, 0x02 };
    const uint8_t tile_adv[] = { 0x03, 0x16, 0xED, 0xFE };
    static const uint32_t sig[3] = { 0x55555555u, 0xAAAAAAAAu, 0x0F0F0F0Fu };

    for (unsigned loc = 0; loc < 3u; loc++) {
        const uint64_t at = base + (uint64_t)loc * 60000000ull;
        pv_observe_locale(s, sig[loc], at);
        for (int i = 0; i < 6; i++) {
            pv_observe_adv(s, follow, 1, -60, tile_adv, sizeof(tile_adv),
                           at + (uint64_t)i * 1000000ull);
        }
    }
}

static void test_vigil_movement_gates_following(void)
{
    banner("vigil: FOLLOWING needs evidence the person actually moved");

    const uint64_t base = 1000000000ull;
    const uint64_t end = base + 200000000ull;

    /* Identical sighting history in every run; only the accelerometer differs. */
    pv_state_t moved, still, no_imu;
    pv_reset(&moved);
    pv_reset(&still);
    pv_reset(&no_imu);
    pv_set_moved(&moved, true);
    pv_set_moved(&still, false);
    /* no_imu deliberately never calls pv_set_moved. */

    feed_three_locales(&moved, base);
    feed_three_locales(&still, base);
    feed_three_locales(&no_imu, base);

    pv_verdict_t vm, vs, vn;
    pv_evaluate(&moved, end, &vm);
    pv_evaluate(&still, end, &vs);
    pv_evaluate(&no_imu, end, &vn);

    CHECK(vm.ceiling > 69, "having moved, the ceiling allows FOLLOWING (%u)",
          vm.ceiling);
    CHECK(vs.ceiling <= 69,
          "having not moved, it does not (%u)", vs.ceiling);
    CHECK(vs.band < PV_BAND_FOLLOWING,
          "so the accusation is withheld (band %s)", pv_band_name(vs.band));

    /* It is a CEILING, not a deletion: the sightings still happened and are
     * still reported. What changes is what may be concluded from them. */
    CHECK_EQ(vs.n_tags, vm.n_tags);
    CHECK_EQ(vs.n_locales, vm.n_locales);

    /* AND THE RULE THAT KEEPS BOARDS WITHOUT AN IMU WORKING. */
    CHECK_EQ(vn.ceiling, vm.ceiling);
    CHECK(vn.band == vm.band, "no motion sensor is not a veto");

    /* Movement reported later lifts the gate rather than latching it. */
    pv_set_moved(&still, true);
    pv_evaluate(&still, end, &vs);
    CHECK_EQ(vs.ceiling, vm.ceiling);
    CHECK(vs.band == vm.band, "and it lifts once they do move");
}

void test_vigil(void)
{
    test_vigil_movement_gates_following();
    test_vigil_tag_list();
    test_vigil_classify_extended();
    banner("vigil: classification");

    CHECK_EQ(pv_classify(g_buf, mk_findmy(false)), PV_KIND_FINDMY);
    CHECK_EQ(pv_classify(g_buf, mk_findmy(true)), PV_KIND_FINDMY_LOST);
    CHECK_EQ(pv_classify(g_buf, mk_service(0xFEED)), PV_KIND_TILE);
    CHECK_EQ(pv_classify(g_buf, mk_service(0xFD5A)), PV_KIND_SMARTTAG);
    CHECK_EQ(pv_classify(g_buf, mk_phone()), PV_KIND_UNKNOWN);
    CHECK_EQ(pv_classify(NULL, 8), PV_KIND_UNKNOWN);
    CHECK_EQ(pv_classify(g_buf, 0), PV_KIND_UNKNOWN);

    /* Truncation at every length must never read past the buffer. The payload
     * is chosen by whoever is advertising, so this is the safety property. */
    const uint8_t full = mk_findmy(true);
    for (uint8_t cut = 0; cut <= full; cut++) {
        uint8_t tmp[32];
        memcpy(tmp, g_buf, cut);
        (void)pv_classify(tmp, cut);
    }
    CHECK(true, "every truncation length survived");

    /* A malformed AD chain (length field lies) must stop, not loop or run on. */
    {
        uint8_t bad[8] = { 0xFF, 0xFF, 0x4C, 0x00, 0x12, 0x19, 0x04, 0x00 };
        (void)pv_classify(bad, sizeof(bad));
        uint8_t zero[6] = { 0x00, 0xFF, 0x4C, 0x00, 0x12, 0x19 };
        (void)pv_classify(zero, sizeof(zero));
        CHECK(true, "malformed AD chains are survivable");
    }

    banner("vigil: following requires actually moving");
    pv_state_t s;
    pv_verdict_t v;

    /* Nothing at all. */
    pv_reset(&s);
    pv_evaluate(&s, 10 * MIN, &v);
    CHECK_EQ(v.band, PV_BAND_CLEAR);
    CHECK_EQ(v.n_tags, 0);
    CHECK(v.notes & PV_NOTE_ROTATION, "rotation limit always disclosed");
    CHECK(strstr(pv_band_advice(v.band), "not a clean bill of health") != NULL,
          "CLEAR never reassures");

    /* THE false positive to avoid: a café full of other people's trackers, and
     * you have not moved. Must never exceed SEEN. */
    pv_reset(&s);
    pv_observe_locale(&s, 0x0F0F0F0F, 0);
    for (uint8_t k = 0; k < 8; k++) {
        const uint8_t n = mk_findmy(false);
        for (int r = 0; r < 20; r++) {
            see(&s, (uint8_t)(0x10 + k), n, (uint64_t)r * MIN);
        }
    }
    pv_evaluate(&s, 20 * MIN, &v);
    CHECK_EQ(v.n_tags, 8);
    CHECK_EQ(v.n_following, 0);
    CHECK(v.notes & PV_NOTE_ONE_PLACE, "standing still is disclosed");
    CHECK(v.band <= PV_BAND_SEEN, "a room full of tags is not an alarm (got %s)",
          pv_band_name(v.band));
    CHECK(v.score <= 44, "capped without a second locale (got %u)", v.score);
    CHECK(strstr(pv_band_advice(v.band), "ordinary") != NULL,
          "advice says plainly this is normal");

    /* Move: the access-point landscape turns over. A tag that came with you is
     * now interesting - but ONE move is still a coincidence, so it is capped
     * below the alarm. */
    pv_observe_locale(&s, 0xF0F0F0F0, 25 * MIN);
    {
        const uint8_t n = mk_findmy(true);
        for (int r = 0; r < 10; r++) {
            see(&s, 0x10, n, (uint64_t)(26 + r) * MIN);
        }
    }
    pv_evaluate(&s, 40 * MIN, &v);
    CHECK_EQ(v.n_locales, 2);
    CHECK_EQ(v.n_following, 1);
    CHECK(v.worst_addr[5] == 0x10, "names the tag that followed");
    CHECK(v.band <= PV_BAND_PERSISTENT,
          "one move alone is not FOLLOWING (got %s)", pv_band_name(v.band));
    CHECK(v.score <= 69, "capped at two locales (got %u)", v.score);

    /* A third distinct place. Now it is travelling with you. */
    pv_observe_locale(&s, 0x00FF00FF, 60 * MIN);
    {
        const uint8_t n = mk_findmy(true);
        for (int r = 0; r < 10; r++) {
            see(&s, 0x10, n, (uint64_t)(61 + r) * MIN);
        }
    }
    pv_evaluate(&s, 80 * MIN, &v);
    CHECK_EQ(v.n_locales, 3);
    CHECK(v.worst_locales >= 3, "seen in three places (%u)", v.worst_locales);
    CHECK_EQ(v.band, PV_BAND_FOLLOWING);
    CHECK_EQ(v.worst_kind, PV_KIND_FINDMY_LOST);
    CHECK(v.worst_minutes >= 30, "duration reported (%u min)", v.worst_minutes);
    CHECK(strstr(v.headline, "several places") != NULL, "headline: %s", v.headline);
    CHECK(strstr(pv_band_advice(v.band), "not a statement about anybody's") != NULL,
          "advice refuses to claim intent");
    CHECK(strstr(pv_band_advice(v.band), "police") != NULL,
          "advice points somewhere real");

    /* The tags that did NOT follow are still only 'seen'. */
    CHECK_EQ(v.n_following, 1);

    /* Your own devices are supposed to follow you. Marking one known removes
     * it from the verdict entirely rather than fudging its score. */
    {
        uint8_t mine[6];
        addr_of(mine, 0x10);
        CHECK(pv_mark_known(&s, mine), "marked known");
        pv_evaluate(&s, 80 * MIN, &v);
        CHECK_EQ(v.n_following, 0);
        CHECK(v.band <= PV_BAND_SEEN, "your own tag no longer alarms (got %s)",
              pv_band_name(v.band));
        CHECK(v.notes & PV_NOTE_KNOWN, "known list disclosed");
    }

    /* A small drift in the access-point landscape is the SAME place - walking
     * to the other end of an office must not count as travelling. */
    pv_reset(&s);
    pv_observe_locale(&s, 0xFFFF0000, 0);
    pv_observe_locale(&s, 0xFFFE0000, 1 * MIN);
    pv_observe_locale(&s, 0xFFFC0000, 2 * MIN);
    pv_evaluate(&s, 3 * MIN, &v);
    CHECK_EQ(v.n_locales, 1);

    /* Non-tracker traffic is ignored entirely: phones and laptops advertise
     * constantly and are not what this lens is about. */
    pv_reset(&s);
    pv_observe_locale(&s, 0x12345678, 0);
    {
        const uint8_t n = mk_phone();
        for (int r = 0; r < 50; r++) {
            see(&s, 0x55, n, (uint64_t)r * MIN);
        }
    }
    pv_evaluate(&s, 50 * MIN, &v);
    CHECK_EQ(v.n_tags, 0);
    CHECK_EQ(v.band, PV_BAND_CLEAR);

    /* A short session cannot be confident, however much it saw. */
    {
        pv_state_t q;
        pv_verdict_t qv;
        pv_reset(&q);
        pv_observe_locale(&q, 0x0000FFFF, 0);
        const uint8_t n = mk_findmy(true);
        uint8_t a[6];
        addr_of(a, 0x77);
        pv_observe_adv(&q, a, 1, -50, g_buf, n, 0);
        pv_observe_locale(&q, 0xFFFF0000, 30ull * 1000000ull);
        pv_observe_adv(&q, a, 1, -50, g_buf, n, 30ull * 1000000ull);
        pv_evaluate(&q, 40ull * 1000000ull, &qv);
        CHECK(qv.notes & PV_NOTE_SHORT, "short session disclosed");
        CHECK(qv.ceiling < 84, "and the ceiling reflects it (%u)", qv.ceiling);
    }

    /* Capacity is bounded and disclosed rather than overrun. */
    {
        pv_state_t big;
        pv_verdict_t bv;
        pv_reset(&big);
        pv_observe_locale(&big, 0xABCDEF01, 0);
        const uint8_t n = mk_service(0xFEED);
        for (unsigned i = 0; i < PV_MAX_TAGS + 8u; i++) {
            uint8_t a[6];
            addr_of(a, (uint8_t)i);
            pv_observe_adv(&big, a, 1, -70, g_buf, n, (uint64_t)i);
        }
        pv_evaluate(&big, 10 * MIN, &bv);
        CHECK(bv.notes & PV_NOTE_FULL, "table full is disclosed");
    }

    /* NULLs are survivable. */
    pv_reset(NULL);
    pv_observe_locale(NULL, 1, 1);
    pv_observe_adv(NULL, NULL, 0, 0, NULL, 0, 0);
    pv_evaluate(NULL, 0, &v);
    pv_evaluate(&s, 0, NULL);
    CHECK(!pv_mark_known(NULL, NULL), "NULL mark refused");
    CHECK(true, "NULL arguments do not crash");

    /* Vocabulary. This subject in particular must never promise safety. */
    for (int b = PV_BAND_CLEAR; b < PV_BAND_COUNT; b++) {
        const char *nm = pv_band_name((pv_band_t)b);
        const char *ad = pv_band_advice((pv_band_t)b);
        CHECK(nm && *nm && ad && *ad, "band %d described", b);
        CHECK(strstr(nm, "SAFE") == NULL && strstr(nm, "SECURE") == NULL,
              "no band claims safety");
        CHECK(strstr(ad, "you are safe") == NULL, "advice never says you are safe");
        CHECK(strstr(ad, "no tracker") == NULL, "advice never promises absence");
    }
    for (int k = 0; k < PV_KIND_COUNT; k++) {
        CHECK(pv_kind_name((pv_kind_t)k)[0] != '\0', "kind %d named", k);
    }
}
