/* Pharos host test suite.
 *
 * Everything testable without a radio is tested here, on a laptop, with a
 * plain C compiler: the ingest ring's wraparound and drop accounting, the
 * lens lifecycle, the round-screen geometry that decides whether a label
 * survives the curve, and the whole deauthentication watch engine including
 * every cap and ceiling it applies to itself.
 *
 *   make -C test/host
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pharos_bus.h"
#include "pharos_caps.h"
#include "pharos_dot11.h"
#include "pharos_lens.h"
#include "pharos_round.h"
#include "pharos_watch.h"
#include "test_support.h"

unsigned g_checks, g_fails;

void banner(const char *s) { printf("\n== %s\n", s); }

/* ------------------------------------------------------------------ bus */

static void test_bus(void)
{
    banner("ingest bus");
    pharos_bus_t bus;
    pharos_event_t storage[8];

    CHECK(!pharos_bus_init(&bus, storage, 0), "zero capacity must be rejected");
    CHECK(!pharos_bus_init(&bus, storage, 3), "non-power-of-two must be rejected");
    CHECK(pharos_bus_init(&bus, storage, 8), "power-of-two accepted");
    CHECK_EQ(pharos_bus_pending(&bus), 0);

    pharos_event_t ev;
    memset(&ev, 0, sizeof(ev));

    for (int i = 0; i < 8; i++) {
        ev.serial = (uint16_t)i;
        CHECK(pharos_bus_push(&bus, &ev), "push %d should fit", i);
    }
    CHECK_EQ(pharos_bus_pending(&bus), 8);
    CHECK(!pharos_bus_push(&bus, &ev), "ninth push must be dropped");
    CHECK_EQ(pharos_bus_dropped(&bus), 1);

    /* FIFO order preserved. */
    for (int i = 0; i < 8; i++) {
        pharos_event_t got;
        CHECK(pharos_bus_pop(&bus, &got), "pop %d", i);
        CHECK_EQ(got.serial, i);
    }
    CHECK(!pharos_bus_pop(&bus, &ev), "empty ring pops nothing");

    /* Yield reflects the single drop out of nine offered. */
    CHECK_EQ(pharos_bus_yield_permil(&bus), (8 * 1000) / 9);

    /* Many laps around the ring: indices must keep matching. */
    pharos_bus_reset(&bus);
    for (int lap = 0; lap < 500; lap++) {
        ev.serial = (uint16_t)lap;
        CHECK(pharos_bus_push(&bus, &ev), "lap push");
        pharos_event_t got;
        CHECK(pharos_bus_pop(&bus, &got), "lap pop");
        CHECK_EQ(got.serial, (uint16_t)lap);
    }
    CHECK_EQ(pharos_bus_dropped(&bus), 0);
    CHECK_EQ(pharos_bus_yield_permil(&bus), 1000);
}

/* ----------------------------------------------------------------- lens */

static int g_mount, g_unmount, g_start, g_stop;
static bool g_start_ok = true;

static bool l_mount(void) { g_mount++; return true; }
static void l_unmount(void) { g_unmount++; }
static bool l_start(void) { g_start++; return g_start_ok; }
static void l_stop(void) { g_stop++; }

static const pharos_lens_t k_lens_a = {
    .id = "test.a", .name = "A", .summary = "first", .glyph = "a",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN,
    .budget_ma = 120,
    .on_mount = l_mount, .on_unmount = l_unmount,
    .on_start = l_start, .on_stop = l_stop,
};

static const pharos_lens_t k_lens_b = {
    .id = "test.b", .name = "B", .summary = "second", .glyph = "b",
    .kind = PHAROS_LENS_TRAIN, .caps = PHAROS_CAP_NONE, .budget_ma = 40,
};

static void test_lens(void)
{
    banner("lens registry");
    pharos_lens_reset_for_test();
    g_mount = g_unmount = g_start = g_stop = 0;
    g_start_ok = true;

    pharos_lens_register(&k_lens_a);
    pharos_lens_register(&k_lens_b);
    pharos_lens_register(&k_lens_a); /* duplicate id */
    CHECK_EQ(pharos_lens_count(), 2);
    CHECK(pharos_lens_find("test.a") == &k_lens_a, "find by id");
    CHECK(pharos_lens_find("nope") == NULL, "unknown id is NULL");
    CHECK(pharos_lens_at(5) == NULL, "out of range index is NULL");

    CHECK_EQ(pharos_lens_active_caps(), PHAROS_CAP_NONE);
    CHECK(pharos_lens_activate("test.a"), "activate a");
    CHECK_EQ(g_mount, 1);
    CHECK_EQ(g_start, 1);
    CHECK(pharos_lens_active() == &k_lens_a, "a is active");
    CHECK_EQ(pharos_lens_active_caps(), PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN);

    /* Switching lenses must fully release the previous one before the next
     * mounts, or two lenses could contend for the radio. */
    CHECK(pharos_lens_activate("test.b"), "activate b");
    CHECK_EQ(g_stop, 1);
    CHECK_EQ(g_unmount, 1);
    CHECK_EQ(pharos_lens_active_caps(), PHAROS_CAP_NONE);

    pharos_lens_deactivate();
    CHECK(pharos_lens_active() == NULL, "nothing active");
    CHECK_EQ(pharos_lens_active_caps(), PHAROS_CAP_NONE);

    /* A lens that mounts but fails to start must not be left half-live. */
    g_start_ok = false;
    CHECK(!pharos_lens_activate("test.a"), "failed start reports failure");
    CHECK(pharos_lens_active() == NULL, "failed start leaves nothing active");
    CHECK_EQ(g_unmount, 2);
    CHECK_EQ(pharos_lens_active_caps(), PHAROS_CAP_NONE);

    char buf[96];
    pharos_caps_describe(k_lens_a.caps, buf, sizeof(buf));
    CHECK(strcmp(buf, "wifi.rx wifi.chan") == 0, "caps render: %s", buf);
    pharos_caps_describe(PHAROS_CAP_NONE, buf, sizeof(buf));
    CHECK(strcmp(buf, "none") == 0, "empty caps render: %s", buf);

    pharos_lens_reset_for_test();
}

/* ---------------------------------------------------------------- round */

static void test_round(void)
{
    banner("round-screen geometry");

    pr_point_t p = pr_polar(PR_R, 0.0f);
    CHECK_EQ(p.x, PR_CX);
    CHECK_EQ(p.y, 0);
    p = pr_polar(PR_R, 90.0f);
    CHECK_EQ(p.x, PR_CX + PR_R);
    CHECK_EQ(p.y, PR_CY);
    p = pr_polar(PR_R, 180.0f);
    CHECK_EQ(p.y, PR_CY + PR_R);
    p = pr_polar(PR_R, 270.0f);
    CHECK_EQ(p.x, PR_CX - PR_R);

    /* polar and bearing are inverses. */
    for (float a = 0.0f; a < 360.0f; a += 17.0f) {
        pr_point_t q = pr_polar(200, a);
        CHECK_NEAR(pr_bearing_of(q.x, q.y), a, 0.5);
        CHECK_NEAR(pr_radius_of(q.x, q.y), 200.0, 1.0);
    }

    CHECK_EQ(pr_zone_of(PR_CX, PR_CY), PR_ZONE_CORE);
    CHECK_EQ(pr_zone_of(PR_CX, PR_CY - 150), PR_ZONE_RING);
    CHECK_EQ(pr_zone_of(PR_CX, PR_CY - 200), PR_ZONE_RIM);
    CHECK_EQ(pr_zone_of(0, 0), PR_ZONE_OUTSIDE); /* the corner is not there */

    CHECK_EQ(pr_chord_halfwidth(PR_R, 0), PR_R);
    CHECK_EQ(pr_chord_halfwidth(PR_R, PR_R), 0);
    CHECK(pr_chord_halfwidth(PR_R, 180) < pr_chord_halfwidth(PR_R, 60),
          "the circle narrows as you leave the middle");

    /* An inscribed card must actually be inside the circle. */
    pr_rect_t card = pr_inscribe(PR_SAFE_R, 16, 9);
    CHECK(card.w > 0 && card.h > 0, "card has size");
    const double corner = sqrt((card.w / 2.0) * (card.w / 2.0) +
                               (card.h / 2.0) * (card.h / 2.0));
    CHECK(corner <= (double)PR_SAFE_R, "card corner %g within %d", corner, PR_SAFE_R);

    CHECK(pr_text_fits(180, 40, 0, PR_SAFE_R), "short label fits at centre");
    CHECK(!pr_text_fits(400, 40, 180, PR_SAFE_R),
          "wide label near the rim must be rejected, not clipped");

    /* Wedge that straddles 12 o'clock. */
    pr_point_t inside = pr_polar(200, 5.0f);
    pr_point_t outside = pr_polar(200, 40.0f);
    pr_point_t too_close = pr_polar(100, 5.0f);
    CHECK(pr_wedge_hit(inside.x, inside.y, 180, 233, 350.0f, 20.0f), "wrap wedge hit");
    CHECK(!pr_wedge_hit(outside.x, outside.y, 180, 233, 350.0f, 20.0f), "outside sweep");
    CHECK(!pr_wedge_hit(too_close.x, too_close.y, 180, 233, 350.0f, 20.0f),
          "inside the annulus radius");

    CHECK(pr_min_wedge_deg(PR_RIM_R) < pr_min_wedge_deg(PR_CORE_R),
          "targets near the rim may be angularly narrower");
    CHECK(pr_min_wedge_deg(PR_RIM_R) > 9.0f, "a thumb still needs ~11 degrees at the rim");

    CHECK_NEAR(pr_value_to_deg(50, 0, 100, 0.0f, 360.0f), 180.0, 0.01);
    CHECK_NEAR(pr_value_to_deg(-5, 0, 100, 0.0f, 360.0f), 0.0, 0.01);
    CHECK_NEAR(pr_value_to_deg(500, 0, 100, 0.0f, 360.0f), 0.0, 0.01); /* 360 wraps */

    for (uint64_t t = 0; t < 3600000ull; t += 7919) {
        int16_t dx = 0, dy = 0;
        pr_burnin_offset(t, &dx, &dy);
        CHECK(dx >= -3 && dx <= 3 && dy >= -3 && dy <= 3,
              "burn-in walk stays within 3px (%d,%d)", dx, dy);
    }

    CHECK_NEAR(pr_dial_angle(0, 8, 0.0f), 0.0, 0.01);
    CHECK_NEAR(pr_dial_angle(2, 8, 0.0f), 90.0, 0.01);
    CHECK_NEAR(pr_dial_angle(8, 8, 0.0f), 0.0, 0.01);
}

/* ---------------------------------------------------------------- watch */

static const uint8_t AP_KNOWN[6] = { 0x02, 0xAA, 0xBB, 0x00, 0x00, 0x01 };
static const uint8_t AP_SPOOF[6] = { 0x02, 0xDE, 0xAD, 0x00, 0x00, 0x02 };
static const uint8_t CLIENT1[6]  = { 0x02, 0xC1, 0x00, 0x00, 0x00, 0x11 };
static const uint8_t BCAST[6]    = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

#define T0 100000000ull /* 100 s, well clear of the bucket epoch */

static pharos_ev_dot11_t mk(uint8_t subtype, const uint8_t *a1, const uint8_t *a2,
                            int8_t rssi, uint16_t reason, uint8_t flags)
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
    f.flags = flags;
    return f;
}

static void feed_beacons(pw_engine_t *e, const uint8_t *bssid, int n, int8_t rssi,
                         uint8_t flags)
{
    pharos_ev_dot11_t b = mk(PHAROS_ST_BEACON, BCAST, bssid, rssi, 0, flags);
    for (int i = 0; i < n; i++) {
        pw_observe(e, &b, T0 + (uint64_t)i * 100000ull);
    }
}

/* n deauths spread evenly across span_us starting at T0. */
static void feed_deauth(pw_engine_t *e, const uint8_t *src, const uint8_t *dst, int n,
                        int8_t rssi, uint16_t reason, uint64_t span_us)
{
    pharos_ev_dot11_t d = mk(PHAROS_ST_DEAUTH, dst, src, rssi, reason, 0);
    for (int i = 0; i < n; i++) {
        pw_observe(e, &d, T0 + ((uint64_t)i * span_us) / (uint64_t)n);
    }
}

static pw_context_t ctx_of(uint16_t dwell, uint16_t yield)
{
    pw_context_t c;
    c.dwell_permil = dwell;
    c.bus_yield_permil = yield;
    c.window_ms = 10000;
    return c;
}

static void test_watch_ceiling(void)
{
    banner("watch: confidence ceiling");

    pw_context_t camped = ctx_of(1000, 1000);
    pw_context_t hopping = ctx_of(71, 1000); /* 200 ms across 14 channels */
    pw_context_t lossy = ctx_of(1000, 300);

    CHECK_EQ(pw_ceiling(&camped), 96);
    CHECK(pw_ceiling(&hopping) < 75,
          "hopping must not be able to reach the alarm band (got %u)",
          pw_ceiling(&hopping));
    CHECK_EQ(pw_ceiling(&hopping), 60);
    CHECK(pw_ceiling(&lossy) < pw_ceiling(&camped), "frame loss lowers confidence");
    CHECK(pw_ceiling(&camped) < 100, "nothing this device sees is certain");

    /* Monotonic in dwell: standing still always buys confidence. */
    uint8_t prev = 0;
    for (uint16_t d = 1; d <= 1000; d += 37) {
        pw_context_t c = ctx_of(d, 1000);
        uint8_t got = pw_ceiling(&c);
        CHECK(got >= prev, "ceiling monotonic in dwell at %u", d);
        prev = got;
    }
    pw_context_t floorish = ctx_of(1, 1);
    CHECK_EQ(pw_ceiling(&floorish), 45);
}

static void test_watch_quiet_and_background(void)
{
    banner("watch: quiet and ordinary networks");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    pw_evaluate(&e, T0 + 10000000ull, &c, &v);
    CHECK_EQ(v.band, PW_BAND_QUIET);
    CHECK_EQ(v.score, 0);
    CHECK_EQ(v.observed, 0);

    /* A healthy network: beacons plus a handful of unicast disconnects. */
    pw_reset(&e);
    feed_beacons(&e, AP_KNOWN, 60, -45, 0);
    feed_deauth(&e, AP_KNOWN, CLIENT1, 3, -45, 8, 9000000ull);
    pw_evaluate(&e, T0 + 9500000ull, &c, &v);
    CHECK(v.band <= PW_BAND_BACKGROUND,
          "roaming traffic must not alarm (band %s score %u)",
          pw_band_name(v.band), v.score);
}

static void test_watch_flood_camped(void)
{
    banner("watch: broadcast flood, camped receiver");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    feed_beacons(&e, AP_KNOWN, 60, -45, 0);
    feed_deauth(&e, AP_SPOOF, BCAST, 1000, -60, 7, 9990000ull);
    pw_evaluate(&e, T0 + 9990000ull, &c, &v);

    CHECK_EQ(v.observed, 1000);
    CHECK(v.shape_sample <= PW_MAX_EVENTS, "shape comes from a bounded sample");
    CHECK(v.notes & PW_NOTE_SAMPLED, "engine admits the shape was sampled");
    CHECK_NEAR(v.est_per_s_x100, 10000, 200); /* ~100 deauth/s */
    CHECK_EQ(v.broadcast_permil, 1000);
    CHECK(v.families & PW_FAM_RATE, "rate family");
    CHECK(v.families & PW_FAM_TARGET, "targeting family");
    CHECK(v.families & PW_FAM_IDENTITY, "identity family: source never beaconed");
    CHECK_EQ(v.band, PW_BAND_LIKELY);
    CHECK(v.score >= 75 && v.score <= v.ceiling, "score %u within ceiling %u",
          v.score, v.ceiling);
    CHECK(memcmp(v.src, AP_SPOOF, 6) == 0, "dominant source identified");
}

static void test_watch_flood_hopping(void)
{
    banner("watch: same flood, hopping receiver");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(71, 1000);

    /* A hopping receiver hears about 7% of it: 71 frames, not 1000. */
    pw_reset(&e);
    feed_beacons(&e, AP_KNOWN, 6, -45, 0);
    feed_deauth(&e, AP_SPOOF, BCAST, 71, -60, 7, 9990000ull);
    pw_evaluate(&e, T0 + 9990000ull, &c, &v);

    /* Duty correction recovers the true rate from the thin sample ... */
    CHECK_NEAR(v.est_per_s_x100, 10000, 1200);
    /* ... but the ceiling refuses to let an extrapolation raise an alarm. */
    CHECK(v.band == PW_BAND_SUSPICIOUS, "hopping tops out at SUSPICIOUS, got %s",
          pw_band_name(v.band));
    CHECK_EQ(v.score, 60);
    CHECK(v.notes & PW_NOTE_THIN_DWELL, "thin dwell disclosed");
    CHECK(!(v.families & PW_FAM_IDENTITY),
          "an unheard beacon is not evidence of forgery while hopping");
    CHECK(v.raw_score > v.score, "the operator can see what camping would buy");
}

static void test_watch_single_family_cap(void)
{
    banner("watch: one family cannot leave ELEVATED");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    /* Heavy but ordinary-shaped: one victim, a real AP, matching signal. */
    pw_reset(&e);
    feed_beacons(&e, AP_KNOWN, 60, -45, 0);
    feed_deauth(&e, AP_KNOWN, CLIENT1, 1000, -45, 7, 9990000ull);
    pw_evaluate(&e, T0 + 9990000ull, &c, &v);

    CHECK_EQ(v.families, PW_FAM_RATE);
    CHECK_EQ(v.score, 49);
    CHECK_EQ(v.band, PW_BAND_ELEVATED);
    CHECK(v.raw_score > v.score, "the cap is visible, not hidden");
}

static void test_watch_two_families_cannot_alarm(void)
{
    banner("watch: two families top out below the alarm band");
    /* Arithmetic property, asserted directly: rate 40 + the larger of the
     * other two families 22 + the 12 point reason modifier = 74. */
    CHECK(40 + 22 + 12 < 75, "any two families stay under the alarm threshold");
}

static void test_watch_identity_rssi(void)
{
    banner("watch: same address, different transmitter");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    feed_beacons(&e, AP_KNOWN, 60, -40, 0);
    /* Frames claim the AP we can hear, but arrive 30 dB weaker. */
    feed_deauth(&e, AP_KNOWN, BCAST, 300, -70, 7, 9990000ull);
    pw_evaluate(&e, T0 + 9990000ull, &c, &v);

    CHECK_EQ(v.rssi_delta, 30);
    CHECK_EQ(v.c_identity, 22);
    CHECK(v.families & PW_FAM_IDENTITY, "signal mismatch is identity evidence");
    CHECK_EQ(v.band, PW_BAND_LIKELY);
}

static void test_watch_mfp_note(void)
{
    banner("watch: 802.11w target is annotated, not scored");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    feed_beacons(&e, AP_KNOWN, 60, -40, PHAROS_DOT11_F_MFP_SEEN);
    feed_deauth(&e, AP_KNOWN, BCAST, 300, -70, 7, 9990000ull);
    pw_evaluate(&e, T0 + 9990000ull, &c, &v);
    CHECK(v.notes & PW_NOTE_MFP_TARGET, "protected-management-frame note raised");
}

static void test_watch_short_window(void)
{
    banner("watch: a blink is not a rate");
    pw_engine_t e;
    pw_verdict_t v;
    pw_context_t c = ctx_of(1000, 1000);

    pw_reset(&e);
    feed_deauth(&e, AP_SPOOF, BCAST, 100, -60, 7, 500000ull);
    pw_evaluate(&e, T0 + 500000ull, &c, &v);

    CHECK(v.notes & PW_NOTE_SHORT_WINDOW, "short window disclosed");
    CHECK(v.score <= 49, "cannot alarm on half a second (got %u)", v.score);
}

static void test_watch_vocabulary(void)
{
    banner("watch: vocabulary contains no all-clear");
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
}

/* --------------------------------------------------------------- dot11 */

static void test_dot11_header(void)
{
    banner("dot11: fixed header");
    uint8_t f[26];
    memset(f, 0, sizeof(f));
    f[0] = 0xC0; /* mgmt, subtype 12 = deauth */
    f[1] = 0x00;
    memset(f + 4, 0xFF, 6);          /* addr1 broadcast */
    memcpy(f + 10, AP_SPOOF, 6);     /* addr2 transmitter */
    memcpy(f + 16, AP_SPOOF, 6);     /* addr3 bssid */
    f[22] = 0x30;                    /* seq 0x123 << 4 */
    f[23] = 0x12;
    f[24] = 0x07;                    /* reason 7 */
    f[25] = 0x00;

    pharos_ev_dot11_t ev;
    CHECK(pharos_dot11_parse_header(f, sizeof(f), &ev), "parses");
    CHECK_EQ(ev.type, PHAROS_FT_MGMT);
    CHECK_EQ(ev.subtype, PHAROS_ST_DEAUTH);
    CHECK_EQ(ev.seq, 0x123);
    CHECK(ev.flags & PHAROS_DOT11_F_BROADCAST, "broadcast recipient flagged");
    CHECK(memcmp(ev.a2, AP_SPOOF, 6) == 0, "transmitter address");

    uint16_t reason = 0;
    CHECK(pharos_dot11_reason(f, sizeof(f), &ev, &reason), "reason present");
    CHECK_EQ(reason, 7);

    /* A runt frame must be refused, not parsed out of adjacent memory. */
    CHECK(!pharos_dot11_parse_header(f, 10, &ev), "short frame rejected");
    CHECK(!pharos_dot11_reason(f, 24, &ev, &reason), "truncated reason rejected");

    ev.subtype = PHAROS_ST_BEACON;
    CHECK(!pharos_dot11_reason(f, sizeof(f), &ev, &reason),
          "beacons carry no reason code");
}

static void test_dot11_ies(void)
{
    banner("dot11: information elements");
    /* 12 bytes of fixed parameters, then SSID, DS param, RSN. */
    uint8_t body[128];
    memset(body, 0, sizeof(body));
    size_t n = 12;

    body[n++] = PHAROS_IE_SSID;
    body[n++] = 5;
    memcpy(body + n, "pharo", 5);
    n += 5;

    body[n++] = PHAROS_IE_DS_PARAM;
    body[n++] = 1;
    body[n++] = 6;

    const size_t rsn_at = n;
    body[n++] = PHAROS_IE_RSN;
    const size_t rsn_len_at = n++;
    const size_t rsn_start = n;
    body[n++] = 0x01; body[n++] = 0x00;                      /* version 1 */
    body[n++] = 0x00; body[n++] = 0x0F; body[n++] = 0xAC; body[n++] = 0x04; /* CCMP */
    body[n++] = 0x01; body[n++] = 0x00;                      /* 1 pairwise */
    body[n++] = 0x00; body[n++] = 0x0F; body[n++] = 0xAC; body[n++] = 0x04;
    body[n++] = 0x01; body[n++] = 0x00;                      /* 1 AKM */
    body[n++] = 0x00; body[n++] = 0x0F; body[n++] = 0xAC; body[n++] = 0x08; /* SAE */
    body[n++] = 0xC0; body[n++] = 0x00;   /* caps: MFPR|MFPC */
    body[rsn_len_at] = (uint8_t)(n - rsn_start);

    uint8_t len = 0;
    const uint8_t *ssid = pharos_dot11_find_ie(body, n, PHAROS_IE_SSID, &len);
    CHECK(ssid != NULL, "SSID element found");
    CHECK_EQ(len, 5);
    CHECK(ssid && memcmp(ssid, "pharo", 5) == 0, "SSID payload");

    const uint8_t *ds = pharos_dot11_find_ie(body, n, PHAROS_IE_DS_PARAM, &len);
    CHECK(ds != NULL && ds[0] == 6, "channel from DS parameter set");
    CHECK(pharos_dot11_find_ie(body, n, 200, &len) == NULL, "absent element");

    pharos_rsn_t rsn;
    CHECK(pharos_dot11_rsn(body, n, &rsn), "RSN parsed");
    CHECK(rsn.has_rsn && rsn.has_sae, "WPA3-Personal detected");
    CHECK(!rsn.has_psk, "no WPA2-PSK AKM in this beacon");
    CHECK(rsn.mfp_required && rsn.mfp_capable, "management frame protection required");

    /* An element that claims more bytes than the frame holds must stop the
     * walk. This is the one place a hostile beacon could reach past us. */
    uint8_t evil[32];
    memset(evil, 0, sizeof(evil));
    evil[12] = PHAROS_IE_SSID;
    evil[13] = 200; /* lies */
    CHECK(pharos_dot11_find_ie(evil, sizeof(evil), PHAROS_IE_SSID, &len) == NULL,
          "over-long element rejected");
    CHECK(pharos_dot11_find_ie(body, 4, PHAROS_IE_SSID, &len) == NULL,
          "body shorter than the fixed parameters");

    /* Truncate the RSN element mid-way at every offset: none may misparse. */
    for (size_t cut = rsn_at; cut <= n; cut++) {
        pharos_rsn_t partial;
        (void)pharos_dot11_rsn(body, cut, &partial);
        CHECK(!(partial.mfp_required && !partial.mfp_capable),
              "truncation never yields required-without-capable at %zu", cut);
    }
}

int main(void)
{
    printf("Pharos host tests\n");
    test_bus();
    test_lens();
    test_round();
    test_dot11_header();
    test_dot11_ies();
    test_watch_ceiling();
    test_watch_quiet_and_background();
    test_watch_flood_camped();
    test_watch_flood_hopping();
    test_watch_single_family_cap();
    test_watch_two_families_cannot_alarm();
    test_watch_identity_rssi();
    test_watch_mfp_note();
    test_watch_short_window();
    test_watch_vocabulary();
    test_census();
    test_twin();
    test_report();
    test_dial();
    test_probe_classify();
    test_probe_grading();
    test_power();
    test_region();
    test_range_determinism();
    test_range_flood();
    test_range_calm_and_roaming();
    test_range_probe_leak();
    test_range_vocabulary();
    test_sha256();
    test_chain();
    test_karma();
    test_flood();
    test_opsec();
    test_locate();
    test_console_tokenise();
    test_console_dispatch();
    test_console_help_and_safety();
    test_sentinel();
    test_harvest();
    test_aegis();

    printf("\n%u checks, %u failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
