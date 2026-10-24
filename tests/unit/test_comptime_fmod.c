/* test_comptime_fmod.c: the comptime evaluator's fmod (#505) against the C
 * library's, bit for bit. The compiler computes it with integer operations
 * (it does not link libm); this test links libm to compare. */
#include "unity.h"
#include "comptime/comptime.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }
static double from_bits(uint64_t u) { double d; memcpy(&d, &u, sizeof d); return d; }

static int g_checked;

static void check(double x, double y) {
    double want = fmod(x, y);
    double got = iron_comptime_fmod(x, y);
    g_checked++;
    if (want != want) {
        if (got == got) {
            char msg[160];
            snprintf(msg, sizeof msg, "fmod(%a, %a): want NaN, got %a", x, y, got);
            TEST_FAIL_MESSAGE(msg);
        }
        return;
    }
    if (bits(want) != bits(got)) {
        char msg[160];
        snprintf(msg, sizeof msg, "fmod(%a, %a): want %a, got %a", x, y, want, got);
        TEST_FAIL_MESSAGE(msg);
    }
}

static const double k_special[] = {
    0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, 2.0, 3.0, -3.0, 5.5, -5.5, 0.1, -0.1,
    10.0, 1e10, -1e10, 1e300, -1e300, 1e-300, 4.9e-324, -4.9e-324,
    2.2250738585072014e-308, 2.2250738585072009e-308, -2.2250738585072009e-308,
    1.7976931348623157e308, -1.7976931348623157e308, 3.141592653589793,
    6.283185307179586, 9007199254740993.0, 4503599627370496.5, 123456789.125,
    INFINITY, -INFINITY, NAN, -NAN,
};

void test_fmod_special_values(void) {
    size_t n = sizeof k_special / sizeof k_special[0];
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < n; j++)
            check(k_special[i], k_special[j]);
}

void test_fmod_issue_example(void) {
    TEST_ASSERT_EQUAL_DOUBLE(1.5, iron_comptime_fmod(5.5, 2.0));
    TEST_ASSERT_EQUAL_DOUBLE(-1.5, iron_comptime_fmod(-5.5, 2.0));
    TEST_ASSERT_EQUAL_DOUBLE(1.5, iron_comptime_fmod(5.5, -2.0));
}

static uint64_t g_state = 0x9E3779B97F4A7C15ull;
static uint64_t next_u64(void) {   /* xorshift64*, fixed seed */
    g_state ^= g_state >> 12;
    g_state ^= g_state << 25;
    g_state ^= g_state >> 27;
    return g_state * 0x2545F4914F6CDD1Dull;
}

/* Random bit patterns: every exponent, subnormals, infinities and NaNs. */
void test_fmod_random_bits(void) {
    for (int k = 0; k < 400000; k++)
        check(from_bits(next_u64()), from_bits(next_u64()));
}

/* Operands of close magnitude, where the long division ends early, and
 * random values against small divisors. */
void test_fmod_random_near(void) {
    for (int k = 0; k < 200000; k++) {
        uint64_t a = next_u64();
        uint64_t e = (a >> 52) & 0x7ff;
        uint64_t b = (next_u64() & 0x800fffffffffffffull) |
                     ((uint64_t)((e + (next_u64() % 9)) & 0x7ff) << 52);
        check(from_bits(a), from_bits(b));
        double small = (double)(int64_t)(next_u64() % 2001) - 1000.0;
        double v = (double)(int64_t)(next_u64() % 2000001) / 7.0 - 100000.0;
        check(v, small);
        check(v, small / 3.0);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fmod_issue_example);
    RUN_TEST(test_fmod_special_values);
    RUN_TEST(test_fmod_random_bits);
    RUN_TEST(test_fmod_random_near);
    printf("checked %d pairs\n", g_checked);
    return UNITY_END();
}
