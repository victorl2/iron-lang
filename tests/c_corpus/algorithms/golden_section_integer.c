/*
 * title: Golden-section and Fibonacci interval reduction
 * topic: algorithms
 * covers: golden section search, Fibonacci search for extrema, evaluation reuse, evaluation counts vs ternary
 * deps: libc, libm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

static long evals;

static double f2(double x) {
    evals++;
    return x * x - 4 * x + exp(-x);
}
static double f3(double x) {
    evals++;
    return fabs(x - 0.25) + 0.1 * x * x;
}

/* golden-section: one new evaluation per iteration */
static double golden_min(double (*f)(double), double a, double b, double tol) {
    const double invphi = (sqrt(5.0) - 1.0) / 2.0;
    double c = b - invphi * (b - a), d = a + invphi * (b - a);
    double fc = f(c), fd = f(d);
    while (b - a > tol) {
        if (fc < fd) {
            b = d;
            d = c;
            fd = fc;
            c = b - invphi * (b - a);
            fc = f(c);
        } else {
            a = c;
            c = d;
            fc = fd;
            d = a + invphi * (b - a);
            fd = f(d);
        }
    }
    return 0.5 * (a + b);
}

/* ternary for comparison: two evaluations per iteration */
static double ternary_min(double (*f)(double), double a, double b, double tol) {
    while (b - a > tol) {
        double m1 = a + (b - a) / 3, m2 = b - (b - a) / 3;
        if (f(m1) < f(m2))
            b = m2;
        else
            a = m1;
    }
    return 0.5 * (a + b);
}

/* Fibonacci search over integers: minimise integer unimodal g on [0,n) */
static long g_cost(long x) {
    evals++;
    return (x - 617) * (x - 617) / 3;
}

static long fib_min_int(long n, long *value) {
    long fib[64];
    int k = 2;
    fib[0] = 1;
    fib[1] = 1;
    while (fib[k - 1] < n + 1) {
        fib[k] = fib[k - 1] + fib[k - 2];
        k++;
    }
    long lo = 0;
    int m = k - 1;
    /* interval [lo, lo+fib[m]] treated as padded with +inf beyond n-1 */
    while (m > 2) {
        long x1 = lo + fib[m - 2], x2 = lo + fib[m - 1];
        long v1 = x1 < n ? g_cost(x1) : (1L << 60);
        long v2 = x2 < n ? g_cost(x2) : (1L << 60);
        if (v1 > v2)
            lo = x1; /* minimum lies in [x1, lo+fib[m]] */
        m--;         /* either way the interval shrinks to fib[m-1] */
    }
    long best = lo, bv = g_cost(lo);
    for (long i = lo; i <= lo + 3 && i < n; i++) {
        long v = g_cost(i);
        if (v < bv) {
            bv = v;
            best = i;
        }
    }
    *value = bv;
    return best;
}

int main(void) {
    struct {
        const char *name;
        double (*f)(double);
        double a, b;
    } fs[] = {{"x^2-4x+e^-x", f2, -2, 6}, {"|x-.25|+.1x^2", f3, -5, 5}};
    for (int i = 0; i < 2; i++) {
        evals = 0;
        double g = golden_min(fs[i].f, fs[i].a, fs[i].b, 1e-8);
        long eg = evals;
        evals = 0;
        double t = ternary_min(fs[i].f, fs[i].a, fs[i].b, 1e-8);
        long et = evals;
        if (fabs(g - t) > 1e-6)
            fail("golden vs ternary");
        printf("%-14s argmin=%.5f golden evals=%ld ternary evals=%ld\n", fs[i].name, g, eg, et);
        if (eg >= et)
            fail("golden should use fewer evaluations");
    }

    /* golden-section on integers via brute-force verification */
    long best = 0, bv = 617 * 617 / 3;
    for (long x = 0; x < 2000; x++) {
        long v = (x - 617) * (x - 617) / 3;
        if (v < bv) {
            bv = v;
            best = x;
        }
    }
    long v;
    evals = 0;
    long r = fib_min_int(2000, &v);
    printf("fibonacci integer search: x=%ld cost=%ld (brute x=%ld cost=%ld) evals<200: %s\n", r,
           v, best, bv, evals < 200 ? "yes" : "no");
    if (v != bv)
        fail("fibonacci integer min");
    return 0;
}
