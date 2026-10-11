/* Minimal test harness: no external framework needed. */
#ifndef IRA_TEST_UTIL_H
#define IRA_TEST_UTIL_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_checks = 0, g_failures = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_NEAR(a, b, tol) do { g_checks++; double _a = (a), _b = (b); \
    if (!(fabs(_a - _b) <= (tol))) { g_failures++; \
    fprintf(stderr, "  FAIL %s:%d: %s = %.6f, expected %.6f\n", \
            __FILE__, __LINE__, #a, _a, _b); } } while (0)

#define RUN(test) do { int _f = g_failures; test(); \
    printf("  %-48s %s\n", #test, g_failures == _f ? "ok" : "FAILED"); } while (0)

#define TEST_SUMMARY() do { printf("  %d checks, %d failures\n", g_checks, g_failures); \
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS; } while (0)

#endif
