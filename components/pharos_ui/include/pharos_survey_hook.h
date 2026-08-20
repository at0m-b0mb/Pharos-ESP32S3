/* Pharos - how a lens contributes to the session survey.
 *
 * A lens is unmounted every few seconds by the rotation, so it cannot hold a
 * picture of the whole session. The survey lives in the UI loop (see
 * pharos_ui.c) and lenses push facts into it through these, exactly as they
 * push findings to Aegis through pharos_lens_t::stage_report.
 *
 * Everything here is deduplicated by address inside the survey, so a lens may
 * call these as often as it likes with whatever it currently knows - once per
 * tick is the normal case, and the count will still be right.
 */
#ifndef PHAROS_SURVEY_HOOK_H
#define PHAROS_SURVEY_HOOK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void pharos_survey_network(const uint8_t bssid[6], uint8_t grade, uint32_t flags);
void pharos_survey_device(const uint8_t mac[6], uint8_t names, bool randomised);
void pharos_survey_tool(uint8_t kind, bool present);

/* Read it back, for the Survey lens and the `survey` console command. */
struct psv_report;
bool pharos_survey_read(struct psv_report *out);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_SURVEY_HOOK_H */
