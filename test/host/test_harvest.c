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

void test_harvest(void)
{
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

    /* The clientless attack: a PMKID solicited and never completed. */
    ph_reset(&s);
    ph_observe(&s, ev_eapol(1, true, AP, STA), 1000000ull);
    ph_settle(&s); /* the sweep ended; no message 2 ever came */
    ph_evaluate(&s, &camped, &v);
    CHECK_EQ(v.pmkid_orphans, 1);
    CHECK(v.families & PH_FAM_PMKID, "pmkid family raised");
    CHECK(v.band >= PH_BAND_SUSPECTED, "an unanswered PMKID is suspicious (got %s)",
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
