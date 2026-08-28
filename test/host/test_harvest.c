/* Pharos host tests, part eleven: Harvest - handshake collection.
 *
 * Two things under test. First the EAPOL parser, against frames built byte by
 * byte, because it reads attacker-chosen input and must never walk off the
 * end. Then the engine, whose whole job is to tell a forced capture apart from
 * the handshakes every network performs all day.
 */
#include "pharos_dot11.h"
#include "pharos_harvest.h"
#include "test_support.h"

/* ---- frame construction ---------------------------------------------- */

static const uint8_t AP[6]  = { 0x02, 0xAA, 0xBB, 0x00, 0x00, 0x01 };
static const uint8_t STA[6] = { 0x02, 0xCC, 0xDD, 0x00, 0x00, 0x09 };

/* Builds an unprotected EAPOL-Key data frame. msg is 1..4; pmkid adds the KDE
 * that makes message 1 the clientless attack. Returns the length. */
static size_t mk_eapol(uint8_t *buf, size_t cap, int msg, bool pmkid, bool qos,
                       bool pairwise)
{
    memset(buf, 0, cap);
    size_t o = 0;
    /* Frame control: data frame, QoS variant if asked. */
    buf[0] = (uint8_t)(qos ? 0x88 : 0x08);
    buf[1] = (msg == 1 || msg == 3) ? 0x02 : 0x01; /* FromDS : ToDS */
    o = 24;
    if (qos) o += 2;

    /* Addresses. M1/M3 come from the AP, M2/M4 from the client. */
    if (msg == 1 || msg == 3) {
        memcpy(buf + 4, STA, 6);  /* a1 = receiver */
        memcpy(buf + 10, AP, 6);  /* a2 = transmitter */
        memcpy(buf + 16, AP, 6);  /* a3 */
    } else {
        memcpy(buf + 4, AP, 6);
        memcpy(buf + 10, STA, 6);
        memcpy(buf + 16, AP, 6);
    }

    /* LLC/SNAP + EAPOL ethertype (network byte order). */
    static const uint8_t snap[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
    memcpy(buf + o, snap, 6); o += 6;
    buf[o++] = 0x88; buf[o++] = 0x8E;

    /* EAPOL header: version, type 3 = EAPOL-Key, length. */
    buf[o++] = 0x02;
    buf[o++] = 0x03;
    buf[o++] = 0x00; buf[o++] = 0x5F;

    buf[o++] = 0x02; /* RSN descriptor */

    /* key_info, big-endian. Bit 3 pairwise, 6 install, 7 ack, 8 mic, 9 secure. */
    uint16_t info = 0;
    if (pairwise) info |= 0x0008;
    switch (msg) {
    case 1: info |= 0x0080; break;                    /* ack            */
    case 2: info |= 0x0100; break;                    /* mic            */
    case 3: info |= 0x0080 | 0x0100 | 0x0040; break;  /* ack+mic+install*/
    case 4: info |= 0x0100 | 0x0200; break;           /* mic+secure     */
    default: break;
    }
    buf[o++] = (uint8_t)(info >> 8);
    buf[o++] = (uint8_t)(info & 0xFF);

    /* key_len(2) replay(8) nonce(32) iv(16) rsc(8) reserved(8) mic(16). */
    o += 2 + 8 + 32 + 16 + 8 + 8 + 16;

    /* key_data_len + key data. */
    if (pmkid) {
        const uint16_t kd = 22;
        buf[o++] = (uint8_t)(kd >> 8);
        buf[o++] = (uint8_t)(kd & 0xFF);
        buf[o++] = 0xDD; buf[o++] = 0x14;
        buf[o++] = 0x00; buf[o++] = 0x0F; buf[o++] = 0xAC; buf[o++] = 0x04;
        o += 16; /* the PMKID itself - never read, never stored */
    } else {
        buf[o++] = 0x00; buf[o++] = 0x00;
    }
    return o;
}

static void test_eapol_parser(void)
{
    banner("harvest: the EAPOL parser");
    uint8_t buf[256];
    pharos_eapol_t e;

    for (int msg = 1; msg <= 4; msg++) {
        const size_t n = mk_eapol(buf, sizeof(buf), msg, false, false, true);
        CHECK(pharos_dot11_eapol(buf, n, &e), "message %d parses", msg);
        CHECK_EQ(e.msg, msg);
        CHECK(e.is_pairwise, "message %d is pairwise", msg);
        CHECK(!e.has_pmkid, "message %d has no PMKID", msg);
    }

    /* The QoS variant shifts the body by two bytes; getting that wrong reads
     * the SNAP header from the wrong offset and silently sees nothing. */
    size_t n = mk_eapol(buf, sizeof(buf), 1, false, true, true);
    CHECK(pharos_dot11_eapol(buf, n, &e), "QoS data frame parses");
    CHECK_EQ(e.msg, 1);

    /* PMKID in message 1: the clientless route. */
    n = mk_eapol(buf, sizeof(buf), 1, true, false, true);
    CHECK(pharos_dot11_eapol(buf, n, &e), "PMKID frame parses");
    CHECK(e.has_pmkid, "PMKID KDE found");

    /* A group rekey is routine housekeeping, not a handshake. */
    n = mk_eapol(buf, sizeof(buf), 1, false, false, false);
    CHECK(pharos_dot11_eapol(buf, n, &e), "group rekey parses");
    CHECK(!e.is_pairwise, "group rekey is not pairwise");
    CHECK_EQ(e.msg, 0);

    /* A protected frame is ciphertext; parsing it would be reading noise. */
    n = mk_eapol(buf, sizeof(buf), 1, false, false, true);
    buf[1] |= 0x40;
    CHECK(!pharos_dot11_eapol(buf, n, &e), "protected frame refused");

    /* Not EAPOL at all. */
    n = mk_eapol(buf, sizeof(buf), 1, false, false, true);
    buf[24 + 6] = 0x08; buf[24 + 7] = 0x00; /* IPv4 ethertype */
    CHECK(!pharos_dot11_eapol(buf, n, &e), "plain IP data frame ignored");

    /* Management frames are not data frames. */
    n = mk_eapol(buf, sizeof(buf), 1, false, false, true);
    buf[0] = 0x80; /* beacon */
    CHECK(!pharos_dot11_eapol(buf, n, &e), "beacon ignored");

    /* THE safety property: truncation at every length must never read past the
     * buffer and never crash. Run under a sanitiser this is the test that
     * matters, because the input is chosen by whoever is transmitting. */
    const size_t full = mk_eapol(buf, sizeof(buf), 1, true, false, true);
    for (size_t cut = 0; cut <= full; cut++) {
        uint8_t tmp[256];
        memcpy(tmp, buf, cut);
        pharos_eapol_t junk;
        (void)pharos_dot11_eapol(tmp, cut, &junk); /* must simply not explode */
    }
    CHECK(true, "every truncation length survived");

    CHECK(!pharos_dot11_eapol(NULL, 40, &e), "NULL buffer refused");
    CHECK(!pharos_dot11_eapol(buf, full, NULL), "NULL out refused");
}

/* ---- the engine ------------------------------------------------------- */

/* These return a pointer into a small rotating pool rather than a value: the
 * call sites feed them straight to ph_observe(), and you cannot take the
 * address of a function's return value in C. */
static pharos_ev_dot11_t g_evpool[4];
static unsigned g_evnext;

static pharos_ev_dot11_t *ev_slot(void)
{
    pharos_ev_dot11_t *f = &g_evpool[g_evnext++ % 4u];
    memset(f, 0, sizeof(*f));
    return f;
}

static const pharos_ev_dot11_t *ev_deauth(const uint8_t *dst, const uint8_t *bssid)
{
    pharos_ev_dot11_t *f = ev_slot();
    f->type = PHAROS_FT_MGMT;
    f->subtype = PHAROS_ST_DEAUTH;
    memcpy(f->a1, dst, 6);
    memcpy(f->a2, bssid, 6);
    memcpy(f->a3, bssid, 6);
    return f;
}

static const pharos_ev_dot11_t *ev_eapol(int msg, bool pmkid, const uint8_t *bssid,
                                         const uint8_t *client)
{
    pharos_ev_dot11_t *f = ev_slot();
    f->type = PHAROS_FT_DATA;
    f->eapol = (uint8_t)msg;
    if (msg == 1 || msg == 3) {
        memcpy(f->a1, client, 6);
        memcpy(f->a2, bssid, 6);
    } else {
        memcpy(f->a1, bssid, 6);
        memcpy(f->a2, client, 6);
    }
    memcpy(f->a3, bssid, 6);
    if (pmkid) f->flags |= PHAROS_DOT11_F_PMKID;
    return f;
}

static ph_context_t ctx_of(uint16_t dwell, bool mfp)
{
    ph_context_t c;
    memset(&c, 0, sizeof(c));
    c.dwell_permil = dwell;
    c.yield_permil = 1000;
    c.mfp_required = mfp;
    return c;
}

/* An ordinary data frame FROM a client: proof we can hear that radio. */
static const pharos_ev_dot11_t *ev_from_client(const uint8_t *bssid,
                                               const uint8_t *client)
{
    pharos_ev_dot11_t *f = ev_slot();
    f->type = PHAROS_FT_DATA;
    memcpy(f->a1, bssid, 6);
    memcpy(f->a2, client, 6);
    memcpy(f->a3, bssid, 6);
    return f;
}

static void test_harvest_one_pmkid_is_a_tuesday(void)
{
    banner("harvest: a single PMKID request is not an attack");

    /* THE FALSE POSITIVE, REPORTED FROM HARDWARE.
     *
     * Camped on a quiet home network with nothing running against it, the
     * device said SUSPECTED 46/96, "A PMKID was requested and never
     * completed". PMKID in message 1 is ROUTINE - it is how PMK caching and
     * fast roaming work - so this accused an ordinary roam of being an
     * attack, on a network nobody was touching. */
    ph_state_t s;
    ph_reset(&s);

    const uint8_t ap[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
    const uint8_t cl[6] = { 0xaa, 0xbb, 0xcc, 0x01, 0x02, 0x03 };

    ph_observe(&s, ev_from_client(ap, cl), 1000);      /* audible client   */
    ph_observe(&s, ev_eapol(1, true, ap, cl), 2000);   /* one solicitation */
    ph_settle(&s);

    ph_context_t ctx = { .dwell_permil = 1000, .yield_permil = 1000 };
    ph_verdict_t v;
    ph_evaluate(&s, &ctx, &v);

    CHECK(v.pmkid_orphans == 1, "the orphan is still counted");
    CHECK(!(v.families & PH_FAM_PMKID), "but one does not light a family");
    CHECK(v.score < 40, "and does not reach SUSPECTED (%u)", v.score);
}

static void test_harvest_unheard_client_is_not_evidence(void)
{
    banner("harvest: a message is not missing from a radio we never hear");

    /* Message 1 comes from the ACCESS POINT, message 2 from the CLIENT, and
     * this device has one antenna. An AP at -40 dBm is loud; the phone it is
     * talking to may be in a pocket. Counting that silence as an abandoned
     * handshake measures our own antenna, not an attacker. */
    ph_state_t s;
    ph_reset(&s);

    const uint8_t ap[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x61 };
    const uint8_t cl[6] = { 0xaa, 0xbb, 0xcc, 0x01, 0x02, 0x04 };

    ph_observe(&s, ev_eapol(1, true, ap, cl), 2000); /* nothing FROM cl */
    ph_settle(&s);

    ph_context_t ctx = { .dwell_permil = 1000, .yield_permil = 1000 };
    ph_verdict_t v;
    ph_evaluate(&s, &ctx, &v);

    CHECK(v.pmkid_orphans == 0, "an unheard client orphans nothing");
    CHECK(!(v.families & PH_FAM_PMKID), "and lights no family");
}

static void test_harvest_repeated_pmkid_still_fires(void)
{
    banner("harvest: repeated unanswered requests are still the attack");

    /* The fix must not blind the lens to the thing it exists for. */
    ph_state_t s;
    ph_reset(&s);

    const uint8_t ap[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x62 };
    for (unsigned i = 0; i < 4u; i++) {
        const uint8_t cl[6] = { 0xaa, 0xbb, 0xcc, 0x09, 0x09, (uint8_t)i };
        ph_observe(&s, ev_from_client(ap, cl), 1000 + i);
        ph_observe(&s, ev_eapol(1, true, ap, cl), 2000 + i);
    }
    ph_settle(&s);

    ph_context_t ctx = { .dwell_permil = 1000, .yield_permil = 1000 };
    ph_verdict_t v;
    ph_evaluate(&s, &ctx, &v);

    CHECK(v.pmkid_orphans == 4, "all four are counted");
    CHECK(v.families & PH_FAM_PMKID, "and the family fires");
    CHECK(v.score >= 34, "with a score worth showing (%u)", v.score);
}

static const pharos_ev_dot11_t *ev_assoc(const uint8_t *bssid,
                                        const uint8_t *client)
{
    pharos_ev_dot11_t *f = ev_slot();
    f->type = PHAROS_FT_MGMT;
    f->subtype = PHAROS_ST_ASSOC_REQ;
    memcpy(f->a1, bssid, 6);
    memcpy(f->a2, client, 6);
    memcpy(f->a3, bssid, 6);
    return f;
}

static void test_harvest_touch_and_go(void)
{
    banner("harvest: joined the network and never used it");

    /* THE FRAME THAT ACTUALLY ARRIVES.
     *
     * A live PMKID attack ran against this device for several minutes and
     * every EAPOL counter stayed at zero - message 1 is one brief data frame
     * and camping on the right channel is not enough to be holding the
     * receiver open at the exact moment it passes. The association request
     * before it is MANAGEMENT: unencrypted, always delivered, unskippable. */
    ph_state_t s;
    ph_reset(&s);
    const uint8_t ap[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x70 };

    for (unsigned i = 0; i < 2u; i++) {
        const uint8_t cl[6] = { 0xde, 0xad, 0xbe, 0xef, 0x00, (uint8_t)i };
        ph_observe(&s, ev_assoc(ap, cl), 1000 + i * 1000);
    }

    ph_context_t ctx = { .dwell_permil = 1000, .yield_permil = 1000 };
    ph_verdict_t v;
    ph_evaluate(&s, &ctx, &v);

    CHECK(v.assoc_reqs == 2, "both approaches are seen");
    CHECK(v.touch_and_go == 2, "and neither carried any traffic");
    CHECK(v.families & PH_FAM_TOUCH_GO, "the family fires");
    CHECK(v.band >= PH_BAND_SUSPECTED, "and it is suspicious (%s)",
          ph_band_name(v.band));

    /* THE WORDS MUST NAME THE EVIDENCE THAT FIRED.
     *
     * On hardware this said "A handshake was captured in a way that looks
     * deliberate" while `handshakes seen` was zero: the association family
     * had raised the score and the headline still described the handshake
     * family. A verdict that misreports its own reason sends the operator
     * looking for the wrong thing. */
    CHECK(v.handshakes == 0, "no handshake was seen at all");
    CHECK(strstr(v.headline, "handshake") == NULL,
          "so the headline must not mention one: %s", v.headline);
    CHECK(strstr(v.headline, "join") != NULL,
          "it names the joining instead: %s", v.headline);
}

static void test_harvest_a_real_client_is_not_a_harvester(void)
{
    banner("harvest: a client that joins and USES the network is fine");

    /* THE NEGATIVE THIS EXISTS FOR. Every phone in the building associates.
     * What separates them from a harvester is that they go on to do
     * something. */
    ph_state_t s;
    ph_reset(&s);
    const uint8_t ap[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x71 };

    for (unsigned i = 0; i < 4u; i++) {
        const uint8_t cl[6] = { 0xc0, 0xff, 0xee, 0x00, 0x00, (uint8_t)i };
        ph_observe(&s, ev_assoc(ap, cl), 1000 + i * 1000);
        ph_observe(&s, ev_from_client(ap, cl), 2000 + i * 1000); /* uses it */
    }

    ph_context_t ctx = { .dwell_permil = 1000, .yield_permil = 1000 };
    ph_verdict_t v;
    ph_evaluate(&s, &ctx, &v);

    CHECK(v.assoc_reqs == 4, "all four approaches are seen");
    CHECK(v.touch_and_go == 0, "none of them is a touch-and-go");
    CHECK(!(v.families & PH_FAM_TOUCH_GO), "and the family stays quiet");
}

static void test_harvest_one_failed_join_is_not_an_attack(void)
{
    banner("harvest: one failed join is a wrong password, not a harvester");

    ph_state_t s;
    ph_reset(&s);
    const uint8_t ap[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x72 };
    const uint8_t cl[6] = { 0x0b, 0xad, 0x00, 0x00, 0x00, 0x01 };
    ph_observe(&s, ev_assoc(ap, cl), 1000);

    ph_context_t ctx = { .dwell_permil = 1000, .yield_permil = 1000 };
    ph_verdict_t v;
    ph_evaluate(&s, &ctx, &v);

    CHECK(v.touch_and_go == 1, "it is counted");
    CHECK(!(v.families & PH_FAM_TOUCH_GO), "but one does not light a family");
    CHECK(v.band < PH_BAND_SUSPECTED, "and does not accuse anybody (%s)",
          ph_band_name(v.band));
}

static void test_harvest_no_pmkid_offered_is_a_finding(void)
{
    banner("harvest: \"none offered\" is a result, not a blank");

    /* A ZERO ON THE PMKID ROW MEANT TWO OPPOSITE THINGS.
     *
     * "nothing has happened yet" and "this network cannot be attacked that
     * way at all" rendered identically. The second is a security finding -
     * the clientless attack simply does not work against an AP that hands out
     * no PMKIDs - and it was being drawn as an empty cell. */
    ph_state_t s;
    ph_reset(&s);
    const uint8_t ap[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x80 };
    const uint8_t cl[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

    ph_context_t ctx = { .dwell_permil = 1000, .yield_permil = 1000 };
    ph_verdict_t v;

    /* Nothing seen yet: no claim either way. */
    ph_evaluate(&s, &ctx, &v);
    CHECK(!(v.notes & PH_NOTE_NO_PMKID), "silence alone concludes nothing");
    CHECK(v.m1_seen == 0, "and reports an empty sample");

    /* One message 1 without a PMKID is not yet a sample. */
    ph_observe(&s, ev_eapol(1, false, ap, cl), 1000);
    ph_evaluate(&s, &ctx, &v);
    CHECK(!(v.notes & PH_NOTE_NO_PMKID), "one is not enough to conclude");

    /* Enough of them, and the absence becomes the finding. */
    for (unsigned i = 1; i < PH_PMKID_SAMPLE + 1u; i++) {
        ph_observe(&s, ev_eapol(1, false, ap, cl), 1000 + i * 1000);
    }
    ph_evaluate(&s, &ctx, &v);
    CHECK(v.notes & PH_NOTE_NO_PMKID, "a sample with none in it is a finding");
    CHECK(v.m1_with_pmkid == 0, "and none of them carried one");
    CHECK(v.m1_seen >= PH_PMKID_SAMPLE, "over a sample worth quoting (%u)",
          (unsigned)v.m1_seen);

    /* But one real PMKID retracts the claim immediately. */
    ph_observe(&s, ev_eapol(1, true, ap, cl), 9000);
    ph_evaluate(&s, &ctx, &v);
    CHECK(!(v.notes & PH_NOTE_NO_PMKID),
          "a single PMKID withdraws \"none offered\"");
    CHECK(v.m1_with_pmkid == 1, "and is counted");
}

void test_harvest(void)
{
    test_harvest_no_pmkid_offered_is_a_finding();
    test_harvest_touch_and_go();
    test_harvest_a_real_client_is_not_a_harvester();
    test_harvest_one_failed_join_is_not_an_attack();
    test_harvest_one_pmkid_is_a_tuesday();
    test_harvest_unheard_client_is_not_evidence();
    test_harvest_repeated_pmkid_still_fires();
    test_eapol_parser();

    banner("harvest: forced captures vs ordinary handshakes");
    ph_state_t s;
    ph_verdict_t v;
    ph_context_t camped = ctx_of(1000, false);

    /* Silence. */
    ph_reset(&s);
    ph_evaluate(&s, &camped, &v);
    CHECK_EQ(v.band, PH_BAND_QUIET);
    CHECK_EQ(v.score, 0);
    CHECK(strstr(ph_band_advice(v.band), "one channel at a time") != NULL,
          "even QUIET admits the receiver's limit");

    /* Ordinary joins: handshakes with nothing forcing them. This is what a
     * normal office looks like and it must never alarm. */
    ph_reset(&s);
    for (int i = 0; i < 6; i++) {
        uint8_t sta[6] = { 0x02, 0xCC, 0xDD, 0x00, 0x00, (uint8_t)i };
        ph_observe(&s, ev_eapol(1, false, AP, sta), 1000000ull * (uint64_t)i);
        ph_observe(&s, ev_eapol(2, false, AP, sta), 1000000ull * (uint64_t)i + 20000ull);
    }
    ph_settle(&s);
    ph_evaluate(&s, &camped, &v);
    CHECK_EQ(v.band, PH_BAND_HANDSHAKES);
    CHECK_EQ(v.forced_cycles, 0);
    CHECK_EQ(v.families, 0);
    CHECK(v.handshakes >= 6, "handshakes counted (%u)", v.handshakes);

    /* One disconnect then one handshake: an access point rebooting looks
     * exactly like this. It must NOT be a family, and must not alarm. */
    ph_reset(&s);
    ph_observe(&s, ev_deauth(STA, AP), 1000000ull);
    ph_observe(&s, ev_eapol(1, false, AP, STA), 1500000ull);
    ph_observe(&s, ev_eapol(2, false, AP, STA), 1520000ull);
    ph_settle(&s);
    ph_evaluate(&s, &camped, &v);
    CHECK_EQ(v.forced_cycles, 1);
    CHECK(!(v.families & PH_FAM_FORCED), "a single cycle is not a family");
    CHECK(v.band <= PH_BAND_HANDSHAKES, "one cycle does not alarm (got %s)",
          ph_band_name(v.band));

    /* A handshake long after a disconnect is not caused by it. */
    ph_reset(&s);
    ph_observe(&s, ev_deauth(STA, AP), 1000000ull);
    ph_observe(&s, ev_eapol(1, false, AP, STA), 1000000ull + PH_FORCE_WINDOW_US + 1);
    ph_settle(&s);
    ph_evaluate(&s, &camped, &v);
    CHECK_EQ(v.forced_cycles, 0);

    /* THE detection: repeated forcing of one victim. */
    ph_reset(&s);
    for (int i = 0; i < 5; i++) {
        const uint64_t t = 10000000ull * (uint64_t)i;
        ph_observe(&s, ev_deauth(STA, AP), t);
        ph_observe(&s, ev_eapol(1, false, AP, STA), t + 400000ull);
        ph_observe(&s, ev_eapol(2, false, AP, STA), t + 420000ull);
    }
    ph_settle(&s);
    ph_evaluate(&s, &camped, &v);
    CHECK_EQ(v.forced_cycles, 5);
    CHECK(v.families & PH_FAM_FORCED, "forced family raised");
    CHECK(v.families & PH_FAM_REPEAT, "repeat family raised");
    CHECK_EQ(v.band, PH_BAND_HARVEST_LIKELY);
    CHECK(memcmp(v.worst_client, STA, 6) == 0, "names the victim");
    CHECK(strstr(ph_band_advice(v.band), "802.11w") != NULL,
          "advice names the actual fix");

    /* Breadth: the same pattern across several victims is a tool sweeping. */
    ph_reset(&s);
    for (int c = 0; c < 4; c++) {
        uint8_t sta[6] = { 0x02, 0xCC, 0xDD, 0x00, 0x00, (uint8_t)(0x40 + c) };
        for (int i = 0; i < 2; i++) {
            const uint64_t t = 10000000ull * (uint64_t)i + 100000ull * (uint64_t)c;
            ph_observe(&s, ev_deauth(sta, AP), t);
            ph_observe(&s, ev_eapol(1, false, AP, sta), t + 300000ull);
        }
    }
    ph_settle(&s);
    ph_evaluate(&s, &camped, &v);
    CHECK_EQ(v.victims, 4);
    CHECK(v.families & PH_FAM_BREADTH, "breadth family raised");
    CHECK_EQ(v.band, PH_BAND_HARVEST_LIKELY);

    /* The clientless attack: PMKIDs solicited and never completed.
     *
     * TWO, not one, and from clients we can actually hear. This test used to
     * assert that a SINGLE unanswered request raised the family and reached
     * SUSPECTED - and that is exactly the false positive the device produced
     * on a quiet home network: one ordinary roam, 46/96, "somebody is
     * collecting your handshakes". PMKID in message 1 is how fast roaming
     * works; one of them is a Tuesday. */
    ph_reset(&s);
    for (unsigned i = 0; i < 2u; i++) {
        const uint8_t cl[6] = { 0x0d, 0x0e, 0x0a, 0x0d, 0x00, (uint8_t)i };
        ph_observe(&s, ev_from_client(AP, cl), 900000ull + i);
        ph_observe(&s, ev_eapol(1, true, AP, cl), 1000000ull + i);
    }
    ph_settle(&s); /* the sweep ended; no message 2 ever came */
    ph_evaluate(&s, &camped, &v);
    CHECK_EQ(v.pmkid_orphans, 2);
    CHECK(v.families & PH_FAM_PMKID, "pmkid family raised");
    CHECK(v.band >= PH_BAND_SUSPECTED, "unanswered PMKIDs are suspicious (got %s)",
          ph_band_name(v.band));
    CHECK(strstr(v.headline, "PMKID") != NULL, "headline: %s", v.headline);

    /* But a PMKID that IS answered is just a modern client connecting. */
    ph_reset(&s);
    ph_observe(&s, ev_eapol(1, true, AP, STA), 1000000ull);
    ph_observe(&s, ev_eapol(2, false, AP, STA), 1050000ull);
    ph_settle(&s);
    ph_evaluate(&s, &camped, &v);
    CHECK_EQ(v.pmkid_orphans, 0);
    CHECK(!(v.families & PH_FAM_PMKID), "a completed PMKID is not an attack");
    CHECK(v.band <= PH_BAND_HANDSHAKES, "normal client does not alarm");

    /* A broadcast disconnect arms every known conversation on that AP - which
     * is exactly what it does on the air. */
    ph_reset(&s);
    {
        const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        uint8_t s1[6] = { 0x02, 0xCC, 0xDD, 0x00, 0x00, 0x11 };
        uint8_t s2[6] = { 0x02, 0xCC, 0xDD, 0x00, 0x00, 0x22 };
        ph_observe(&s, ev_eapol(1, false, AP, s1), 1000ull);
        ph_observe(&s, ev_eapol(1, false, AP, s2), 2000ull);
        for (int i = 1; i <= 3; i++) {
            const uint64_t t = 10000000ull * (uint64_t)i;
            ph_observe(&s, ev_deauth(bcast, AP), t);
            ph_observe(&s, ev_eapol(1, false, AP, s1), t + 200000ull);
            ph_observe(&s, ev_eapol(1, false, AP, s2), t + 250000ull);
        }
        ph_settle(&s);
        ph_evaluate(&s, &camped, &v);
        CHECK(v.forced_cycles >= 6, "broadcast disconnect arms both (%u)",
              v.forced_cycles);
        CHECK_EQ(v.victims, 2);
    }

    /* 802.11w: forged disconnects are rejected, so forcing evidence is worth
     * much less. Same frames, materially lower score. */
    {
        ph_state_t s2;
        ph_verdict_t open_v, mfp_v;
        ph_context_t mfp_ctx = ctx_of(1000, true);
        ph_reset(&s2);
        for (int i = 0; i < 5; i++) {
            const uint64_t t = 10000000ull * (uint64_t)i;
            ph_observe(&s2, ev_deauth(STA, AP), t);
            ph_observe(&s2, ev_eapol(1, false, AP, STA), t + 400000ull);
        }
        ph_settle(&s2);
        ph_evaluate(&s2, &camped, &open_v);
        ph_evaluate(&s2, &mfp_ctx, &mfp_v);
        CHECK(mfp_v.score < open_v.score,
              "802.11w discounts forced evidence (%u < %u)", mfp_v.score, open_v.score);
        CHECK(mfp_v.notes & PH_NOTE_MFP, "MFP disclosed");
    }

    /* Ceilings and caps behave like every other engine. */
    {
        ph_context_t thin = ctx_of(71, false);
        ph_context_t lossy = ctx_of(1000, false);
        lossy.yield_permil = 500;
        CHECK(ph_ceiling(&camped) > ph_ceiling(&thin), "camping buys confidence");
        CHECK(ph_ceiling(&camped) > ph_ceiling(&lossy),
              "dropped frames break the ordering, so they cost confidence");
        CHECK(ph_ceiling(&camped) < 100, "nothing here is certain");

        /* A single family cannot reach the alarm band, however loud. */
        ph_state_t s3;
        ph_verdict_t v3;
        ph_reset(&s3);
        for (int i = 0; i < 40; i++) {
            ph_observe(&s3, ev_eapol(1, true, AP, STA), 1000000ull * (uint64_t)i);
            ph_settle(&s3);
        }
        ph_evaluate(&s3, &camped, &v3);
        CHECK(v3.score <= 62, "one family alone is capped (got %u)", v3.score);
        CHECK(v3.band < PH_BAND_HARVEST_LIKELY,
              "one family cannot raise the alarm");
    }

    /* The pair table is bounded and says so rather than overrunning. */
    {
        ph_state_t big;
        ph_verdict_t bv;
        ph_reset(&big);
        for (unsigned i = 0; i < PH_MAX_PAIRS + 10u; i++) {
            uint8_t sta[6] = { 0x02, 0x11, 0x22, 0x33, (uint8_t)(i >> 8), (uint8_t)i };
            ph_observe(&big, ev_eapol(1, false, AP, sta), 1000ull * i);
        }
        ph_evaluate(&big, &camped, &bv);
        CHECK(bv.notes & PH_NOTE_FULL, "table full is disclosed, not silent");
    }

    /* Vocabulary: nothing may promise safety. */
    for (int b = PH_BAND_QUIET; b < PH_BAND_COUNT; b++) {
        const char *nm = ph_band_name((ph_band_t)b);
        const char *ad = ph_band_advice((ph_band_t)b);
        CHECK(nm && *nm && ad && *ad, "band %d described", b);
        CHECK(strstr(nm, "SAFE") == NULL && strstr(nm, "SECURE") == NULL,
              "no band claims safety");
        CHECK(strstr(ad, " is safe") == NULL, "advice never says safe");
    }
    CHECK(strcmp(ph_family_name(PH_FAM_PMKID), "pmkid") == 0, "families named");
}
