/*
 * title: Ternary search on unimodal functions
 * topic: algorithms
 * covers: ternary search, integer unimodal maximum, real-valued minimum, plateau hazard, function pointers
 * deps: libc, libm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

typedef long (*IntFn)(long);
typedef double (*RealFn)(double);

static long parabola(long x) { return -(x - 37) * (x - 37) + 1000; }
static long tent(long x) { return x < 250 ? 3 * x : 750 - (x - 250) * 2; }
static long profit(long x) { return x * (100 - x) - (x * x) / 8; }

/* maximum of a strictly unimodal integer function over [lo,hi] */
static long ternary_max(IntFn f, long lo, long hi, long *arg) {
    while (hi - lo > 2) {
        long m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
        if (f(m1) < f(m2))
            lo = m1 + 1;
        else
            hi = m2;
    }
    long best = lo;
    for (long i = lo + 1; i <= hi; i++)
        if (f(i) > f(best))
            best = i;
    *arg = best;
    return f(best);
}

static double ternary_min(RealFn f, double lo, double hi) {
    for (int i = 0; i < 200; i++) {
        double m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
        if (f(m1) < f(m2))
            hi = m2;
        else
            lo = m1;
    }
    return 0.5 * (lo + hi);
}

static double bowl(double x) { return (x - 1.5) * (x - 1.5) + 4; }
static double cosh_shift(double x) { return cosh(x - 0.7) + 0.2 * x; }
static double dist_sum(double x) { /* sum of distances to three points: minimised at the median */
    return fabs(x - 1) + fabs(x - 4) + fabs(x - 10);
}

int main(void) {
    struct {
        const char *name;
        IntFn f;
        long lo, hi;
    } ints[] = {
        {"parabola", parabola, -500, 500},
        {"tent", tent, 0, 500},
        {"profit", profit, 0, 100},
    };
    for (int i = 0; i < 3; i++) {
        long arg;
        long v = ternary_max(ints[i].f, ints[i].lo, ints[i].hi, &arg);
        long bv = ints[i].f(ints[i].lo), ba = ints[i].lo;
        for (long x = ints[i].lo; x <= ints[i].hi; x++)
            if (ints[i].f(x) > bv) {
                bv = ints[i].f(x);
                ba = x;
            }
        if (v != bv || arg != ba)
            fail("integer maximum");
        printf("%-8s max %ld at x=%ld\n", ints[i].name, v, arg);
    }
    double m = ternary_min(bowl, -100, 100);
    printf("bowl min at %.5f value %.5f\n", m, bowl(m));
    if (fabs(m - 1.5) > 1e-6)
        fail("bowl");
    m = ternary_min(cosh_shift, -10, 10);
    /* derivative: sinh(x-0.7)+0.2=0 -> x = 0.7 - asinh(0.2) */
    if (fabs(m - (0.7 - asinh(0.2))) > 1e-6)
        fail("cosh");
    printf("cosh_shift min at %.5f\n", m);
    m = ternary_min(dist_sum, -20, 20);
    printf("median-distance min at %.3f value %.3f\n", m, dist_sum(m));
    if (fabs(m - 4.0) > 1e-3)
        fail("dist");
    return 0;
}
