/* Pharos lens: Survey - what this place is actually like
 *
 * The screen somebody reads when they want to know whether picking this device
 * up was worth it.
 *
 * Every other lens answers a question in the present tense and forgets. With
 * the watches on a rotation, "the present tense" is a five-second glance every
 * forty seconds - so Census would grade twenty-three networks, find six that
 * would drop their clients under a flood, and lose all of it when the radio
 * moved on. The device knew something worth knowing about the room and had
 * nowhere to put it.
 *
 * This lens holds no radio and needs none: the survey accumulates in the UI
 * loop as the rotation feeds it (see pharos_survey_hook.h), and this file only
 * reads it back. The wording lives in pharos_survey.c, where it is host-tested
 * - including a test that it never calls somebody's badly configured network
 * an attack.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "pharos_lens.h"
#include "pharos_census.h"
#include "pharos_survey.h"
#include "pharos_survey_hook.h"

static const char *TAG = "lens.survey";

static bool survey_mount(void)
{
    ESP_LOGI(TAG, "survey: reading the accumulated picture");
    return true;
}

static bool k_survey_display(struct pharos_lens_display *o)
{
    psv_report_t r;
    if (!pharos_survey_read((struct psv_report *)&r)) {
        return false;
    }

    snprintf(o->big, sizeof(o->big), "%u", r.networks);
    snprintf(o->band, sizeof(o->band), "%s",
             r.networks ? "networks seen" : "still listening");
    if (r.minutes) {
        snprintf(o->detail, sizeof(o->detail), "over %u min%s", (unsigned)r.minutes,
                 r.truncated ? "  (a floor)" : "");
    } else {
        snprintf(o->detail, sizeof(o->detail), "%s", "less than a minute so far");
    }
    snprintf(o->advice, sizeof(o->advice), "%s", r.headline);

    /* The single most useful sentence goes on the glass under the number. */
    char line[40];
    if (psv_line(&r, 1, line, sizeof(line))) {
        snprintf(o->why, sizeof(o->why), "%.47s", line);
    }

    o->families = (uint8_t)((r.networks ? 1u : 0u) | (r.no_mfp ? 2u : 0u) |
                            (r.devices ? 4u : 0u) | (r.tools ? 8u : 0u));
    o->fam_label[0] = "NETS";
    o->fam_label[1] = "WEAK";
    o->fam_label[2] = "LEAK";
    o->fam_label[3] = "TOOLS";

    /* NO SCORE, DELIBERATELY.
     *
     * There is no honest 0..100 for "what is this place like". A number would
     * invite comparison between rooms it cannot support, and it would put a
     * neighbour's WPS setting on the same dial as a deauthentication flood.
     * The headline is a count of things actually heard, which is a fact. */
    o->has_score = false;

    /* And the ring is told directly that this is a survey, not a watch. */
    o->has_alert = true;
    o->alert = r.tools_present ? 2u : (r.open || r.no_mfp) ? 1u : 0u;
    return true;
}

/* THE ROWS: plain English first, numbers after.
 *
 * The order is the point. Somebody who reads only the first three lines should
 * come away knowing the useful thing, so the sentences come first and the
 * supporting counts sit underneath for anyone who wants them. */
static bool k_survey_row(unsigned index, struct pharos_lens_row *out)
{
    psv_report_t r;
    if (!pharos_survey_read((struct psv_report *)&r)) {
        return false;
    }

    char line[40];
    if (psv_line(&r, index, line, sizeof(line))) {
        snprintf(out->left, sizeof(out->left), "%.25s", line);
        out->right[0] = '\0';
        /* Tone by what the line is ABOUT, not by how big the number is: the
         * good-news line must not be red because five networks run WPA3. */
        out->tone = (strstr(line, "WPA3") || strstr(line, "nothing"))
                        ? PHAROS_TONE_GOOD
                    : (strstr(line, "open") || strstr(line, "broken") ||
                       strstr(line, "pentest"))
                        ? PHAROS_TONE_BAD
                    : (strstr(line, "minimum")) ? PHAROS_TONE_DIM
                                                : PHAROS_TONE_WARN;
        return true;
    }

    /* Then the counts, for anybody who wants the arithmetic. */
    unsigned k = index;
    for (unsigned i = 0; i < 16 && psv_line(&r, i, line, sizeof(line)); i++) {
        k--;
    }
    switch (k) {
    case 0:
        snprintf(out->left, sizeof(out->left), "surveyed for");
        snprintf(out->right, sizeof(out->right), "%u min", (unsigned)r.minutes);
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "worst grade seen");
        /* pc_grade_name() is the ONE place a grade becomes a letter. This was
         * a hand-rolled ladder of its own, with the comparisons the wrong way
         * round, so it printed every grade inverted - an open network as "A".
         * A second spelling of a shared rule is a second thing to get wrong;
         * there is now only the one. */
        snprintf(out->right, sizeof(out->right), "%s",
                 (r.networks && r.worst_grade != PC_GRADE_UNGRADED)
                     ? pc_grade_name((pc_grade_t)r.worst_grade)
                     : "--");
        out->tone = (r.worst_grade == PC_GRADE_UNGRADED) ? PHAROS_TONE_DIM
                  : (r.worst_grade <= PC_GRADE_D)        ? PHAROS_TONE_BAD
                  : (r.worst_grade <= PC_GRADE_B)        ? PHAROS_TONE_WARN
                                                         : PHAROS_TONE_GOOD;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "hidden names");
        snprintf(out->right, sizeof(out->right), "%u", r.hidden);
        /* Amber whatever the count: hiding an SSID is not security, and a
         * green tick here would imply it were. */
        out->tone = r.hidden ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "network names leaked");
        snprintf(out->right, sizeof(out->right), "%u", r.names_leaked);
        out->tone = r.names_leaked ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD;
        return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "counts are complete");
        snprintf(out->right, sizeof(out->right), "%s", r.truncated ? "no" : "yes");
        out->tone = r.truncated ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD;
        return true;
    default:
        return false;
    }
}

static const pharos_lens_t k_survey = {
    .id = "sys.survey",
    .purpose = "a full site survey",
    .name = "Survey",
    .summary = "What this place is like: every network and device seen so far",
    .glyph = "list",
    .kind = PHAROS_LENS_ANALYSE,
    .caps = PHAROS_CAP_NONE, /* it reads what the rotation already gathered */
    .budget_ma = 30,
    .on_mount = survey_mount,
    .display = k_survey_display,
    .row = k_survey_row,
    .row_head_left = "WHAT THIS PLACE IS LIKE",
    .row_head_right = "",
};

PHAROS_LENS_REGISTER(&k_survey);
