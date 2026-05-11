/* test/unit/harness.h
 *
 * Hand-rolled C99 unit-test harness for thermal-core.
 * No third-party deps (per implementation plan §1.6).
 * One TEST_CASE per .c file; the macro defines main().
 *
 * Macros:
 *   TEST_CASE(name)                              — declare the test body
 *   EXPECT_EQ(actual, expected)                  — integer equality
 *   EXPECT_NEAR_Q16_16(actual, expected, tol)    — Q16.16 nearness
 *   EXPECT_STATUS_OK(actual)                     — thermal_status_t == 0
 *
 * On failure: prints location + values to stderr and exit(1).
 */
#ifndef THERMAL_TEST_HARNESS_H
#define THERMAL_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define EXPECT_EQ(actual, expected)                                            \
    do {                                                                       \
        long long _a = (long long)(actual);                                    \
        long long _e = (long long)(expected);                                  \
        if (_a != _e) {                                                        \
            fprintf(stderr,                                                    \
                    "FAIL %s:%d: EXPECT_EQ(%s, %s): got %lld, expected %lld\n",\
                    __FILE__, __LINE__, #actual, #expected, _a, _e);           \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define EXPECT_STATUS_OK(actual)                                               \
    do {                                                                       \
        int _s = (int)(actual);                                                \
        if (_s != 0) {                                                         \
            fprintf(stderr,                                                    \
                    "FAIL %s:%d: EXPECT_STATUS_OK(%s): got %d\n",              \
                    __FILE__, __LINE__, #actual, _s);                          \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define EXPECT_NEAR_Q16_16(actual, expected, tol)                              \
    do {                                                                       \
        int32_t _a = (int32_t)(actual);                                        \
        int32_t _e = (int32_t)(expected);                                      \
        int32_t _t = (int32_t)(tol);                                           \
        int32_t _d = (_a > _e) ? (_a - _e) : (_e - _a);                        \
        if (_d > _t) {                                                         \
            fprintf(stderr,                                                    \
                    "FAIL %s:%d: EXPECT_NEAR_Q16_16(%s, %s, %s):"              \
                    " |%d - %d| = %d > %d\n",                                  \
                    __FILE__, __LINE__, #actual, #expected, #tol,              \
                    _a, _e, _d, _t);                                           \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define TEST_CASE(name)                                                        \
    static void test_##name(void);                                             \
    int main(void) {                                                           \
        printf("RUN  test_%s\n", #name);                                       \
        test_##name();                                                         \
        printf("PASS test_%s\n", #name);                                       \
        return 0;                                                              \
    }                                                                          \
    static void test_##name(void)

#endif /* THERMAL_TEST_HARNESS_H */
