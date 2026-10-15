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

static void test_vigil_google_find_my(void)
{
    banner("vigil: Google's Find My Device network, and the beacon it hides among");

    /* THE HALF OF THE MARKET THIS LENS COULD NOT SEE.
     *
     * Google's network launched in 2024 and now carries Pebblebee, Moto Tag,
     * Eufy and Chipolo Point. A tag on it follows somebody exactly as well as
     * an AirTag does, and Vigil was blind to every one of them.
     *
     * The catch is that FMDN advertises under service UUID 0xFEAA - the SAME
     * one as ordinary Eddystone proximity beacons in shops and museums. The
     * frame type is what separates them. */
    {
        /* Eddystone-UID: frame type 0x00. A beacon on a shelf. */
        const uint8_t beacon[] = { 0x03, 0x03, 0xAA, 0xFE,
                                   0x06, 0x16, 0xAA, 0xFE, 0x00, 0xE7, 0x01 };
        CHECK(pv_classify(beacon, sizeof beacon) == PV_KIND_EDDYSTONE,
              "an Eddystone UID frame is a beacon, not a tracker");
    }
    {
        const uint8_t url[] = { 0x03, 0x03, 0xAA, 0xFE,
                                0x06, 0x16, 0xAA, 0xFE, 0x10, 0x00, 0x02 };
        CHECK(pv_classify(url, sizeof url) == PV_KIND_EDDYSTONE,
              "so is a URL frame");
    }
    {
        /* FMDN: a frame type above the documented Eddystone four. */
        const uint8_t fmdn[] = { 0x03, 0x03, 0xAA, 0xFE,
                                 0x07, 0x16, 0xAA, 0xFE, 0x40, 0x11, 0x22, 0x33 };
        CHECK(pv_classify(fmdn, sizeof fmdn) == PV_KIND_FINDMY_GOOGLE,
              "a Find My Device frame is a tracker");
    }

    /* AND THE FALSE POSITIVE THIS ORDERING EXISTS TO PREVENT.
     *
     * A named beacon must never reach the tag table, because everything in
     * that table is a candidate for "travelling with you". Accusing a
     * supermarket shelf of following somebody is the worst sentence this lens
     * has available to it. */
    pv_state_t st;
    pv_reset(&st);
    const uint8_t addr[6] = { 0xC0, 0xFF, 0xEE, 0x00, 0x00, 0x01 };
    const uint8_t beacon[] = { 0x03, 0x03, 0xAA, 0xFE,
                               0x06, 0x16, 0xAA, 0xFE, 0x00, 0xE7, 0x01 };
    for (unsigned i = 0; i < 20u; i++) {
        pv_observe_adv(&st, addr, 0, -50, beacon, sizeof beacon,
                       1000000ull * i);
    }
    CHECK(st.n == 0, "a beacon never enters the tracker table (%u)", st.n);

    /* A real tag on the same UUID does. */
    const uint8_t fmdn[] = { 0x03, 0x03, 0xAA, 0xFE,
                             0x07, 0x16, 0xAA, 0xFE, 0x40, 0x11, 0x22, 0x33 };
    pv_observe_adv(&st, addr, 0, -50, fmdn, sizeof fmdn, 30000000ull);
    CHECK(st.n == 1, "but a Find My Device tag does");
}

static void test_vigil_named_tags(void)
{
    banner("vigil: a tag that puts its maker in the advertisement");

    const uint8_t pb[] = { 0x0A, 0x09, 'P','e','b','b','l','e','b','e','e' };
    CHECK(pv_classify(pb, sizeof pb) == PV_KIND_PEBBLEBEE,
          "Pebblebee names itself");

    /* And the negative: a name that merely starts similarly is not a tag. */
    const uint8_t other[] = { 0x07, 0x09, 'P','e','b','b','l','e' };
    CHECK(pv_classify(other, sizeof other) != PV_KIND_PEBBLEBEE,
          "a shorter name is not assumed to be one");
}

/* A Tile, built the same way every other test here builds one. */
static void feed(pv_state_t *s, const uint8_t addr[6], uint64_t t)
{
    const uint8_t n = mk_service(0xFEED);
    pv_observe_adv(s, addr, 1, -60, g_buf, n, t);
}

static void test_vigil_table_keeps_the_evidence(void)
{
    banner("vigil: a full table must not lock out the tag that follows you");

    /* THE FIRST-COME LOTTERY.
     *
     * The table used to set `full` and return, so the first thirty-two
     * addresses seen kept their slots for the whole session. In a cafe with a
     * dozen trackers in other people's bags the table fills with strangers
     * within a minute - and a tag that starts following you AFTER that could
     * never be recorded at all. The device this lens exists to catch is
     * precisely the one that appears and persists. */
    pv_state_t s;
    pv_reset(&s);
    pv_observe_locale(&s, 0x11111111u, 1000000ull);

    for (unsigned i = 0; i < PV_MAX_TAGS; i++) {
        uint8_t a[6] = { 0x40, 0x11, 0x22, 0x33, 0x00, (uint8_t)i };
        feed(&s, a, 1000000ull + i);
    }

    const uint8_t hunter[6] = { 0x50, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
    feed(&s, hunter, 5000000ull);

    bool present = false;
    for (unsigned i = 0; i < PV_MAX_TAGS; i++) {
        if (s.tags[i].in_use && memcmp(s.tags[i].addr, hunter, 6) == 0) {
            present = true;
        }
    }
    CHECK(present, "a later arrival can still get a slot");
    CHECK(s.full, "and the table still reports that it overflowed");
}

static void test_vigil_evidence_outranks_arrival(void)
{
    banner("vigil: a tag seen across two places is not evicted for a stranger");

    /* Eviction must be ranked by EVIDENCE, not by age. A tracker seen across
     * two locales is the finding; a stranger seen once is noise.
     *
     * And a tag that has gone QUIET must survive. BLE-Doubt measured paired
     * AirTags going silent for up to an hour and resuming only for minutes at
     * a time, and that behaviour persisted after separation from the owner -
     * so evicting on silence alone would discard the exact device this lens
     * exists to catch. Silence is the tie-break, never the first test. */
    pv_state_t s;
    pv_reset(&s);

    const uint8_t follower[6] = { 0x60, 0x01, 0x02, 0x03, 0x04, 0x05 };
    pv_observe_locale(&s, 0x55555555u, 1000000ull);
    feed(&s, follower, 1500000ull);
    pv_observe_locale(&s, 0xAAAAAAAAu, 3000000ull);
    feed(&s, follower, 3500000ull);

    /* Then a crowd of strangers, every one of them seen more recently. */
    for (unsigned i = 0; i < PV_MAX_TAGS + 8u; i++) {
        uint8_t a[6] = { 0x70, 0x11, 0x22, 0x33, 0x00, (uint8_t)i };
        feed(&s, a, 9000000ull + i);
    }

    bool kept = false;
    for (unsigned i = 0; i < PV_MAX_TAGS; i++) {
        if (s.tags[i].in_use && memcmp(s.tags[i].addr, follower, 6) == 0) {
            kept = true;
        }
    }
    CHECK(kept, "the tag with locale evidence survives a crowd of strangers");
}

/* A BENIGN STRUCTURE IN FRONT OF A TRACKER MUST NOT HIDE IT.
 *
 * The file already paid for this once with an UNRECOGNISED prefix - "the
 * cheapest possible way to hide a tracker from it was to put a scrap of
 * Apple-shaped data in front". The same attack came back through a
 * RECOGNISED one: the classifier returned on the first structure it knew, so
 * five bytes of Eddystone in front of a real tag classified the frame as a
 * proximity beacon, and pv_observe_adv() rightly refuses to table a beacon.
 * The tracker was then not scored, not listed, invisible.
 *
 * Every fixture in the existing decoy block uses the unrecognised prefix, so
 * nothing asserted order-independence against a recognised one. */
static void test_vigil_a_recognised_decoy_cannot_hide_a_tracker(void)
{
    banner("vigil: a recognised benign element must not hide a tracker");

    /* Benign, and recognised: an Eddystone UID frame. */
    static const uint8_t eddystone[] = { 0x04, 0x16, 0xAA, 0xFE, 0x00 };

    struct { const char *name; const uint8_t *adv; uint8_t len;
             pv_kind_t kind; } tags[] = {
        { "SmartTag",  (const uint8_t[]){ 0x03, 0x16, 0x5A, 0xFD }, 4,
          PV_KIND_SMARTTAG },
        { "Tile",      (const uint8_t[]){ 0x03, 0x16, 0xED, 0xFE }, 4,
          PV_KIND_TILE },
        { "Chipolo",   (const uint8_t[]){ 0x03, 0x16, 0x33, 0xFE }, 4,
          PV_KIND_CHIPOLO },
        { "Find My",   (const uint8_t[]){ 0x03, 0x16, 0x44, 0xFD }, 4,
          PV_KIND_FINDMY },
    };

    for (unsigned t = 0; t < sizeof(tags) / sizeof(tags[0]); t++) {
        uint8_t first[32], second[32];
        uint8_t n1 = 0, n2 = 0;

        /* decoy first, then the tracker */
        memcpy(first, eddystone, sizeof(eddystone)); n1 = sizeof(eddystone);
        memcpy(first + n1, tags[t].adv, tags[t].len); n1 += tags[t].len;
        /* the tracker first, then the decoy */
        memcpy(second, tags[t].adv, tags[t].len); n2 = tags[t].len;
        memcpy(second + n2, eddystone, sizeof(eddystone));
        n2 += sizeof(eddystone);

        const pv_kind_t ka = pv_classify(first, n1);
        const pv_kind_t kb = pv_classify(second, n2);

        CHECK(ka == kb, "%s: order changed the verdict (%s then %s)",
              tags[t].name, pv_kind_name(ka), pv_kind_name(kb));
        CHECK(ka == tags[t].kind,
              "%s behind a beacon classified as %s", tags[t].name,
              pv_kind_name(ka));

        /* AND THE PROPERTY THAT ACTUALLY PROTECTS SOMEBODY: the tag reaches
         * the table either way round, because a tag that is never tabled is
         * never counted, never listed and never reported. */
        for (unsigned order = 0; order < 2u; order++) {
            const uint8_t *adv = order ? second : first;
            const uint8_t n = order ? n2 : n1;
            pv_state_t st;
            pv_reset(&st);
            pv_set_moved(&st, true);
            static const uint8_t addr[6] = { 0x44, 0, 0, 0, 0, 0x01 };
            pv_observe_locale(&st, 0x0F0F0F0Fu, 1000000ull);
            pv_observe_adv(&st, addr, 1, -50, adv, n, 1000000ull);
            pv_verdict_t v;
            pv_evaluate(&st, 2000000ull, &v);
            CHECK(v.n_tags == 1,
                  "%s was not tabled with the beacon %s it (%u tags)",
                  tags[t].name, order ? "after" : "before", v.n_tags);
        }
    }

    /* AND THE FALSE POSITIVE THE EARLY RETURN EXISTED FOR IS STILL REFUSED.
     * A frame carrying only beacon structures is still a beacon. */
    CHECK(pv_classify(eddystone, sizeof(eddystone)) == PV_KIND_EDDYSTONE,
          "a beacon-only advertisement is still a beacon");
    {
        pv_state_t st;
        pv_reset(&st);
        pv_set_moved(&st, true);
        static const uint8_t addr[6] = { 0x55, 0, 0, 0, 0, 0x01 };
        pv_observe_locale(&st, 0x0F0F0F0Fu, 1000000ull);
        pv_observe_adv(&st, addr, 1, -50, eddystone, sizeof(eddystone),
                       1000000ull);
        pv_verdict_t v;
        pv_evaluate(&st, 2000000ull, &v);
        CHECK(v.n_tags == 0, "a shop beacon must still not be tabled");
    }
}

/* THE SEPARATED VERDICT MAY ONLY COME FROM THE STATUS BYTE.
 *
 * In a Find My element the status is at p[4]: company (2), type 0x12 (1),
 * payload length (1), status. An element too short to hold it was read at
 * p[plen - 1] instead - the LENGTH byte - so six bytes anyone can transmit
 * classified as SEPARATED. That is remembered permanently and adds to the
 * stalking score. */
static void test_vigil_short_findmy_is_not_separated(void)
{
    banner("vigil: a Find My element too short for a status is not separated");

    /* plen == 4: company, 0x12, and a length byte whose value looks like a
     * set separated bit. There is no status field in this element. */
    const uint8_t no_status[] = { 0x05, 0xFF, 0x4C, 0x00, 0x12, 0x04 };
    CHECK(pv_classify(no_status, sizeof(no_status)) != PV_KIND_FINDMY_LOST,
          "a status byte that does not exist was read as separated");
    CHECK(pv_classify(no_status, sizeof(no_status)) == PV_KIND_FINDMY,
          "and the honest reading is Find My, got %s",
          pv_kind_name(pv_classify(no_status, sizeof(no_status))));

    /* With a real status byte it must still work, both ways. */
    const uint8_t sep[]  = { 0x06, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x04 };
    const uint8_t near[] = { 0x06, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x00 };
    CHECK(pv_classify(sep, sizeof(sep)) == PV_KIND_FINDMY_LOST,
          "a genuine separated status is still read, got %s",
          pv_kind_name(pv_classify(sep, sizeof(sep))));
    CHECK(pv_classify(near, sizeof(near)) == PV_KIND_FINDMY,
          "and an owner-nearby status is still read, got %s",
          pv_kind_name(pv_classify(near, sizeof(near))));
}

/* A TAG THAT ARRIVES IN A CROWDED ROOM MUST STILL COUNT ITS PLACES.
 *
 * When the tag table is full, a new arrival may evict the least evidential
 * entry. That branch duplicated the tail's work and returned - and the one
 * thing it did not duplicate was recording the locale. So a tag admitted by
 * eviction started with no locale bit at all.
 *
 * n_locales is the evidence that separates being FOLLOWED from being near,
 * so a tracker that first appeared in a busy room needed one more place than
 * one that appeared in an empty room before it could be reported. The room
 * being crowded is not a fact about the tracker. */
static void test_vigil_an_evicted_entry_still_counts_its_locale(void)
{
    banner("vigil: a tag admitted by eviction records the place it arrived");

    pv_state_t s;
    pv_reset(&s);
    pv_set_moved(&s, true);
    pv_observe_locale(&s, 0x0F0F0F0Fu, 1000ull);

    /* Fill the table with single-sighting tags, which are the least
     * evidential and therefore the ones eviction takes. */
    const uint8_t len = mk_findmy(false);
    for (unsigned i = 0; i < PV_MAX_TAGS; i++) {
        see(&s, (uint8_t)(0x40 + i), len, 2000ull + i);
    }

    /* One more arrival, in this same place. It must be admitted AND must
     * record that it was heard here. */
    see(&s, 0xC1, len, 9000ull);

    bool found = false;
    for (unsigned i = 0; i < PV_MAX_TAGS; i++) {
        if (!s.tags[i].in_use) { continue; }
        if (s.tags[i].addr[5] != 0xC1) { continue; }
        found = true;
        CHECK(s.tags[i].n_locales >= 1u,
              "a tag admitted by eviction recorded %u locales",
              s.tags[i].n_locales);
    }
    CHECK(found, "the evicting arrival was admitted");

    /* AND IT ACCRUES FROM THERE like any other tag: a second place is a
     * second place, which is what the following verdict is built on. */
    pv_observe_locale(&s, 0xF0F0F0F0u, 20000000ull);
    see(&s, 0xC1, len, 21000000ull);
    for (unsigned i = 0; i < PV_MAX_TAGS; i++) {
        if (s.tags[i].in_use && s.tags[i].addr[5] == 0xC1) {
            CHECK(s.tags[i].n_locales >= 2u,
                  "it should have two places now, has %u",
                  s.tags[i].n_locales);
        }
    }
}


void test_vigil(void)
{
    test_vigil_an_evicted_entry_still_counts_its_locale();
    test_vigil_a_recognised_decoy_cannot_hide_a_tracker();
    test_vigil_short_findmy_is_not_separated();
    test_vigil_table_keeps_the_evidence();
    test_vigil_evidence_outranks_arrival();
    test_vigil_google_find_my();
    test_vigil_named_tags();
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

    banner("vigil: a decoy element must not hide what follows it");
    {
        /* AN ADVERTISEMENT IS SEVERAL AD STRUCTURES, AND THE WALK MUST SEE
         * ALL OF THEM. pv_observe_adv() drops anything classified UNKNOWN, so
         * a classifier that gives up part way through does not merely miss a
         * detail - it makes the device invisible to an anti-stalking lens. */

        /* A four-byte Apple manufacturer element: a company ID and one byte,
         * carrying no Find My type. On its own it is nothing. */
        const uint8_t decoy[] = { 0x03, 0xFF, 0x4C, 0x00 };
        CHECK(pv_classify(decoy, sizeof decoy) == PV_KIND_UNKNOWN,
              "an Apple scrap alone should name nothing");

        /* The same scrap, followed by a Flipper announcing itself by name.
         * The Flipper must still be found. */
        const uint8_t hidden_flipper[] = {
            0x03, 0xFF, 0x4C, 0x00,                       /* the decoy      */
            0x0A, 0x09, 'F', 'l', 'i', 'p', 'p', 'e', 'r', ' ', 'X'
        };
        CHECK(pv_classify(hidden_flipper, sizeof hidden_flipper) == PV_KIND_FLIPPER,
              "a decoy in front of a Flipper hid it; got %d",
              (int)pv_classify(hidden_flipper, sizeof hidden_flipper));

        /* And in front of a SmartTag's service UUID. */
        const uint8_t hidden_tag[] = {
            0x03, 0xFF, 0x4C, 0x00,                       /* the decoy      */
            0x03, 0x16, 0x5A, 0xFD                        /* 0xFD5A         */
        };
        CHECK(pv_classify(hidden_tag, sizeof hidden_tag) == PV_KIND_SMARTTAG,
              "a decoy in front of a SmartTag hid it; got %d",
              (int)pv_classify(hidden_tag, sizeof hidden_tag));

        /* A real Find My element still wins when it is actually there. */
        const uint8_t findmy[] = { 0x06, 0xFF, 0x4C, 0x00, 0x12, 0x19, 0x04 };
        const pv_kind_t k = pv_classify(findmy, sizeof findmy);
        CHECK(k == PV_KIND_FINDMY || k == PV_KIND_FINDMY_LOST,
              "a Find My element should still be named, got %d", (int)k);
    }

    banner("vigil: a company ID is not a tracker");
    {
        /* 0x0075 IS SAMSUNG, NOT A GALAXY SMARTTAG. Every Galaxy phone, every
         * pair of Buds and every Watch advertises under it, and this lens
         * accuses whatever it names of possibly following somebody. */
        const uint8_t galaxy[] = { 0x05, 0xFF, 0x75, 0x00, 0x42, 0x01 };
        CHECK(pv_classify(galaxy, sizeof galaxy) != PV_KIND_SMARTTAG,
              "a Samsung company ID alone must not be called a SmartTag");

        /* The honest signal: the SmartThings Find service UUID. */
        const uint8_t smarttag[] = { 0x03, 0x16, 0x5A, 0xFD };
        CHECK(pv_classify(smarttag, sizeof smarttag) == PV_KIND_SMARTTAG,
              "0xFD5A must still name a SmartTag");
    }
}
