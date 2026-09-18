#ifndef COUART_TEST_MAIN_H
#define COUART_TEST_MAIN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _test_pass;
static int _test_fail;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg); \
        _test_fail++; \
    } else { \
        _test_pass++; \
    } \
} while (0)

#define ASSERT_INT_EQ(a, b) do { \
    int _a = (int)(a), _b = (int)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s:%d: %d != %d\n", __FILE__, __LINE__, _a, _b); \
        _test_fail++; \
    } else { \
        _test_pass++; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    printf("  %s ...\n", #fn); \
    fn(); \
} while (0)

#define TEST_REPORT() do { \
    printf("\n%d passed, %d failed\n", _test_pass, _test_fail); \
    return _test_fail > 0 ? 1 : 0; \
} while (0)

#endif
