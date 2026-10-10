/*
 * title: Type punning through memcpy and unions for float layouts
 * topic: memory
 * covers: IEEE 754 fields, memcpy punning, ulp neighbours, sign-magnitude ordering, unions
 * deps: libc, libm
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t f2u(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static float u2f(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static uint64_t d2u(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

static double u2d(uint64_t u) {
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

typedef union {
    float f;
    uint32_t u;
    unsigned char b[4];
} FloatBits;

/* order-preserving map from float bit patterns to unsigned ints */
static uint32_t ordered(float f) {
    uint32_t u = f2u(f);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

static float next_up(float f) {
    uint32_t u = f2u(f);
    if (u == 0x80000000u)
        return u2f(1);
    return u2f((u & 0x80000000u) ? u - 1 : u + 1);
}

static void describe(const char *label, float f) {
    uint32_t u = f2u(f);
    unsigned sign = u >> 31, exp = (u >> 23) & 0xFFu, frac = u & 0x7FFFFFu;
    const char *cls = exp == 0xFF ? (frac ? "nan" : "inf") : exp == 0 ? (frac ? "subnormal" : "zero") : "normal";
    printf("%-10s bits=0x%08x sign=%u exp=%3u frac=0x%06x %s\n", label, (unsigned)u, sign, exp, frac, cls);
}

/* fast reciprocal-square-root seed by bit manipulation, then Newton steps */
static float rsqrt(float x) {
    uint32_t i = f2u(x);
    i = 0x5F3759DFu - (i >> 1);
    float y = u2f(i);
    for (int k = 0; k < 3; k++)
        y = y * (1.5f - 0.5f * x * y * y);
    return y;
}

int main(void) {
    check(sizeof(float) == 4 && sizeof(double) == 8 && sizeof(FloatBits) == 4, "sizes");
    describe("1.0", 1.0f);
    describe("-2.5", -2.5f);
    describe("0.1", 0.1f);
    describe("+0", 0.0f);
    describe("min-sub", u2f(1));
    describe("max", u2f(0x7F7FFFFFu));
    describe("+inf", u2f(0x7F800000u));
    describe("-inf", u2f(0xFF800000u));

    /* union and memcpy views agree */
    FloatBits fb;
    fb.f = 6.5f;
    check(fb.u == f2u(6.5f), "union view");
    printf("6.5f = 0x%08x\n", (unsigned)fb.u);
    /* the byte view is a permutation of the integer's bytes, whatever the host order */
    unsigned bsum = 0, usum = 0;
    for (int i = 0; i < 4; i++) {
        bsum += fb.b[i];
        usum += (fb.u >> (8 * i)) & 0xFFu;
    }
    check(bsum == usum, "byte view is a permutation");

    printf("1.0 double = 0x%016llx\n", (unsigned long long)d2u(1.0));
    printf("-0.5 double = 0x%016llx\n", (unsigned long long)d2u(-0.5));
    check(u2d(0x3FF0000000000000ull) == 1.0, "double one");
    check(u2d(d2u(3.14159)) == 3.14159, "double round trip");

    /* sign-flip via the sign bit, and abs via masking */
    float v = 3.75f;
    check(u2f(f2u(v) ^ 0x80000000u) == -v, "negate by xor");
    check(u2f(f2u(-v) & 0x7FFFFFFFu) == v, "abs by mask");
    printf("negate/abs via bits ok\n");

    /* ordering: the mapped integers sort the same way as the floats */
    float vals[] = {-100.5f, -1.0f, -0.25f, -0.0f, 0.0f, 0.25f, 1.0f, 2.0f, 1000.0f};
    for (size_t i = 0; i + 1 < sizeof vals / sizeof vals[0]; i++)
        check(ordered(vals[i]) <= ordered(vals[i + 1]), "order preserving");
    printf("ordering map monotone over %zu values\n", sizeof vals / sizeof vals[0]);

    /* ulp neighbours */
    float a = 1.0f;
    float b = next_up(a);
    printf("next_up(1.0) bits=0x%08x delta bits=%u\n", (unsigned)f2u(b), (unsigned)(f2u(b) - f2u(a)));
    check(b > a && next_up(next_up(a)) > b, "increasing");
    check(next_up(-0.0f) == u2f(1) && next_up(u2f(0x80000001u)) == 0.0f, "cross zero");
    check(nextafterf(1.0f, 2.0f) == b, "matches libm nextafterf");
    /* ulp size doubles per binade */
    float e1 = next_up(2.0f) - 2.0f, e2 = next_up(4.0f) - 4.0f;
    check(e2 == 2.0f * e1, "ulp scales");
    printf("ulp(2)=2^-22 ulp(4)=2^-21: %s\n", (e1 == ldexpf(1.0f, -22) && e2 == ldexpf(1.0f, -21)) ? "yes" : "no");

    /* count representable floats between two values by subtracting ordered bits */
    printf("floats in [1,2): %u\n", (unsigned)(ordered(2.0f) - ordered(1.0f)));
    printf("floats in [0,1): %u\n", (unsigned)(ordered(1.0f) - ordered(0.0f)));

    /* bit-trick rsqrt versus libm, to four digits */
    float xs[] = {0.25f, 1.0f, 2.0f, 9.0f, 100.0f};
    for (size_t i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        float r = rsqrt(xs[i]);
        float ref = 1.0f / sqrtf(xs[i]);
        printf("rsqrt(%g) = %.4f\n", (double)xs[i], (double)r);
        check(fabsf(r - ref) < 1e-5f * (ref > 1 ? ref : 1.0f), "rsqrt accurate");
    }
    return 0;
}
