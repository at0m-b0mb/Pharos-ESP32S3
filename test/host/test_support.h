#ifndef PHAROS_TEST_SUPPORT_H
#define PHAROS_TEST_SUPPORT_H

#include <math.h>

/* M_PI is POSIX, not ISO C, and gcc -std=c11 therefore does not define it.
 * Apple's libc does regardless, so this only ever breaks in CI. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <string.h>

extern unsigned g_checks, g_fails;
void banner(const char *s);

#define CHECK(cond, fmt, ...)                                                     \
    do {                                                                          \
        g_checks++;                                                               \
        if (!(cond)) {                                                            \
            g_fails++;                                                            \
            printf("  FAIL %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                                         \
    } while (0)

#define CHECK_EQ(a, b) CHECK((long)(a) == (long)(b), "%ld != %ld", (long)(a), (long)(b))
#define CHECK_NEAR(a, b, tol) \
    CHECK(fabs((double)(a) - (double)(b)) <= (tol), "%g vs %g", (double)(a), (double)(b))

/* Suites defined in test_engines.c */
void test_census(void);
void test_twin(void);
void test_report(void);
void test_dial(void);

/* Suites defined in test_privacy.c */
void test_probe_classify(void);
void test_probe_grading(void);
void test_power(void);

/* Suite defined in test_region.c */
void test_region(void);

/* Suites defined in test_range.c */
void test_range_determinism(void);
void test_range_flood(void);
void test_range_proven(void);
void test_range_calm_and_roaming(void);
void test_range_probe_leak(void);
void test_range_vocabulary(void);

/* Suites defined in test_chain.c */
void test_sha256(void);
void test_chain(void);
void test_karma(void);

/* Suites defined in test_redteam.c */
void test_flood(void);
void test_opsec(void);

/* Suite defined in test_locate.c */
void test_locate(void);

/* Suites defined in test_console.c */
void test_console_tokenise(void);
void test_console_dispatch(void);
void test_console_help_and_safety(void);

/* Suite defined in test_sentinel.c */
void test_sentinel(void);

/* Suite defined in test_harvest.c */
void test_harvest(void);

/* Suite defined in test_aegis.c */
void test_aegis(void);

/* Suite defined in test_squall.c */
void test_squall(void);

/* Suite defined in test_vigil.c */
void test_vigil(void);

/* Suite defined in test_acoustic.c */
void test_acoustic(void);

/* Suite defined in test_attrib.c */
void test_attrib(void);

/* Suite defined in test_pulse.c */
void test_pulse(void);

/* Suite defined in test_skew.c */
void test_skew(void);

/* Suite defined in test_theme.c */
void test_theme(void);

/* Suite defined in test_style.c */
void test_style(void);

/* Suite defined in test_tower.c */
void test_tower(void);

/* Suite defined in test_survey.c */
void test_survey(void);

/* Suite defined in test_ring.c */
void test_ring(void);

/* Suite defined in test_motion.c */
void test_motion(void);

/* Suite defined in test_roster.c */
void test_roster(void);

/* Suite defined in test_wps.c */
void test_wps(void);

/* Suite defined in test_rival.c */
void test_rival(void);

/* Suite defined in test_watch.c */
void test_watch(void);

#endif /* PHAROS_TEST_SUPPORT_H */
