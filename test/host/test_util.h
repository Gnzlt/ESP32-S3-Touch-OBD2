/* The check macros the host test files share; the counters live in test_main.c. */
#pragma once

#include <math.h>
#include <stdio.h>
#include <string.h>

extern int s_failures, s_checks;

#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failures++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))
#define CHECK_STR(a, b) do { s_checks++; if (strcmp((a), (b)) != 0) { s_failures++; \
    printf("  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

// test_tpms.c
void test_tpms_adv(void);
void test_tpms_formats(void);
void test_tpms_tyres(void);
void test_tpms_alerts(void);
void test_tpms_pair(void);
void test_tpms_cfgfile(void);
