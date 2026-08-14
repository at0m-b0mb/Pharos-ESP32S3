#ifndef PHAROS_TEST_SUPPORT_H
#define PHAROS_TEST_SUPPORT_H

#include <math.h>
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

#endif /* PHAROS_TEST_SUPPORT_H */
