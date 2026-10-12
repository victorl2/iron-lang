/*
 * title: Bisection on real numbers
 * topic: algorithms
 * covers: floating-point binary search, iteration bounds, sqrt/cbrt by bisection, root of monotone function, function pointers
 * deps: libc, libm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

typedef double (*Fn)(double);

/* root of f in [lo,hi] where f(lo) and f(hi) have opposite signs; fixed iteration count */
static double bisect(Fn f, double lo, double hi, int *iters) {
    double flo = f(lo);
    int i;
    for (i = 0; i < 100 && hi - lo > 1e-12; i++) {
        double mid = 0.5 * (lo + hi);
        double fm = f(mid);
        if ((fm < 0) == (flo < 0)) {
            lo = mid;
            flo = fm;
        } else
            hi = mid;
    }
    *iters = i;
    return 0.5 * (lo + hi);
}

static double bis_sqrt(double x) {
    double lo = 0, hi = x < 1 ? 1 : x;
    for (int i = 0; i < 100; i++) {
        double mid = 0.5 * (lo + hi);
        if (mid * mid < x)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

static double bis_cbrt(double x) {
    double lo = 0, hi = x < 1 ? 1 : x;
    for (int i = 0; i < 120; i++) {
        double mid = 0.5 * (lo + hi);
        if (mid * mid * mid < x)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

static double f_cubic(double x) { return x * x * x - 2 * x - 5; }
static double f_cos(double x) { return cos(x) - x; }
static double f_exp(double x) { return exp(x) + x - 4; }
static double f_log(double x) { return log(x) * x - 3; }

/* largest integer floor(sqrt(n)) by integer bisection for comparison */
static unsigned long isqrt_bs(unsigned long n) {
    unsigned long lo = 0, hi = n < 4294967295ul ? n : 4294967295ul;
    while (lo < hi) {
        unsigned long mid = lo + (hi - lo + 1) / 2;
        if (mid * mid <= n)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

int main(void) {
    double xs[] = {2, 10, 0.25, 1e6, 12345.678, 1};
    for (int i = 0; i < 6; i++) {
        double s = bis_sqrt(xs[i]), c = bis_cbrt(xs[i]);
        if (fabs(s - sqrt(xs[i])) > 1e-9 * (1 + s))
            fail("sqrt");
        if (fabs(c - cbrt(xs[i])) > 1e-9 * (1 + c))
            fail("cbrt");
        printf("x=%-10g sqrt=%.6f cbrt=%.6f\n", xs[i], s, c);
    }

    struct {
        const char *name;
        Fn f;
        double lo, hi;
    } tests[] = {
        {"x^3-2x-5", f_cubic, 2, 3},
        {"cos(x)-x", f_cos, 0, 1},
        {"e^x+x-4", f_exp, 0, 2},
        {"x ln x-3", f_log, 1, 5},
    };
    for (int i = 0; i < 4; i++) {
        int it;
        double r = bisect(tests[i].f, tests[i].lo, tests[i].hi, &it);
        if (fabs(tests[i].f(r)) > 1e-8)
            fail("residual");
        printf("root of %-9s = %.6f (iterations within bound: %s)\n", tests[i].name, r,
               it <= 45 ? "yes" : "no");
    }

    unsigned long ns[] = {0, 1, 2, 15, 16, 17, 99999999ul, 4294967295ul, 1000000007ul};
    for (int i = 0; i < 9; i++) {
        unsigned long r = isqrt_bs(ns[i]);
        if (r * r > ns[i] || (r + 1) * (r + 1) <= ns[i])
            fail("isqrt");
        printf("isqrt(%lu)=%lu\n", ns[i], r);
    }
    return 0;
}
