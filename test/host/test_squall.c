/* Pharos host tests, part thirteen: Squall - busy, broken, or denied.
 *
 * The test that matters most here is the one asserting a BUSY BUILDING is not
 * called an attack. Every naive jamming detector ever shipped fails that one,
 * and a tool that cries wolf at a crowded office gets switched off.
 */
#include "pharos_squall.h"
#include "test_support.h"

static pq_dwell_t dw(uint8_t ch, uint16_t ms, uint16_t frames, uint16_t retries,
                     uint16_t busy)
{
    pq_dwell_t d;
    memset(&d, 0, sizeof(d));
    d.channel = ch;
    d.dwell_ms = ms;
    d.frames = frames;
    d.retries = retries;
    d.busy_permil = busy;
    d.noise_floor = -92;
    d.peak_rssi = -40;
    return d;
}

/* Feed the same kind of visit n times, so a channel clears PQ_MIN_SAMPLES. */
static void feed(pq_state_table_t *t, unsigned n, pq_dwell_t d)
{
    for (unsigned i = 0; i < n; i++) {
        pq_observe(t, &d);
    }
}

static pq_context_t ctx_of(uint16_t dwell, bool camped)
{
    pq_context_t c;
    memset(&c, 0, sizeof(c));
    c.dwell_permil = dwell;
    c.camped = camped;
    return c;
}

void test_squall(void)
{
    banner("squall: busy, broken, or denied");
    pq_state_table_t t;
    pq_verdict_t v;
    pq_context_t camped = ctx_of(1000, true);

    /* Nothing observed. */
    pq_reset(&t);
    pq_evaluate(&t, &camped, &v);
    CHECK(v.notes & PQ_NOTE_FEW, "no data disclosed as such");
    CHECK_EQ(v.n_graded, 0);
    CHECK(strstr(v.headline, "Not enough") != NULL, "headline: %s", v.headline);

    /* One bad visit is a sample, not a finding. */
    pq_reset(&t);
    feed(&t, 1, dw(6, 300, 0, 0, 900));
    pq_evaluate(&t, &camped, &v);
    CHECK_EQ(v.n_graded, 0);
    CHECK(v.notes & PQ_NOTE_FEW, "one dwell cannot grade a channel");

    /* THE false positive every naive detector fails: a busy building. Loud AND
     * productive. It must read CONGESTED, never denial. */
    pq_reset(&t);
    for (uint8_t ch = 1; ch <= 11; ch += 5) {
        feed(&t, 5, dw(ch, 400, 400, 40, 700)); /* 1000 fps, very loud */
    }
    pq_evaluate(&t, &camped, &v);
    CHECK_EQ(v.worst, PQ_STATE_CONGESTED);
    CHECK_EQ(v.n_denial, 0);
    CHECK(v.score < 70, "a busy building is never an alarm (got %u)", v.score);
    CHECK(strstr(pq_state_advice(v.worst), "not an attack") != NULL,
          "advice says plainly it is not an attack");
    CHECK(strstr(v.headline, "Busy") != NULL, "headline: %s", v.headline);

    /* Quiet air: little energy, few frames. Honest about ambiguity. */
    pq_reset(&t);
    feed(&t, 5, dw(1, 400, 1, 0, 40));
    pq_evaluate(&t, &camped, &v);
    CHECK_EQ(v.worst, PQ_STATE_QUIET);
    CHECK_EQ(v.score, 0);
    CHECK(strstr(pq_state_advice(v.worst), "not hearing") != NULL,
          "QUIET admits it may be a deaf receiver");

    /* Healthy: energy and traffic in proportion. */
    pq_reset(&t);
    feed(&t, 5, dw(6, 400, 60, 3, 200));
    pq_evaluate(&t, &camped, &v);
    CHECK_EQ(v.worst, PQ_STATE_HEALTHY);
    CHECK_EQ(v.score, 0);

    /* THE detection: loud and barren, across the band, with senders suffering.
     * Energy family + spread family + retries. */
    pq_reset(&t);
    for (uint8_t ch = 1; ch <= 11; ch++) {
        /* 800 permil busy, almost nothing decoding, and what does decode is
         * mostly retransmission. */
        feed(&t, 5, dw(ch, 400, 2, 1, 820));
    }
    pq_evaluate(&t, &camped, &v);
    CHECK_EQ(v.worst, PQ_STATE_DENIAL);
    CHECK(v.n_denial >= 3, "denial seen across the band (%u)", v.n_denial);
    CHECK(v.families & PQ_FAM_ENERGY, "energy family");
    CHECK(v.families & PQ_FAM_SPREAD, "spread family");
    CHECK(v.score >= 70, "band-wide denial reaches the alarm (got %u)", v.score);
    CHECK(strstr(pq_state_advice(v.worst), "walk to it") != NULL,
          "advice makes it a physical search");

    /* THE cap: energy on ONE channel with no corroboration is a device, not an
     * attack, and must not reach the top. */
    pq_reset(&t);
    feed(&t, 6, dw(6, 400, 1, 0, 900));
    pq_evaluate(&t, &camped, &v);
    CHECK_EQ(v.n_denial, 1);
    CHECK(v.notes & PQ_NOTE_NARROW, "single-channel disclosed");
    CHECK(v.score <= 74, "one loud channel is capped (got %u)", v.score);
    CHECK(v.families == PQ_FAM_ENERGY, "energy only, no corroboration");
    CHECK(v.score <= 62, "energy alone is capped harder (got %u)", v.score);
    CHECK(v.worst != PQ_STATE_DENIAL,
          "and the WORD is downgraded too, not just the number (got %s)",
          pq_state_name(v.worst));
    CHECK(strstr(v.headline, "device") != NULL,
          "headline offers the innocent explanation: %s", v.headline);

    /* Degraded: usable but fighting - high retries, frames still flowing. */
    pq_reset(&t);
    feed(&t, 5, dw(6, 400, 100, 60, 300));
    pq_evaluate(&t, &camped, &v);
    CHECK_EQ(v.worst, PQ_STATE_DEGRADED);
    CHECK(v.retry_permil >= 500, "retry fraction computed (%u)", v.retry_permil);
    CHECK(v.families & PQ_FAM_RETRIES, "retry family");
    CHECK(v.score < 70, "retries alone do not prove denial");

    /* Hopping cannot tell "no frames" from "not listening", and its ceiling is
     * the harshest in Pharos for exactly that reason. */
    {
        pq_context_t hopping = ctx_of(71, false);
        pq_verdict_t hv;
        pq_reset(&t);
        for (uint8_t ch = 1; ch <= 11; ch++) {
            feed(&t, 5, dw(ch, 400, 2, 1, 820));
        }
        pq_evaluate(&t, &hopping, &hv);
        CHECK(hv.notes & PQ_NOTE_THIN, "thin sweep disclosed");
        CHECK(hv.score < 70, "hopping cannot claim denial (got %u)", hv.score);
        CHECK(pq_ceiling(&hopping) < pq_ceiling(&camped),
              "camping buys confidence");
        CHECK(pq_ceiling(&camped) < 100, "nothing here is certain");
    }

    /* Congestion and denial can coexist; the worst one is reported. */
    pq_reset(&t);
    feed(&t, 5, dw(1, 400, 500, 20, 700));  /* congested   */
    for (uint8_t ch = 6; ch <= 11; ch++) {
        feed(&t, 5, dw(ch, 400, 1, 1, 850)); /* denied      */
    }
    pq_evaluate(&t, &camped, &v);
    CHECK(v.n_congested >= 1, "congestion still counted (%u)", v.n_congested);
    CHECK(v.n_denial >= 3, "denial still counted (%u)", v.n_denial);
    CHECK_EQ(v.worst, PQ_STATE_DENIAL);

    /* Bounds: channel 0 and 15 are refused rather than corrupting the table. */
    pq_reset(&t);
    {
        pq_dwell_t bad;
        bad = dw(0, 400, 10, 0, 100);   pq_observe(&t, &bad);
        bad = dw(15, 400, 10, 0, 100);  pq_observe(&t, &bad);
        bad = dw(200, 400, 10, 0, 100); pq_observe(&t, &bad);
    }
    pq_evaluate(&t, &camped, &v);
    CHECK_EQ(v.n_graded, 0);

    /* NULLs are survivable. */
    {
        pq_dwell_t any = dw(6, 400, 10, 0, 100);
        pq_reset(NULL);
        pq_observe(NULL, &any);
        pq_observe(&t, NULL);
    }
    pq_evaluate(NULL, &camped, &v);
    pq_evaluate(&t, &camped, NULL);
    CHECK(true, "NULL arguments do not crash");

    /* Vocabulary: no state promises safety, and every one is explained. */
    for (int i = 0; i < PQ_STATE_COUNT; i++) {
        const char *nm = pq_state_name((pq_state_t)i);
        const char *ad = pq_state_advice((pq_state_t)i);
        CHECK(nm && *nm && ad && *ad, "state %d described", i);
        CHECK(strstr(nm, "SAFE") == NULL && strstr(nm, "SECURE") == NULL,
              "no state claims safety");
        CHECK(strstr(ad, " is safe") == NULL, "advice never says safe");
    }
}
