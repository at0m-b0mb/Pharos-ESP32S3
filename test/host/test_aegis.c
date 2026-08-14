/* Pharos host tests, part twelve: Aegis - correlation, and the latch.
 *
 * The properties that matter here are refusals: correlation must not invent
 * evidence out of one finding, must not lend a good sweep's confidence to a
 * thin one, and must not report a twenty-minute-old peak as the present tense.
 */
#include "pharos_aegis.h"
#include "test_support.h"

#define SEC 1000000ull

void test_aegis(void)
{
    banner("aegis: correlation without invention");
    pa_state_t s;
    pa_verdict_t v;

    /* Nothing observed at all. */
    pa_reset(&s);
    pa_evaluate(&s, 0, &v);
    CHECK_EQ(v.band, PA_BAND_CLEAR);
    CHECK_EQ(v.score, 0);
    CHECK_EQ(v.n_raised, 0);
    CHECK(strstr(pa_band_advice(v.band), "one channel at a time") != NULL,
          "even CLEAR admits the receiver's limit");

    /* Quiet background from several lenses is still CLEAR/NOTED, never more. */
    pa_reset(&s);
    pa_observe(&s, PA_STAGE_RECON, 20, 90, 1 * SEC);
    pa_observe(&s, PA_STAGE_DRIFT, 15, 90, 2 * SEC);
    pa_evaluate(&s, 3 * SEC, &v);
    CHECK_EQ(v.n_raised, 0);
    CHECK(v.band <= PA_BAND_NOTED, "background stays background (got %s)",
          pa_band_name(v.band));

    /* THE anti-invention rule: one stage raised, alone, reports its OWN score.
     * Correlation with nothing to correlate must add exactly zero. */
    pa_reset(&s);
    pa_observe(&s, PA_STAGE_DISRUPT, 66, 90, 10 * SEC);
    pa_evaluate(&s, 11 * SEC, &v);
    CHECK_EQ(v.n_raised, 1);
    CHECK_EQ(v.score, 66);
    CHECK(v.notes & PA_NOTE_SINGLE, "single-stage disclosed");
    CHECK(v.band < PA_BAND_INCIDENT, "one stage alone is never an incident");
    CHECK_EQ(v.worst, PA_STAGE_DISRUPT);

    /* Even a very loud single stage cannot be promoted by correlation. */
    pa_reset(&s);
    pa_observe(&s, PA_STAGE_HARVEST, 92, 96, 10 * SEC);
    pa_evaluate(&s, 11 * SEC, &v);
    CHECK_EQ(v.score, 92);
    CHECK_EQ(v.n_raised, 1);

    /* Two independent stages: now there is genuinely something to fuse, and
     * the score may exceed either one alone. */
    pa_reset(&s);
    pa_observe(&s, PA_STAGE_IMPERSONATE, 60, 92, 10 * SEC);
    pa_observe(&s, PA_STAGE_DISRUPT, 58, 92, 12 * SEC);
    pa_evaluate(&s, 13 * SEC, &v);
    CHECK_EQ(v.n_raised, 2);
    CHECK(v.score > 60, "two stages fuse to more than either (got %u)", v.score);
    CHECK(!(v.notes & PA_NOTE_SINGLE), "not flagged single");

    /* The full sequence, in the attacker's own order: recon, then a twin, then
     * disruption, then collection. That is a campaign, and it says so. */
    pa_reset(&s);
    pa_observe(&s, PA_STAGE_RECON,       50, 96, 10 * SEC);
    pa_observe(&s, PA_STAGE_IMPERSONATE, 62, 96, 20 * SEC);
    pa_observe(&s, PA_STAGE_DISRUPT,     58, 96, 30 * SEC);
    pa_observe(&s, PA_STAGE_HARVEST,     64, 96, 40 * SEC);
    pa_evaluate(&s, 41 * SEC, &v);
    CHECK_EQ(v.n_raised, 4);
    CHECK(v.notes & PA_NOTE_SEQUENCE, "ordered arrival recognised");
    CHECK_EQ(v.band, PA_BAND_INCIDENT);
    CHECK(strstr(v.headline, "operation") != NULL, "headline: %s", v.headline);
    CHECK(strstr(pa_band_advice(v.band), "escalate") != NULL,
          "advice tells the operator what to do");

    /* The same four findings arriving out of order is a noisy room, not a
     * campaign - it must not earn the sequence bonus. */
    {
        pa_state_t j;
        pa_verdict_t jv;
        pa_reset(&j);
        pa_observe(&j, PA_STAGE_HARVEST,     64, 96, 10 * SEC);
        pa_observe(&j, PA_STAGE_RECON,       50, 96, 20 * SEC);
        pa_observe(&j, PA_STAGE_DISRUPT,     58, 96, 30 * SEC);
        pa_observe(&j, PA_STAGE_IMPERSONATE, 62, 96, 40 * SEC);
        pa_evaluate(&j, 41 * SEC, &jv);
        CHECK_EQ(jv.n_raised, 4);
        CHECK(!(jv.notes & PA_NOTE_SEQUENCE), "jumbled order earns no bonus");
        CHECK(jv.score < v.score, "a jumble scores below a sequence (%u < %u)",
              jv.score, v.score);
    }

    /* THE latch. A burst happens, then the air goes quiet and the operator
     * finally looks. The peak must survive, and must be reported as history
     * with its age - not as the present tense. */
    pa_reset(&s);
    pa_observe(&s, PA_STAGE_DISRUPT, 80, 92, 10 * SEC);
    pa_observe(&s, PA_STAGE_DISRUPT, 0, 92, 600 * SEC); /* quiet again */
    pa_evaluate(&s, 600 * SEC, &v);
    CHECK_EQ(v.worst_peak, 80);
    CHECK(v.notes & PA_NOTE_LATCHED, "old peak marked as history");
    CHECK(v.worst_age_s >= 580, "age reported (%us)", v.worst_age_s);
    CHECK_EQ(v.n_live, 0);
    CHECK(strstr(v.headline, "not watching") != NULL,
          "headline says you missed it: %s", v.headline);
    CHECK(strstr(v.headline, "serious") != NULL,
          "and does not understate a latched 80");

    /* Acknowledging clears the latch so a fresh watch starts clean. */
    pa_acknowledge(&s);
    pa_evaluate(&s, 601 * SEC, &v);
    CHECK_EQ(v.band, PA_BAND_CLEAR);
    CHECK_EQ(v.worst_peak, 0);

    /* THE ceiling rule: a conclusion is only as good as its worst input. A
     * thin sweep contributing to the picture caps the whole picture. */
    pa_reset(&s);
    pa_observe(&s, PA_STAGE_IMPERSONATE, 70, 60, 10 * SEC); /* thin sweep */
    pa_observe(&s, PA_STAGE_DISRUPT,     75, 96, 20 * SEC); /* camped      */
    pa_evaluate(&s, 21 * SEC, &v);
    CHECK_EQ(v.ceiling, 60);
    CHECK(v.score <= 60, "score cannot exceed the weakest ceiling (got %u)", v.score);
    CHECK(v.raw_score > v.score, "and the raw score records what was cut");

    /* A stage's peak keeps the ceiling it was measured with - a later, better
     * sweep must not retroactively lend confidence to an older thin one. */
    pa_reset(&s);
    pa_observe(&s, PA_STAGE_HARVEST, 80, 55, 10 * SEC);
    pa_observe(&s, PA_STAGE_HARVEST, 30, 96, 20 * SEC);
    pa_evaluate(&s, 21 * SEC, &v);
    CHECK_EQ(v.worst_peak, 80);
    CHECK_EQ(v.ceiling, 55);

    /* Reconnaissance is common and weighted low: everyone's phone probes. */
    {
        pa_state_t r, h;
        pa_verdict_t rv, hv;
        pa_reset(&r); pa_reset(&h);
        pa_observe(&r, PA_STAGE_RECON, 40, 90, 1 * SEC);
        pa_observe(&h, PA_STAGE_HARVEST, 40, 90, 1 * SEC);
        pa_evaluate(&r, 2 * SEC, &rv);
        pa_evaluate(&h, 2 * SEC, &hv);
        CHECK(rv.score < hv.score,
              "probing is worth less than collection at equal score (%u < %u)",
              rv.score, hv.score);
    }

    /* Liveness: a fresh raised stage counts as live, an old one does not. */
    pa_reset(&s);
    pa_observe(&s, PA_STAGE_DISRUPT, 70, 90, 100 * SEC);
    pa_evaluate(&s, 101 * SEC, &v);
    CHECK_EQ(v.n_live, 1);
    CHECK(!(v.notes & PA_NOTE_STALE), "recent observation is not stale");
    pa_evaluate(&s, 900 * SEC, &v);
    CHECK_EQ(v.n_live, 0);
    CHECK(v.notes & PA_NOTE_STALE, "nothing recent is disclosed as stale");

    /* Out-of-range stages are refused rather than corrupting the table. */
    pa_reset(&s);
    pa_observe(&s, (pa_stage_t)PA_STAGE_COUNT, 99, 99, 1 * SEC);
    pa_observe(&s, (pa_stage_t)-1, 99, 99, 1 * SEC);
    pa_evaluate(&s, 2 * SEC, &v);
    CHECK_EQ(v.band, PA_BAND_CLEAR);

    /* NULLs are survivable. */
    pa_reset(NULL);
    pa_observe(NULL, PA_STAGE_RECON, 50, 90, 1);
    pa_acknowledge(NULL);
    pa_evaluate(NULL, 1, &v);
    pa_evaluate(&s, 1, NULL);
    CHECK(true, "NULL arguments do not crash");

    /* Vocabulary: every stage and band is described, and none promises safety. */
    for (int i = 0; i < PA_STAGE_COUNT; i++) {
        CHECK(pa_stage_name((pa_stage_t)i)[0] != '?', "stage %d named", i);
        CHECK(pa_stage_meaning((pa_stage_t)i)[0] != '\0', "stage %d explained", i);
    }
    for (int b = PA_BAND_CLEAR; b < PA_BAND_COUNT; b++) {
        const char *nm = pa_band_name((pa_band_t)b);
        const char *ad = pa_band_advice((pa_band_t)b);
        CHECK(nm && *nm && ad && *ad, "band %d described", b);
        CHECK(strstr(nm, "SAFE") == NULL && strstr(nm, "SECURE") == NULL,
              "no band claims safety");
        CHECK(strstr(ad, " is safe") == NULL, "advice never says safe");
    }
}
