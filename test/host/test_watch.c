/* Pharos host tests - the deauthentication watch engine (v2).
 *
 * The engine's claims are only worth what these assertions are worth, so this
 * file is written to attack it rather than to demonstrate it. In particular
 * every forgery test has a matching NEGATIVE test: a benign situation that
 * would fool a naive version of the same idea. Those negatives are the point.
 *
 *   sequence order    -> gaps from frames we never heard must NOT fire it,
 *                        and a genuinely busy access point must NOT fire it
 *   signal level      -> a beacon that naturally fades 12 dB must NOT fire it
 *   aftermath         -> a network where clients associate all day must NOT
 *                        fire it
 *   volume + shape    -> together they must never reach the alarm band, no
 *                        matter how extreme, because a busy network is not an
 *                        attack
 */
#include <stdint.h>
#include <string.h>

#include "pharos_watch.h"
#include "test_support.h"

#define T0 100000000ull /* 100 s, well clear of the slot epoch */

static const uint8_t BCAST[6]   = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t CLIENT1[6] = { 0x02, 0xC1, 0x00, 0x00, 0x00, 0x11 };
static const uint8_t CLIENT2[6] = { 0x02, 0xC1, 0x00, 0x00, 0x00, 0x22 };
static const uint8_t CLIENT3[6] = { 0x02, 0xC1, 0x00, 0x00, 0x00, 0x33 };

/* ---- a simulated access point ---------------------------------------
 *
 * Carries its own 802.11 sequence counter, because every interesting thing
 * this engine does depends on that counter behaving the way a real radio's
 * does: one counter per transmitter, advancing on every frame it sends,
 * including the ones this receiver never hears. */
typedef struct {
    uint8_t bssid[6];
    uint16_t seq;       /* where its counter is now                     */
    uint16_t per_beacon;/* how far it advances between beacons (traffic) */
    int8_t rssi;
    uint8_t rsn;        /* PHAROS_RSN_F_*                               */
    const char *ssid;
} ap_t;

static ap_t mk_ap(uint8_t tag, int8_t rssi, uint8_t rsn, uint16_t per_beacon)
{
    ap_t a;
    memset(&a, 0, sizeof(a));
    a.bssid[0] = 0x02; a.bssid[1] = 0xAA; a.bssid[2] = 0xBB;
    a.bssid[3] = 0x00; a.bssid[4] = 0x00; a.bssid[5] = tag;
    a.seq = 1000;
    a.per_beacon = per_beacon ? per_beacon : 1;
    a.rssi = rssi;
    a.rsn = rsn;
    a.ssid = "testnet";
    return a;
}

static pharos_ev_dot11_t frame(uint8_t subtype, const uint8_t *a1,
                               const uint8_t *a2, int8_t rssi, uint16_t reason,
                               uint16_t seq, uint8_t flags, uint8_t rsn)
{
    pharos_ev_dot11_t f;
    memset(&f, 0, sizeof(f));
    f.type = PHAROS_FT_MGMT;
    f.subtype = subtype;
    memcpy(f.a1, a1, 6);
    memcpy(f.a2, a2, 6);
    memcpy(f.a3, a2, 6);
    f.rssi = rssi;
    f.channel = 6;
    f.reason_or_status = reason;
    f.seq = seq;
    f.flags = flags;
    f.rsn_flags = rsn;
    return f;
}

/* One beacon, advancing the AP's counter the way traffic would. */
static void beacon(pw_engine_t *e, ap_t *ap, uint64_t t_us, int8_t rssi_override)
{
    ap->seq = (uint16_t)((ap->seq + ap->per_beacon) & 0x0FFF);
    pharos_ev_dot11_t b = frame(PHAROS_ST_BEACON, BCAST, ap->bssid,
                                rssi_override ? rssi_override : ap->rssi, 0,
                                ap->seq, 0, ap->rsn);
    const size_t n = strlen(ap->ssid);
    memcpy(b.ssid, ap->ssid, n);
    b.ssid_len = (uint8_t)n;
    pw_observe(e, &b, t_us);
}

/* `n` beacons at a 100 ms interval starting at `t0`. */
static void beacons(pw_engine_t *e, ap_t *ap, int n, uint64_t t0)
{
    for (int i = 0; i < n; i++) {
        beacon(e, ap, t0 + (uint64_t)i * 100000ull, 0);
    }
}

/* A disconnect the access point genuinely sent: its own counter, its own
 * signal level. Nothing here should ever look like a forgery. */
static void genuine_deauth(pw_engine_t *e, ap_t *ap, const uint8_t *dst,
                           uint64_t t_us, uint16_t reason)
{
    ap->seq = (uint16_t)((ap->seq + 1) & 0x0FFF);
    pharos_ev_dot11_t d = frame(PHAROS_ST_DEAUTH, dst, ap->bssid, ap->rssi,
                                reason, ap->seq, 0, 0);
    pw_observe(e, &d, t_us);
}

/* A disconnect somebody else sent while wearing the AP's address. */
static void spoofed_deauth(pw_engine_t *e, const ap_t *ap, const uint8_t *dst,
                           uint64_t t_us, uint16_t reason, uint16_t seq,
                           int8_t rssi)
{
    pharos_ev_dot11_t d = frame(PHAROS_ST_DEAUTH, dst, ap->bssid, rssi, reason,
                                seq, 0, 0);
    pw_observe(e, &d, t_us);
}

static void rejoin(pw_engine_t *e, ap_t *ap, const uint8_t *client, uint64_t t_us)
{
    pharos_ev_dot11_t a = frame(PHAROS_ST_AUTH, ap->bssid, client, -55, 0, 7, 0, 0);
    pw_observe(e, &a, t_us);
    pharos_ev_dot11_t s = frame(PHAROS_ST_ASSOC_REQ, ap->bssid, client, -55, 0, 8, 0, 0);
    pw_observe(e, &s, t_us + 2000ull);
}

static pw_context_t ctx_of(uint16_t dwell, uint16_t yield)
{
    pw_context_t c;
    c.dwell_permil = dwell;
    c.bus_yield_permil = yield;
    c.window_ms = 10000;
    return c;
}

/* ---- the ceiling ----------------------------------------------------- */

static void test_watch_ceiling(void)
{
    banner("watch: the confidence ceiling");

    pw_context_t camped = ctx_of(1000, 1000);
    pw_context_t hopping = ctx_of(77, 1000); /* 200 ms across 13 channels */
    pw_context_t lossy = ctx_of(1000, 300);

    CHECK_EQ(pw_ceiling(&camped), PW_CEILING_MAX);
    CHECK(pw_ceiling(&camped) < 100, "nothing this device sees is certain");
    CHECK(pw_ceiling(&hopping) < 75,
          "hopping alone must not reach the alarm band (got %u)",
          pw_ceiling(&hopping));
    CHECK(pw_ceiling(&lossy) < pw_ceiling(&camped), "frame loss lowers confidence");

    /* Monotone in dwell: standing still must never be worth less. */
    uint8_t prev = 0;
    for (uint16_t d = 1; d <= 1000; d += 37) {
        pw_context_t c = ctx_of(d, 1000);
        const uint8_t got = pw_ceiling(&c);
        CHECK(got >= prev, "ceiling must not fall as dwell rises (%u -> %u)", prev, got);
        prev = got;
    }
    pw_context_t floorish = ctx_of(1, 1);
    CHECK_EQ(pw_ceiling(&floorish), 45);
}

/* ---- the quiet and ordinary cases ------------------------------------ */

static void test_watch_quiet(void)
{
    banner("watch: an empty sky and an ordinary one");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    pw_evaluate(&e, T0 + 10000000ull, &c, &v);
    CHECK_EQ(v.band, PW_BAND_QUIET);
    CHECK_EQ(v.score, 0);
    CHECK_EQ(v.families, 0);

    /* A healthy network: beacons, and a client genuinely timed out now and
     * then. The access point's own counter, its own signal level. */
    pw_reset(&e);
    ap_t ap = mk_ap(0x01, -48, PHAROS_RSN_F_PRESENT | PHAROS_RSN_F_PSK, 3);
    for (int i = 0; i < 100; i++) {
        beacon(&e, &ap, T0 + (uint64_t)i * 100000ull, 0);
        if (i == 30 || i == 70) {
            genuine_deauth(&e, &ap, CLIENT1, T0 + (uint64_t)i * 100000ull + 5000ull, 4);
        }
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);
    CHECK(v.band <= PW_BAND_BACKGROUND,
          "an ordinary network must not be graded above BACKGROUND (got %s/%u)",
          pw_band_name(v.band), v.score);
    CHECK_EQ(v.forgery, 0);
}

/* THE ONE THAT MATTERS MOST. An access point that is genuinely, legitimately
 * disconnecting a lot of clients - a captive portal cycling its guests, say -
 * produces volume and shape and nothing else. It must never alarm. */
static void test_watch_volume_alone_never_alarms(void)
{
    banner("watch: volume and shape alone can never reach the alarm band");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x02, -50, PHAROS_RSN_F_PRESENT, 2);
    beacons(&e, &ap, 100, T0);

    /* Absurd volume, broadcast, many victims, tight per-victim bursts - every
     * volume and shape signal at once, all of it genuinely from the access
     * point on its own counter and at its own signal level. */
    const uint8_t *victims[3] = { CLIENT1, CLIENT2, CLIENT3 };
    for (int i = 0; i < 900; i++) {
        genuine_deauth(&e, &ap, (i % 40 == 0) ? BCAST : victims[(i / 12) % 3],
                       T0 + (uint64_t)i * 11000ull, 7);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    CHECK(v.c_rate >= 25, "the rate family must be shouting (got %u)", v.c_rate);
    CHECK(v.c_shape >= 8, "the shape family must be firing (got %u)", v.c_shape);
    CHECK_EQ(v.forgery, 0);
    CHECK((v.families & PW_FAM_FORGERY) == 0, "nothing was forged");
    CHECK(v.score <= PW_CAP_NO_CORROBORATION,
          "volume+shape must stay below the alarm band (got %u)", v.score);
    CHECK(v.band < PW_BAND_LIKELY, "must not alarm: got %s", pw_band_name(v.band));
}

/* The rule above, asserted as an INVARIANT rather than on one fixture: sweep
 * the attacker's intensity across three orders of magnitude and check that a
 * verdict which found no forgery and no aftermath never reaches the alarm
 * band, whatever the numbers underneath it say. */
static void test_watch_no_corroboration_invariant(void)
{
    banner("watch: no forgery and no aftermath means no alarm, at any volume");
    pw_context_t c = ctx_of(1000, 1000);

    for (int step = 0; step < 24; step++) {
        const uint64_t gap_us = 2000ull + (uint64_t)step * 4000ull;
        pw_engine_t e;
        pw_verdict_t v;
        pw_reset(&e);
        ap_t ap = mk_ap((uint8_t)(0x40 + step), -50, PHAROS_RSN_F_PRESENT, 2);
        beacons(&e, &ap, 100, T0);
        const uint8_t *victims[3] = { CLIENT1, CLIENT2, CLIENT3 };
        for (int i = 0; i < 2000 && (uint64_t)i * gap_us < 9000000ull; i++) {
            genuine_deauth(&e, &ap, (i % 30 == 0) ? BCAST : victims[(i / 10) % 3],
                           T0 + (uint64_t)i * gap_us, 7);
        }
        pw_evaluate(&e, T0 + 9900000ull, &c, &v);

        const bool corroborated =
            (v.families & (PW_FAM_FORGERY | PW_FAM_AFTERMATH)) != 0;
        if (!corroborated) {
            CHECK(v.score < 75,
                  "uncorroborated verdict reached the alarm band at gap %llu us "
                  "(score %u, families 0x%02x)",
                  (unsigned long long)gap_us, v.score, v.families);
        }
    }
}

static void test_watch_one_and_two_family_caps(void)
{
    banner("watch: one family cannot leave ELEVATED, two cannot leave SUSPICIOUS");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    /* Rate only: unicast at one victim, one at a time, no bursts. */
    pw_reset(&e);
    ap_t ap = mk_ap(0x03, -50, 0, 2);
    beacons(&e, &ap, 100, T0);
    for (int i = 0; i < 120; i++) {
        genuine_deauth(&e, &ap, CLIENT1, T0 + (uint64_t)i * 80000ull, 7);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    unsigned fams = 0;
    for (unsigned b = 0; b < 4; b++) if (v.families & (1u << b)) fams++;
    if (fams == 1) {
        CHECK(v.score <= PW_CAP_ONE_FAMILY,
              "one family capped at %u (got %u)", PW_CAP_ONE_FAMILY, v.score);
    }
    if (fams == 2) {
        CHECK(v.score <= PW_CAP_TWO_FAMILIES,
              "two families capped at %u (got %u)", PW_CAP_TWO_FAMILIES, v.score);
    }
    CHECK(fams >= 1, "something must have fired");
}

/* ---- forgery: the 802.11w contradiction ------------------------------ */

static void test_watch_mfp_proof(void)
{
    banner("watch: unprotected deauth on an 802.11w-required network is proof");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x04, -45,
                    PHAROS_RSN_F_PRESENT | PHAROS_RSN_F_MFP_CAPABLE |
                        PHAROS_RSN_F_MFP_REQUIRED, 2);
    beacons(&e, &ap, 60, T0);
    for (int i = 0; i < 200; i++) {
        spoofed_deauth(&e, &ap, BCAST, T0 + (uint64_t)i * 40000ull, 7, 40, -70);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    CHECK(v.forgery & PW_FORGE_MFP_PROOF, "the contradiction was found");
    CHECK(v.notes & PW_NOTE_HARD, "it counts as hard evidence");
    CHECK(v.notes & PW_NOTE_MFP_TARGET, "the operator is told the net runs 802.11w");
    CHECK(v.families & PW_FAM_FORGERY, "the forgery family fires");
    CHECK(v.band >= PW_BAND_SUSPICIOUS, "got %s/%u", pw_band_name(v.band), v.score);
}

/* THE PAYOFF. The same proof, found while hopping. v1 could not report this
 * above SUSPICIOUS however certain it was, because the ceiling punished all
 * evidence for hopping. A contradiction is not weakened by a short visit. */
static void test_watch_hard_evidence_survives_hopping(void)
{
    banner("watch: a proven contradiction is not weakened by hopping");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t hopping = ctx_of(77, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x05, -45,
                    PHAROS_RSN_F_PRESENT | PHAROS_RSN_F_MFP_CAPABLE |
                        PHAROS_RSN_F_MFP_REQUIRED, 2);
    beacons(&e, &ap, 60, T0);
    for (int i = 0; i < 300; i++) {
        spoofed_deauth(&e, &ap, (i % 3) ? BCAST : CLIENT1,
                       T0 + (uint64_t)i * 30000ull, 7, (uint16_t)(60 + i % 2), -72);
        if (i % 7 == 0) {
            rejoin(&e, &ap, CLIENT2, T0 + (uint64_t)i * 30000ull + 1000ull);
        }
    }
    pw_evaluate(&e, T0 + 9900000ull, &hopping, &v);

    CHECK(v.notes & PW_NOTE_HARD, "hard evidence recognised while hopping");
    CHECK_EQ(v.ceiling, PW_CEILING_HARD_EVIDENCE);
    CHECK(v.ceiling > pw_ceiling(&hopping),
          "the ceiling rose above the dwell-only value (%u vs %u)",
          v.ceiling, pw_ceiling(&hopping));
    CHECK(v.ceiling < 100, "and it is still not certainty");
}

static void test_watch_mfp_capable_is_only_a_hint(void)
{
    banner("watch: 802.11w capable-but-not-required is a hint, not proof");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x06, -45,
                    PHAROS_RSN_F_PRESENT | PHAROS_RSN_F_MFP_CAPABLE, 2);
    beacons(&e, &ap, 60, T0);
    for (int i = 0; i < 40; i++) {
        /* Genuine: the AP's own counter and level, so only the MFP question
         * is on the table. A client that never negotiated protection can
         * legitimately be disconnected in the clear. */
        genuine_deauth(&e, &ap, CLIENT1, T0 + (uint64_t)i * 200000ull, 4);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    CHECK((v.forgery & PW_FORGE_MFP_PROOF) == 0,
          "capable-only must never be reported as proof");
    CHECK((v.notes & PW_NOTE_HARD) == 0, "and must not raise the ceiling");
    CHECK_EQ(v.ceiling, pw_ceiling(&c));
}

/* ---- forgery: sequence order ----------------------------------------- */

static void test_watch_seq_order_violation(void)
{
    banner("watch: a counter that runs backwards is a forgery");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x07, -45, PHAROS_RSN_F_PRESENT, 2);
    beacons(&e, &ap, 40, T0);

    /* The attacker's counter starts wherever its own driver started - far
     * behind where this AP's counter has already got to. */
    for (int i = 0; i < 60; i++) {
        const uint64_t t = T0 + 4000000ull + (uint64_t)i * 60000ull;
        spoofed_deauth(&e, &ap, BCAST, t, 7, (uint16_t)(50 + i), -68);
        /* the real AP keeps beaconing right through it */
        beacon(&e, &ap, t + 30000ull, 0);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    CHECK(v.forgery & PW_FORGE_SEQ_ORDER, "the order violation was found");
    CHECK(v.seq_violations >= 3, "violations counted (got %u)", v.seq_violations);
    CHECK(v.notes & PW_NOTE_HARD, "order violations are dwell-independent");
}

/* THE NEGATIVE THAT JUSTIFIES THE WHOLE APPROACH. Threshold-on-gap-size
 * schemes are documented to false-positive on frames the receiver simply
 * missed. Testing ORDER instead must not: a huge forward gap is exactly what
 * missing 900 frames looks like, and it is not evidence of anything. */
static void test_watch_seq_gaps_are_not_violations(void)
{
    banner("watch: frames we never heard widen gaps but must not accuse anyone");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    /* An access point whose counter leaps 900 between the beacons we hear,
     * because it is carrying data frames this receiver is not shown. */
    ap_t ap = mk_ap(0x08, -50, PHAROS_RSN_F_PRESENT, 900);
    beacons(&e, &ap, 40, T0);
    for (int i = 0; i < 60; i++) {
        const uint64_t t = T0 + 4000000ull + (uint64_t)i * 60000ull;
        genuine_deauth(&e, &ap, CLIENT1, t, 7);
        beacon(&e, &ap, t + 30000ull, 0);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    CHECK((v.forgery & PW_FORGE_SEQ_ORDER) == 0,
          "a busy access point is not a liar (violations=%u)", v.seq_violations);
    CHECK_EQ(v.seq_violations, 0);
}

static void test_watch_seq_frozen(void)
{
    banner("watch: a counter that never moves is a forgery");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x09, -45, PHAROS_RSN_F_PRESENT, 2);
    beacons(&e, &ap, 40, T0);
    for (int i = 0; i < 40; i++) {
        /* Every frame carries sequence 0 - the classic hand-built frame. */
        spoofed_deauth(&e, &ap, BCAST, T0 + 4000000ull + (uint64_t)i * 100000ull,
                       7, 0, -46);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    CHECK(v.forgery & PW_FORGE_SEQ_FROZEN, "the frozen counter was found");
    CHECK(v.seq_distinct <= 2, "distinct sequence values (got %u)", v.seq_distinct);
}

static void test_watch_advancing_counter_is_not_frozen(void)
{
    banner("watch: a normally advancing counter is not accused of freezing");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x0A, -45, PHAROS_RSN_F_PRESENT, 2);
    beacons(&e, &ap, 40, T0);
    for (int i = 0; i < 40; i++) {
        genuine_deauth(&e, &ap, CLIENT1, T0 + 4000000ull + (uint64_t)i * 100000ull, 7);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);
    CHECK((v.forgery & PW_FORGE_SEQ_FROZEN) == 0, "not frozen");
    CHECK(v.seq_distinct > 2, "many distinct values (got %u)", v.seq_distinct);
}

/* ---- forgery: signal level ------------------------------------------- */

static void test_watch_rssi_split(void)
{
    banner("watch: two transmitters wearing one address");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    /* A rock-steady beacon at -70, and disconnects arriving at -35: the
     * attacker is in the room and the access point is not. */
    ap_t ap = mk_ap(0x0B, -70, PHAROS_RSN_F_PRESENT, 2);
    beacons(&e, &ap, 60, T0);
    for (int i = 0; i < 60; i++) {
        spoofed_deauth(&e, &ap, BCAST, T0 + 4000000ull + (uint64_t)i * 60000ull,
                       7, (uint16_t)(2000 + i), -35);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    CHECK(v.forgery & PW_FORGE_RSSI_SPLIT, "the level split was found");
    CHECK(v.rssi_delta >= 20, "delta reported (got %d dB)", (int)v.rssi_delta);
}

/* The negative: a distant access point whose own beacons swing 12 dB. A flat
 * 10 dB threshold - which is what v1 used - calls this a forgery. Comparing
 * against the beacon's OWN spread does not. */
static void test_watch_fading_ap_is_not_a_forgery(void)
{
    banner("watch: an access point that fades is not two access points");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x0C, -70, PHAROS_RSN_F_PRESENT, 2);
    static const int8_t swing[8] = { -58, -82, -62, -78, -60, -80, -64, -76 };
    for (int i = 0; i < 80; i++) {
        beacon(&e, &ap, T0 + (uint64_t)i * 100000ull, swing[i % 8]);
    }
    /* Genuine disconnects, arriving 12 dB off the running mean - well inside
     * the spread this beacon has been showing all along. */
    for (int i = 0; i < 30; i++) {
        ap.seq = (uint16_t)((ap.seq + 1) & 0x0FFF);
        pharos_ev_dot11_t d = frame(PHAROS_ST_DEAUTH, CLIENT1, ap.bssid, -58, 4,
                                    ap.seq, 0, 0);
        pw_observe(&e, &d, T0 + 4000000ull + (uint64_t)i * 100000ull);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    CHECK((v.forgery & PW_FORGE_RSSI_SPLIT) == 0,
          "a fading beacon must not be called a forgery (delta=%d spread=%d)",
          (int)v.rssi_delta, (int)v.rssi_spread);
    CHECK(v.rssi_spread >= 5, "the spread was actually measured (got %d)",
          (int)v.rssi_spread);
}

/* ---- the aftermath family -------------------------------------------- */

static void test_watch_aftermath_stampede(void)
{
    banner("watch: clients coming straight back is evidence it worked");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x0D, -50, PHAROS_RSN_F_PRESENT, 2);
    beacons(&e, &ap, 30, T0);

    /* One hard burst in second 4, then the whole room reassociates in 5-6. */
    for (int i = 0; i < 40; i++) {
        spoofed_deauth(&e, &ap, BCAST, T0 + 4000000ull + (uint64_t)i * 20000ull,
                       7, (uint16_t)(300 + i), -60);
    }
    const uint8_t *clients[3] = { CLIENT1, CLIENT2, CLIENT3 };
    for (int i = 0; i < 12; i++) {
        rejoin(&e, &ap, clients[i % 3], T0 + 5100000ull + (uint64_t)i * 90000ull);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    CHECK(v.rejoins_after >= 3, "the stampede was correlated (got %u of %u)",
          v.rejoins_after, v.rejoins);
    CHECK(v.families & PW_FAM_AFTERMATH, "the aftermath family fires");
    CHECK(v.c_aftermath > 0, "and it scores (got %u)", v.c_aftermath);
}

/* The negative: a busy network where clients associate all day long, with no
 * disconnect burst to attribute them to. */
static void test_watch_ordinary_associations_are_not_aftermath(void)
{
    banner("watch: routine associations are not an aftermath");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x0E, -50, PHAROS_RSN_F_PRESENT, 2);
    beacons(&e, &ap, 100, T0);
    const uint8_t *clients[3] = { CLIENT1, CLIENT2, CLIENT3 };
    for (int i = 0; i < 60; i++) {
        rejoin(&e, &ap, clients[i % 3], T0 + (uint64_t)i * 150000ull);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    CHECK_EQ(v.rejoins_after, 0);
    CHECK((v.families & PW_FAM_AFTERMATH) == 0,
          "no burst to attribute the traffic to");
    CHECK_EQ(v.c_aftermath, 0);
}

/* ---- the full attack ------------------------------------------------- */

static void test_watch_full_attack_camped(void)
{
    banner("watch: the real thing, camped - three families and an alarm");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x0F, -70,
                    PHAROS_RSN_F_PRESENT | PHAROS_RSN_F_MFP_CAPABLE |
                        PHAROS_RSN_F_MFP_REQUIRED, 2);
    beacons(&e, &ap, 30, T0);

    const uint8_t *clients[3] = { CLIENT1, CLIENT2, CLIENT3 };
    for (int i = 0; i < 400; i++) {
        const uint64_t t = T0 + 3000000ull + (uint64_t)i * 10000ull;
        /* Tight bursts per victim, broadcast every fifth, one reason code,
         * frozen-ish counter, arriving 30 dB louder than the AP it claims. */
        spoofed_deauth(&e, &ap, (i % 5 == 0) ? BCAST : clients[(i / 8) % 3], t,
                       7, (uint16_t)(500 + (i / 64)), -40);
    }
    for (int i = 0; i < 15; i++) {
        rejoin(&e, &ap, clients[i % 3], T0 + 7300000ull + (uint64_t)i * 80000ull);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    unsigned fams = 0;
    for (unsigned b = 0; b < 4; b++) if (v.families & (1u << b)) fams++;
    CHECK(fams >= 3, "at least three families must agree (got %u, mask 0x%02x)",
          fams, v.families);
    CHECK(v.families & PW_FAM_FORGERY, "forgery is among them");
    CHECK_EQ(v.band, PW_BAND_LIKELY);
    CHECK(v.score >= 75, "score reaches the alarm band (got %u)", v.score);
    CHECK(v.score <= v.ceiling, "and never exceeds its own ceiling");
    CHECK(v.channel == 6, "the channel to camp on is reported (got %u)", v.channel);
    CHECK(strcmp(v.ssid, "testnet") == 0, "the network under pressure is named");
    CHECK(pw_forgery_name(v.forgery) != NULL, "the operator is told why");
}

/* ---- housekeeping ----------------------------------------------------- */

static void test_watch_short_window(void)
{
    banner("watch: a blink is not a rate");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x10, -50, PHAROS_RSN_F_PRESENT, 2);
    for (int i = 0; i < 100; i++) {
        spoofed_deauth(&e, &ap, BCAST, T0 + (uint64_t)i * 5000ull, 7, 0, -60);
    }
    pw_evaluate(&e, T0 + 500000ull, &c, &v);

    CHECK(v.notes & PW_NOTE_SHORT_WINDOW, "short window disclosed");
    CHECK(v.score <= PW_CAP_SHORT_WINDOW,
          "cannot alarm on half a second (got %u)", v.score);
}

static void test_watch_pressure_channel(void)
{
    banner("watch: where to go and stand");
    pw_engine_t e;
    pw_reset(&e);

    ap_t ap = mk_ap(0x11, -50, 0, 2);
    for (int i = 0; i < 30; i++) {
        pharos_ev_dot11_t d = frame(PHAROS_ST_DEAUTH, BCAST, ap.bssid, -60, 7,
                                    (uint16_t)i, 0, 0);
        d.channel = (i % 4 == 0) ? 1 : 11;
        pw_observe(&e, &d, T0 + (uint64_t)i * 100000ull);
    }
    CHECK_EQ(pw_pressure_channel(&e, T0 + 3000000ull, 10000), 11);

    pw_reset(&e);
    CHECK_EQ(pw_pressure_channel(&e, T0, 10000), 0);
}

static void test_watch_vocabulary(void)
{
    banner("watch: the vocabulary contains no all-clear");
    for (int b = PW_BAND_QUIET; b <= PW_BAND_LIKELY; b++) {
        const char *name = pw_band_name((pw_band_t)b);
        const char *advice = pw_band_advice((pw_band_t)b);
        CHECK(name && *name, "band %d named", b);
        CHECK(advice && *advice, "band %d advised", b);
        CHECK(strstr(name, "SAFE") == NULL, "no band claims safety: %s", name);
        CHECK(strstr(name, "CLEAR") == NULL, "no band claims all-clear: %s", name);
        CHECK(strstr(advice, " is safe") == NULL, "advice never says safe");
        CHECK(strstr(advice, "no attack") == NULL, "advice never rules out attack");
    }
    /* Every forgery test must be able to explain itself in words. */
    const uint8_t all[] = { PW_FORGE_MFP_PROOF, PW_FORGE_MFP_HINT,
                            PW_FORGE_SEQ_ORDER, PW_FORGE_SEQ_FROZEN,
                            PW_FORGE_RSSI_SPLIT, PW_FORGE_GHOST };
    for (unsigned i = 0; i < sizeof(all); i++) {
        CHECK(pw_forgery_name(all[i]) != NULL, "forgery bit %u names itself", i);
    }
    CHECK(pw_forgery_name(0) == NULL, "no forgery, no claim");
    CHECK(pw_reason_name(7) != NULL, "reason codes are put into words");
}

/* Never, in any configuration, may a verdict exceed its own stated ceiling. */
static void test_watch_ceiling_is_never_exceeded(void)
{
    banner("watch: no verdict ever exceeds its own ceiling");
    for (uint16_t dwell = 20; dwell <= 1000; dwell += 61) {
        for (uint16_t yield = 200; yield <= 1000; yield += 133) {
            pw_engine_t e;
            pw_verdict_t v;
            pw_context_t c = ctx_of(dwell, yield);
            pw_reset(&e);
            ap_t ap = mk_ap(0x12, -70,
                            PHAROS_RSN_F_PRESENT | PHAROS_RSN_F_MFP_REQUIRED, 2);
            beacons(&e, &ap, 40, T0);
            for (int i = 0; i < 300; i++) {
                spoofed_deauth(&e, &ap, BCAST,
                               T0 + 3000000ull + (uint64_t)i * 15000ull, 7,
                               (uint16_t)(77 + i / 50), -30);
            }
            for (int i = 0; i < 20; i++) {
                rejoin(&e, &ap, CLIENT1, T0 + 8000000ull + (uint64_t)i * 50000ull);
            }
            pw_evaluate(&e, T0 + 9900000ull, &c, &v);
            CHECK(v.score <= v.ceiling, "score %u > ceiling %u at dwell=%u yield=%u",
                  v.score, v.ceiling, dwell, yield);
            CHECK(v.ceiling <= PW_CEILING_MAX, "ceiling stays under the cap");
            CHECK(v.score <= 100, "score in range");
        }
    }
}


/* ---- the two faults real air found ----------------------------------- */

/* A retransmitted disconnect carries the SAME sequence number, by design: the
 * retry bit is set precisely so the receiver can discard the duplicate. An
 * access point retrying one frame therefore looked exactly like a tool
 * blasting a hand-built one, and the first ambient capture this engine ever
 * saw graded an empty room SUSPICIOUS with SEQ_FROZEN lit. */
static void test_watch_retries_are_not_a_frozen_counter(void)
{
    banner("watch: retransmissions are not a frozen sequence counter");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    ap_t ap = mk_ap(0x30, -50, PHAROS_RSN_F_PRESENT, 2);
    beacons(&e, &ap, 40, T0);

    /* One genuine disconnect, retried fifteen times on the same counter. */
    ap.seq = (uint16_t)((ap.seq + 1) & 0x0FFF);
    for (int i = 0; i < 16; i++) {
        pharos_ev_dot11_t d = frame(PHAROS_ST_DEAUTH, CLIENT1, ap.bssid, ap.rssi,
                                    4, ap.seq, i ? PHAROS_DOT11_F_RETRY : 0, 0);
        pw_observe(&e, &d, T0 + 4000000ull + (uint64_t)i * 3000ull);
    }
    pw_evaluate(&e, T0 + 9900000ull, &c, &v);

    CHECK((v.forgery & PW_FORGE_SEQ_FROZEN) == 0,
          "a retried frame is not a forgery (forgery=0x%02x)", v.forgery);
}

/* The duty correction divides by the share of the channel that was heard, and
 * as that share approaches zero the quotient approaches nonsense. On hardware
 * this read `est=33600.00/s dwell=0%` moments after the lens stopped camping,
 * and the RATE family fired on an empty room. Below PW_MIN_CHANNEL_MS of
 * observed channel time there is no denominator worth dividing by. */
static void test_watch_no_rate_without_channel_time(void)
{
    banner("watch: no channel time, no rate");
    pw_engine_t e;
    pw_verdict_t v;

    pw_reset(&e);
    ap_t ap = mk_ap(0x31, -50, PHAROS_RSN_F_PRESENT, 2);
    beacons(&e, &ap, 30, T0);
    /* Spread thinly enough that no single second holds two, so the peak-second
     * path - which is a direct measurement and rightly survives - contributes
     * nothing and the extrapolation is the only thing under test. */
    for (int i = 0; i < 5; i++) {
        genuine_deauth(&e, &ap, CLIENT1, T0 + 3000000ull + (uint64_t)i * 1400000ull, 7);
    }

    /* A receiver that has barely visited this channel: 3 per mille of a ten
     * second window is 30 ms of channel time, well under the floor. */
    pw_context_t sliver = ctx_of(3, 1000);
    pw_evaluate(&e, T0 + 9900000ull, &sliver, &v);
    CHECK(v.notes & PW_NOTE_NO_RATE, "the engine declines to quote a rate");
    CHECK_EQ(v.est_per_s_x100, 0);
    CHECK_EQ(v.c_rate, 0);
    CHECK((v.families & PW_FAM_RATE) == 0,
          "and the rate family does not fire on a sliver");

    /* Camped on the same evidence, the rate is quotable again. */
    pw_context_t camped = ctx_of(1000, 1000);
    pw_evaluate(&e, T0 + 9900000ull, &camped, &v);
    CHECK((v.notes & PW_NOTE_NO_RATE) == 0, "camped, the rate stands");
    CHECK(v.est_per_s_x100 > 0, "and it is a real number (%u)", v.est_per_s_x100);
}

/* The peak second is a COUNT, not an extrapolation - it is what this receiver
 * actually heard inside one wall-clock second - so a thin dwell does not
 * invalidate it and it must keep contributing even when the extrapolated rate
 * is refused. Suppressing both would throw away the one rate signal that
 * hopping cannot distort. */
static void test_watch_peak_second_survives_thin_dwell(void)
{
    banner("watch: the peak second is a measurement and survives a thin dwell");
    pw_engine_t e;
    pw_verdict_t v;

    pw_reset(&e);
    ap_t ap = mk_ap(0x32, -50, PHAROS_RSN_F_PRESENT, 2);
    beacons(&e, &ap, 30, T0);
    /* Twenty in one second: heard, counted, not inferred. */
    for (int i = 0; i < 20; i++) {
        genuine_deauth(&e, &ap, CLIENT1, T0 + 4000000ull + (uint64_t)i * 40000ull, 7);
    }
    pw_context_t sliver = ctx_of(3, 1000);
    pw_evaluate(&e, T0 + 9900000ull, &sliver, &v);

    CHECK(v.notes & PW_NOTE_NO_RATE, "the extrapolation is still refused");
    CHECK_EQ(v.est_per_s_x100, 0);
    CHECK(v.peak_second >= 15, "the peak second was counted (%u)", v.peak_second);
    CHECK(v.c_rate > 0, "and it still scores on what was actually heard");
}


/* The round screen gives about 318 px on the hint line, which is roughly forty
 * characters at the size it is drawn. A string that does not fit is not a
 * softer warning, it is a clipped one - the device showed
 * "...connect traffic in view. This receiver hears one channel at..." to a
 * real operator. So the limit is a test, not a guideline. */
static void test_watch_hints_fit_the_glass(void)
{
    banner("watch: every on-screen hint fits on one line");
    for (int b = PW_BAND_QUIET; b <= PW_BAND_LIKELY; b++) {
        const char *h = pw_band_hint((pw_band_t)b);
        CHECK(h && *h, "band %d has a hint", b);
        CHECK(strlen(h) <= PW_HINT_MAX_CHARS,
              "hint for %s is %u chars, limit %u: \"%s\"",
              pw_band_name((pw_band_t)b), (unsigned)strlen(h),
              (unsigned)PW_HINT_MAX_CHARS, h);
        /* The long form is still allowed to be a full sentence - it goes to
         * reports and the info card, where there is room. */
        CHECK(pw_band_advice((pw_band_t)b) != NULL, "and a long form exists");
    }
    /* The forgery findings share that line, so they answer to it too. */
    const uint8_t all[] = { PW_FORGE_MFP_PROOF, PW_FORGE_MFP_HINT,
                            PW_FORGE_SEQ_ORDER, PW_FORGE_SEQ_FROZEN,
                            PW_FORGE_RSSI_SPLIT, PW_FORGE_GHOST };
    for (unsigned i = 0; i < sizeof(all); i++) {
        const char *n = pw_forgery_name(all[i]);
        CHECK(n && strlen(n) <= PW_HINT_MAX_CHARS,
              "forgery name %u is %u chars: \"%s\"", i,
              (unsigned)(n ? strlen(n) : 0), n ? n : "(null)");
    }
}

void test_watch(void)
{
    test_watch_ceiling();
    test_watch_quiet();
    test_watch_volume_alone_never_alarms();
    test_watch_no_corroboration_invariant();
    test_watch_one_and_two_family_caps();
    test_watch_mfp_proof();
    test_watch_hard_evidence_survives_hopping();
    test_watch_mfp_capable_is_only_a_hint();
    test_watch_seq_order_violation();
    test_watch_seq_gaps_are_not_violations();
    test_watch_seq_frozen();
    test_watch_advancing_counter_is_not_frozen();
    test_watch_rssi_split();
    test_watch_fading_ap_is_not_a_forgery();
    test_watch_aftermath_stampede();
    test_watch_ordinary_associations_are_not_aftermath();
    test_watch_full_attack_camped();
    test_watch_short_window();
    test_watch_pressure_channel();
    test_watch_vocabulary();
    test_watch_hints_fit_the_glass();
    test_watch_retries_are_not_a_frozen_counter();
    test_watch_no_rate_without_channel_time();
    test_watch_peak_second_survives_thin_dwell();
    test_watch_ceiling_is_never_exceeded();
}
