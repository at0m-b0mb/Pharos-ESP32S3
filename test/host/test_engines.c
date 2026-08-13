/* Pharos host tests, part two: the grading engines, the evidence writer and
 * the round-screen widget geometry. */
#include <stdlib.h>

#include "pharos_census.h"
#include "pharos_dial.h"
#include "pharos_report.h"
#include "pharos_twin.h"
#include "test_support.h"

/* ---------------------------------------------------------------- census */

static pc_ap_t ap_base(const char *ssid, uint8_t chan, int8_t rssi)
{
    pc_ap_t ap;
    memset(&ap, 0, sizeof(ap));
    const uint8_t bssid[6] = { 0xAC, 0x11, 0x22, 0x33, 0x44, 0x55 };
    memcpy(ap.bssid, bssid, 6);
    ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(ap.ssid, ssid, ap.ssid_len);
    ap.channel = chan;
    ap.rssi = rssi;
    ap.beacons = 20;
    ap.beacon_ms = 100;
    return ap;
}

static pc_ap_t ap_wpa3(void)
{
    pc_ap_t ap = ap_base("office", 6, -50);
    ap.privacy = true;
    ap.ccmp_pairwise = true;
    ap.rsn.has_rsn = true;
    ap.rsn.has_sae = true;
    ap.rsn.mfp_required = true;
    ap.rsn.mfp_capable = true;
    return ap;
}

static pc_ap_t ap_wpa2(void)
{
    pc_ap_t ap = ap_base("office", 6, -50);
    ap.privacy = true;
    ap.ccmp_pairwise = true;
    ap.rsn.has_rsn = true;
    ap.rsn.has_psk = true;
    return ap;
}

void test_census(void)
{
    banner("census: posture grading");
    pc_verdict_t v;

    /* A glimpse is not an observation. */
    pc_ap_t thin = ap_wpa3();
    thin.beacons = 2;
    pc_grade(&thin, &v);
    CHECK_EQ(v.grade, PC_GRADE_UNGRADED);
    CHECK(v.notes & PC_NOTE_THIN, "thin observation disclosed");
    CHECK_EQ(v.score, 0);

    /* WPA3 with protected management frames is the ceiling of what this
     * device can observe going right. */
    pc_ap_t best = ap_wpa3();
    pc_grade(&best, &v);
    CHECK_EQ(v.score, 100);
    CHECK_EQ(v.grade, PC_GRADE_A_PLUS);
    CHECK_EQ(v.caps_applied, 0);

    /* Transition mode still accepts WPA2, so it is not an A. */
    pc_ap_t trans = ap_wpa3();
    trans.rsn.has_psk = true;
    trans.rsn.mfp_required = false;
    pc_grade(&trans, &v);
    CHECK(v.notes & PC_NOTE_TRANSITION, "transition mode noted");
    CHECK_EQ(v.grade, PC_GRADE_B);
    CHECK(v.score < 88, "transition mode cannot reach an A");

    /* The tie to the Watch engine: no 802.11w means the flood works. */
    pc_ap_t no_mfp = ap_wpa2();
    pc_grade(&no_mfp, &v);
    CHECK(v.caps_applied & PC_CAP_NO_MFP, "missing MFP is a ceiling, not a deduction");
    CHECK(strstr(pc_grade_advice(&v), "802.11w") != NULL, "advice names the fix");

    pc_ap_t mfp = ap_wpa2();
    mfp.rsn.mfp_required = true;
    mfp.rsn.mfp_capable = true;
    pc_grade(&mfp, &v);
    const uint8_t with_mfp = v.score;
    pc_grade(&no_mfp, &v);
    CHECK(with_mfp > v.score, "enabling MFP always improves the grade");

    /* Monotonicity: no single hardening step may lower a score. */
    pc_ap_t step = ap_wpa2();
    uint8_t prev = 0;
    pc_grade(&step, &v); prev = v.score;
    step.rsn.mfp_capable = true;
    pc_grade(&step, &v);
    CHECK(v.score >= prev, "mfp_capable does not lower the score"); prev = v.score;
    step.rsn.mfp_required = true;
    pc_grade(&step, &v);
    CHECK(v.score >= prev, "mfp_required does not lower the score");

    /* Weaknesses that are not gradual. */
    pc_ap_t open = ap_base("guest", 1, -40);
    pc_grade(&open, &v);
    CHECK_EQ(v.grade, PC_GRADE_F);
    CHECK(v.caps_applied & PC_CAP_OPEN, "open network capped, not scored down");
    CHECK(strstr(v.headline, "Open network") != NULL, "headline leads with the fact");

    pc_ap_t wep = ap_base("legacy", 1, -40);
    wep.privacy = true;
    pc_grade(&wep, &v);
    CHECK_EQ(v.grade, PC_GRADE_F);
    CHECK(v.caps_applied & PC_CAP_WEP, "WEP capped");

    pc_ap_t tkip = ap_wpa2();
    tkip.rsn.mfp_required = true;
    tkip.rsn.mfp_capable = true;
    tkip.tkip_pairwise = true;
    pc_grade(&tkip, &v);
    CHECK(v.caps_applied & PC_CAP_TKIP, "mixed-mode TKIP capped");
    CHECK(v.score <= 57, "TKIP cannot exceed 57, got %u", v.score);
    CHECK(v.raw_score > v.score, "the cap is visible in the breakdown");

    pc_ap_t wps = ap_wpa3();
    wps.wps_present = true;
    wps.wps_pin = true;
    pc_grade(&wps, &v);
    CHECK(v.caps_applied & PC_CAP_WPS_PIN, "WPS PIN capped");
    CHECK(v.score <= 67, "WPS PIN cannot exceed 67, got %u", v.score);

    pc_ap_t owe = ap_wpa3();
    owe.rsn.has_sae = false;
    owe.rsn.has_owe = true;
    pc_grade(&owe, &v);
    CHECK(v.notes & PC_NOTE_OWE, "OWE noted as unauthenticated");
    CHECK(v.grade <= PC_GRADE_C, "encryption without authentication is not a B");

    pc_ap_t ent = ap_wpa3();
    ent.rsn.has_sae = false;
    ent.akm_8021x = true;
    pc_grade(&ent, &v);
    CHECK(v.notes & PC_NOTE_ENTERPRISE, "enterprise noted");
    CHECK_EQ(v.grade, PC_GRADE_A);

    /* Hiding an SSID is a note, never a score. */
    pc_ap_t hidden = ap_wpa3();
    hidden.hidden = true;
    pc_grade(&hidden, &v);
    CHECK(v.notes & PC_NOTE_HIDDEN, "hidden SSID noted");
    CHECK_EQ(v.score, 100);

    /* Vocabulary: no grade or advice may promise safety. */
    for (int g = PC_GRADE_UNGRADED; g <= PC_GRADE_A_PLUS; g++) {
        const char *nm = pc_grade_name((pc_grade_t)g);
        CHECK(nm && *nm, "grade %d named", g);
        CHECK(strstr(nm, "SAFE") == NULL, "no grade claims safety");
    }
    pc_grade(&best, &v);
    CHECK(strstr(pc_grade_advice(&v), "not the same as no exposure") != NULL,
          "even the best grade refuses to promise safety");

    /* Sort order: worst posture first, ungraded last. */
    pc_verdict_t vb, vo, vt;
    pc_grade(&best, &vb);
    pc_grade(&open, &vo);
    pc_grade(&thin, &vt);
    CHECK(pc_compare(&open, &vo, &best, &vb) < 0, "worse posture sorts first");
    CHECK(pc_compare(&thin, &vt, &best, &vb) > 0, "ungraded sorts last, not first");
}

/* ------------------------------------------------------------------ twin */

static pc_ap_t twin_member(uint8_t b3, uint8_t chan, int8_t rssi)
{
    pc_ap_t ap = ap_wpa3();
    ap.bssid[0] = 0xAC; ap.bssid[1] = 0x11; ap.bssid[2] = 0x22;
    ap.bssid[3] = b3; ap.bssid[4] = 0x00; ap.bssid[5] = 0x01;
    ap.channel = chan;
    ap.rssi = rssi;
    return ap;
}

void test_twin(void)
{
    banner("twin: rogue access point detection");
    pt_context_t ctx = { .dwell_permil = 1000 };
    pt_verdict_t v;

    /* One radio cannot impersonate itself. */
    pc_ap_t one = twin_member(1, 6, -50);
    pt_evaluate(&one, 1, NULL, &ctx, &v);
    CHECK_EQ(v.band, PT_BAND_CONSISTENT);
    CHECK(v.notes & PT_NOTE_SINGLE, "single member disclosed");

    /* THE test. A corporate roaming deployment is one SSID on many BSSIDs,
     * which is exactly what a naive detector calls an evil twin. */
    pc_ap_t roam[6];
    const uint8_t chans[6] = { 1, 6, 11, 1, 6, 11 };
    const int8_t rss[6] = { -50, -55, -60, -52, -58, -62 };
    for (unsigned i = 0; i < 6; i++) {
        roam[i] = twin_member((uint8_t)(0x10 + i), chans[i], rss[i]);
    }
    pt_evaluate(roam, 6, NULL, &ctx, &v);
    CHECK_EQ(v.band, PT_BAND_CONSISTENT);
    CHECK(v.notes & PT_NOTE_ROAMING, "multiplicity recognised as roaming");
    CHECK_EQ(v.families, 0);
    CHECK(v.score <= 19, "six BSSIDs on one SSID is not an attack (got %u)", v.score);
    CHECK_EQ(v.members, 6);

    /* An open member of a protected SSID, on a software address, on its own
     * channel, far louder than the infrastructure. */
    pc_ap_t evil[4];
    evil[0] = twin_member(0x10, 1, -60);
    evil[1] = twin_member(0x11, 6, -65);
    evil[2] = twin_member(0x12, 11, -70);
    evil[3] = ap_base("office", 3, -35);
    evil[3].bssid[0] = 0x02; /* locally administered: a soft AP */
    evil[3].bssid[1] = 0xAA; evil[3].bssid[2] = 0xBB;
    evil[3].bssid[3] = 0xCC; evil[3].bssid[4] = 0xDD; evil[3].bssid[5] = 0xEE;
    evil[3].beacon_ms = 102;

    pt_evaluate(evil, 4, NULL, &ctx, &v);
    CHECK_EQ(v.band, PT_BAND_TWIN_LIKELY);
    CHECK(v.families & PT_FAM_POSTURE, "posture family");
    CHECK(v.families & PT_FAM_IDENTITY, "identity family");
    CHECK(v.families & PT_FAM_BEHAVIOUR, "behaviour family");
    CHECK(v.notes & PT_NOTE_OPEN_MEMBER, "open member disclosed");
    CHECK(v.notes & PT_NOTE_LOCAL_MAC, "software address disclosed");
    CHECK_EQ(v.suspect_index, 3);
    CHECK(memcmp(v.suspect, evil[3].bssid, 6) == 0, "suspect identified");
    CHECK(v.rssi_excess >= 18, "signal excess measured (%d dB)", v.rssi_excess);
    CHECK(v.score <= v.ceiling, "score within ceiling");

    /* Odd, but not asking for less: identity and behaviour alone must not
     * raise an alarm however strong they get. */
    pc_ap_t odd[4];
    odd[0] = twin_member(0x10, 1, -60);
    odd[1] = twin_member(0x11, 6, -65);
    odd[2] = twin_member(0x12, 11, -70);
    odd[3] = ap_wpa3();                 /* identical posture to the others */
    odd[3].bssid[0] = 0x02;             /* but a software address ...      */
    odd[3].bssid[1] = 0x99; odd[3].bssid[2] = 0x88;
    odd[3].bssid[3] = 0x77; odd[3].bssid[4] = 0x66; odd[3].bssid[5] = 0x55;
    odd[3].channel = 3;                 /* ... on its own channel ...      */
    odd[3].rssi = -35;                  /* ... and much louder             */
    odd[3].beacon_ms = 102;

    pt_evaluate(odd, 4, NULL, &ctx, &v);
    CHECK(!(v.families & PT_FAM_POSTURE), "no posture gap");
    CHECK(v.families & PT_FAM_IDENTITY, "identity still fires");
    CHECK(v.families & PT_FAM_BEHAVIOUR, "behaviour still fires");
    CHECK(v.band == PT_BAND_ANOMALOUS,
          "without a posture gap this is odd, not hostile (got %s)",
          pt_band_name(v.band));
    CHECK(v.score <= 59, "the alarm band needs the posture family (got %u)", v.score);
    CHECK(v.raw_score > v.score, "the cap is visible");

    /* A site profile is real evidence, so it raises the ceiling. */
    pt_profile_t prof;
    pt_profile_reset(&prof);
    CHECK(!prof.loaded, "empty profile is not loaded");
    for (unsigned i = 0; i < 3; i++) {
        CHECK(pt_profile_add(&prof, evil[i].bssid), "profile add");
    }
    CHECK(pt_profile_add(&prof, evil[0].bssid), "duplicate add is idempotent");
    CHECK_EQ(prof.n, 3);
    CHECK(prof.loaded, "profile loaded");
    CHECK(pt_profile_contains(&prof, evil[1].bssid), "known BSSID");
    CHECK(!pt_profile_contains(&prof, evil[3].bssid), "unknown BSSID");
    CHECK(pt_ceiling(&ctx, &prof) > pt_ceiling(&ctx, NULL),
          "a baseline raises what a verdict may claim");

    pt_verdict_t vp;
    pt_evaluate(evil, 4, &prof, &ctx, &vp);
    CHECK(!(vp.notes & PT_NOTE_NO_PROFILE), "profile presence disclosed");
    CHECK(vp.score >= v.score, "the baseline does not weaken the case");

    /* Hopping lowers the ceiling here too. */
    pt_context_t hop = { .dwell_permil = 71 };
    CHECK(pt_ceiling(&hop, NULL) < pt_ceiling(&ctx, NULL), "hopping lowers the ceiling");
    CHECK(pt_ceiling(&ctx, NULL) < 100, "nothing here is certain");

    /* Vocabulary. */
    for (int b = PT_BAND_CONSISTENT; b <= PT_BAND_TWIN_LIKELY; b++) {
        const char *nm = pt_band_name((pt_band_t)b);
        const char *ad = pt_band_advice((pt_band_t)b);
        CHECK(nm && *nm && ad && *ad, "band %d described", b);
        CHECK(strstr(nm, "SAFE") == NULL, "no band claims safety");
        CHECK(strstr(ad, " is safe") == NULL, "advice never says safe");
    }
}

/* ---------------------------------------------------------------- report */

#define CANARY 0x7E

static void fill_canary(char *b, size_t n) { memset(b, CANARY, n); }

static bool canary_intact(const char *b, size_t from, size_t to)
{
    for (size_t i = from; i < to; i++) {
        if ((unsigned char)b[i] != CANARY) return false;
    }
    return true;
}

void test_report(void)
{
    banner("report: bounded, escaped, redacted evidence");
    char buf[512];
    prt_t w;
    const uint8_t mac[6] = { 0xAC, 0x11, 0x22, 0x33, 0x44, 0x55 };

    /* Structure: nested containers close with the right bracket. */
    prt_init(&w, buf, sizeof(buf), PRT_REDACT_NONE, 0);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_u32(&w, "score", 60);
    prt_i32(&w, "rssi", -70);
    prt_bool(&w, "camped", false);
    prt_arr_begin(&w, "aps");
    prt_obj_begin(&w, NULL);
    prt_mac(&w, "bssid", mac);
    prt_obj_end(&w);
    prt_arr_end(&w);
    prt_obj_end(&w);
    CHECK(prt_finish(&w), "well-formed write succeeds");
    CHECK(strcmp(buf,
                 "{\"tool\":\"pharos\",\"score\":60,\"rssi\":-70,\"camped\":false,"
                 "\"aps\":[{\"bssid\":\"ac:11:22:33:44:55\"}]}") == 0,
          "exact JSON: %s", buf);

    /* prt_finish closes unbalanced containers with the correct brackets. */
    prt_init(&w, buf, sizeof(buf), PRT_REDACT_NONE, 0);
    prt_obj_begin(&w, NULL);
    prt_arr_begin(&w, "open");
    prt_obj_begin(&w, NULL);
    prt_u32(&w, "n", 1);
    CHECK(prt_finish(&w), "auto-close succeeds");
    CHECK(strcmp(buf, "{\"open\":[{\"n\":1}]}") == 0, "auto-closed JSON: %s", buf);

    /* The hostile SSID. 32 bytes of somebody else's choosing must become a
     * JSON string, never JSON structure. */
    prt_init(&w, buf, sizeof(buf), PRT_REDACT_NONE, 0);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "ssid", "\", \"admin\": true, \"x\": \"");
    prt_obj_end(&w);
    CHECK(prt_finish(&w), "hostile SSID written");
    CHECK(strstr(buf, "\\\"admin\\\"") != NULL, "quotes escaped: %s", buf);
    CHECK(strstr(buf, "\"admin\": true") == NULL, "no forged field escapes the string");

    prt_init(&w, buf, sizeof(buf), PRT_REDACT_NONE, 0);
    prt_obj_begin(&w, NULL);
    prt_strn(&w, "ssid", "a\x01\nb\\c", 6);
    prt_obj_end(&w);
    CHECK(prt_finish(&w), "control characters written");
    CHECK(strstr(buf, "\\u0001") != NULL, "control byte escaped: %s", buf);
    CHECK(strstr(buf, "\\n") != NULL, "newline escaped");
    CHECK(strstr(buf, "\\\\") != NULL, "backslash escaped");

    /* Redaction happens at write time; the full address never exists. */
    prt_init(&w, buf, sizeof(buf), PRT_REDACT_OUI, 0);
    prt_obj_begin(&w, NULL);
    prt_mac(&w, "bssid", mac);
    prt_obj_end(&w);
    prt_finish(&w);
    CHECK(strstr(buf, "ac:11:22:xx:xx:xx") != NULL, "OUI redaction: %s", buf);
    CHECK(strstr(buf, "33:44:55") == NULL, "device half never written");

    char hashed_a[128], hashed_b[128];
    prt_init(&w, hashed_a, sizeof(hashed_a), PRT_REDACT_HASH, 0xA5A5A5A5u);
    prt_obj_begin(&w, NULL); prt_mac(&w, "b", mac); prt_obj_end(&w); prt_finish(&w);
    prt_init(&w, hashed_b, sizeof(hashed_b), PRT_REDACT_HASH, 0x5A5A5A5Au);
    prt_obj_begin(&w, NULL); prt_mac(&w, "b", mac); prt_obj_end(&w); prt_finish(&w);
    CHECK(strstr(hashed_a, "\"h:") != NULL, "hash form: %s", hashed_a);
    CHECK(strstr(hashed_a, "ac:11") == NULL, "no address in the hashed form");
    CHECK(strcmp(hashed_a, hashed_b) != 0,
          "a fresh salt stops two sessions being joined together");

    prt_init(&w, hashed_b, sizeof(hashed_b), PRT_REDACT_HASH, 0xA5A5A5A5u);
    prt_obj_begin(&w, NULL); prt_mac(&w, "b", mac); prt_obj_end(&w); prt_finish(&w);
    CHECK(strcmp(hashed_a, hashed_b) == 0, "same salt correlates within a session");

    /* Truncation is reported, not hidden, and never overruns. */
    char small[600];
    for (size_t cap = 2; cap < 90; cap++) {
        fill_canary(small, sizeof(small));
        prt_init(&w, small, cap, PRT_REDACT_NONE, 0);
        prt_obj_begin(&w, NULL);
        prt_str(&w, "a_reasonably_long_key_name", "and a reasonably long value too");
        prt_mac(&w, "bssid", mac);
        prt_obj_end(&w);
        const bool ok = prt_finish(&w);
        CHECK(canary_intact(small, cap, sizeof(small)),
              "no write past the buffer at cap %zu", cap);
        CHECK(strlen(small) < cap, "NUL-terminated within the buffer at cap %zu", cap);
        if (!ok) {
            CHECK(prt_len(&w) < cap, "truncated length stays in bounds");
        }
    }
    /* And the same content in a buffer that is definitely big enough works. */
    prt_init(&w, small, sizeof(small), PRT_REDACT_NONE, 0);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "a_reasonably_long_key_name", "and a reasonably long value too");
    prt_mac(&w, "bssid", mac);
    prt_obj_end(&w);
    CHECK(prt_finish(&w), "ample buffer succeeds");

    /* Integer edges. */
    prt_init(&w, buf, sizeof(buf), PRT_REDACT_NONE, 0);
    prt_obj_begin(&w, NULL);
    prt_i32(&w, "min", -2147483647 - 1);
    prt_u32(&w, "max", 4294967295u);
    prt_u32(&w, "zero", 0);
    prt_null(&w, "absent");
    prt_obj_end(&w);
    CHECK(prt_finish(&w), "integer edges written");
    CHECK(strstr(buf, "\"min\":-2147483648") != NULL, "INT32_MIN: %s", buf);
    CHECK(strstr(buf, "\"max\":4294967295") != NULL, "UINT32_MAX");
    CHECK(strstr(buf, "\"zero\":0") != NULL, "zero");
    CHECK(strstr(buf, "\"absent\":null") != NULL, "null");
}

/* ------------------------------------------------------------------ dial */

void test_dial(void)
{
    banner("dial: launcher, gauge and type that survives the curve");
    pd_dial_t d;

    pd_dial_layout(8, 0.0f, PR_RING_R, PR_SAFE_R, &d);
    CHECK(d.hittable, "eight items are comfortably hittable");
    CHECK_NEAR(d.step_deg, 45.0, 0.01);
    CHECK_NEAR(pd_dial_item_angle(&d, 2), 90.0, 0.01);
    CHECK_NEAR(pd_dial_item_angle(&d, 8), 0.0, 0.01);

    /* Every item can be hit at its own angle, and only that item. */
    for (unsigned i = 0; i < 8; i++) {
        pr_point_t p = pr_polar(206, pd_dial_item_angle(&d, i));
        CHECK_EQ(pd_dial_hit(&d, p.x, p.y), (int)i);
    }
    pr_point_t centre = { PR_CX, PR_CY };
    CHECK_EQ(pd_dial_hit(&d, centre.x, centre.y), -1);

    /* Too many items must be reported, not silently drawn unhittable. */
    pd_dial_layout(40, 0.0f, PR_RING_R, PR_SAFE_R, &d);
    CHECK(!d.hittable, "forty wedges cannot be hit with a thumb");
    CHECK(d.max_hittable > 0 && d.max_hittable < 40,
          "the layout says how many would fit (%u)", d.max_hittable);

    pd_dial_layout(12, 0.0f, PR_RING_R, PR_SAFE_R, &d);
    CHECK_EQ(pd_dial_selected(&d, 0.0f), 0);
    CHECK_EQ(pd_dial_selected(&d, -30.0f), 1);
    CHECK_EQ(pd_dial_selected(&d, -360.0f), 0);

    /* The gauge shows what was capped away rather than dropping it. */
    pd_gauge_t g;
    const uint8_t comps[4] = { 40, 22, 22, 12 }; /* raw 96 ... */
    pd_gauge_layout(comps, 4, 60, 60, 0.0f, 300.0f, &g); /* ... allowed 60 */
    CHECK(g.capped, "capping detected");
    CHECK_EQ(g.denied_points, 36);
    CHECK_NEAR(g.score_deg, 180.0, 0.01);
    CHECK_NEAR(g.ceiling_deg, 180.0, 0.01);
    unsigned earned = 0, denied = 0;
    for (unsigned i = 0; i < g.n_arcs; i++) {
        if (g.arcs[i].denied) denied += g.arcs[i].value; else earned += g.arcs[i].value;
    }
    CHECK_EQ(earned, 60);
    CHECK(denied > 0, "denied points are drawn, not discarded");
    CHECK(earned + denied <= 96, "no points invented");

    pd_gauge_layout(comps, 4, 96, 96, 0.0f, 300.0f, &g);
    CHECK(!g.capped, "an uncapped verdict has no denied arc");
    CHECK_EQ(g.denied_points, 0);

    /* Type. A label that cannot fit must report zero rather than be clipped. */
    CHECK_EQ(pd_label_size(2, 0, PR_SAFE_R), 64);
    CHECK(pd_label_size(40, 190, PR_SAFE_R) == 0,
          "a forty-character label near the rim does not fit at any size");
    CHECK(pd_label_size(12, 0, PR_SAFE_R) > 0, "a short centred label fits");

    /* Capacity shrinks as you move away from the middle, at every size. */
    for (unsigned i = 0; i < 4; i++) {
        const int16_t s = PD_TYPE_SCALE[i];
        CHECK(pd_label_capacity(s, 0, PR_SAFE_R) >= pd_label_capacity(s, 150, PR_SAFE_R),
              "capacity narrows off centre at size %d", s);
        CHECK_EQ(pd_label_capacity(s, PR_SAFE_R + 10, PR_SAFE_R), 0);
    }

    /* Every string the firmware ships must fit somewhere it is drawn. */
    static const char *core_labels[] = {
        "QUIET", "BACKGROUND", "ELEVATED", "SUSPICIOUS", "FLOOD LIKELY",
        "CONSISTENT", "MIXED ESTATE", "ANOMALOUS", "TWIN LIKELY",
        "Spectrum", "Census", "Watch", "Twin", "Probe", "Beacon", "Sentry",
    };
    for (unsigned i = 0; i < sizeof(core_labels) / sizeof(core_labels[0]); i++) {
        const unsigned n = (unsigned)strlen(core_labels[i]);
        const int16_t size = pd_label_size(n, 34, PR_RING_R);
        CHECK(size > 0, "\"%s\" fits in the core at some size", core_labels[i]);
        CHECK(pd_label_capacity(size, 34, PR_RING_R) >= n,
              "\"%s\" fits at the size chosen for it", core_labels[i]);
    }
}
