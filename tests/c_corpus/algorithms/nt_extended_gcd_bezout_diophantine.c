/*
 * title: Extended gcd, Bezout and linear Diophantine equations
 * topic: algorithms
 * covers: extended Euclid, Bezout coefficients, linear Diophantine solutions, non-negative solution counting, coin problem
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef long long i64;

static i64 egcd(i64 a, i64 b, i64 *x, i64 *y) {
    if (b == 0) { *x = (a >= 0) ? 1 : -1; *y = 0; return a >= 0 ? a : -a; }
    i64 x1, y1;
    i64 g = egcd(b, a % b, &x1, &y1);
    *x = y1;
    *y = x1 - (a / b) * y1;
    return g;
}

static i64 floordiv(i64 a, i64 b) {
    i64 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
static i64 ceildiv(i64 a, i64 b) { return -floordiv(-a, b); }

/* Solve a x + b y = c. Returns 0 if none. Base solution in (x0,y0); step is (b/g, -a/g). */
static int diophantine(i64 a, i64 b, i64 c, i64 *x0, i64 *y0, i64 *dx, i64 *dy) {
    i64 x, y, g = egcd(a, b, &x, &y);
    if (g == 0 || c % g) return 0;
    *x0 = x * (c / g);
    *y0 = y * (c / g);
    *dx = b / g;
    *dy = -a / g;
    return 1;
}

/* count solutions with x,y >= 0, a,b > 0 */
static i64 count_nonneg(i64 a, i64 b, i64 c) {
    i64 x0, y0, dx, dy;
    if (!diophantine(a, b, c, &x0, &y0, &dx, &dy)) return 0;
    /* x = x0 + t dx >= 0 ; y = y0 + t dy >= 0 with dx > 0, dy < 0 */
    i64 tlo = ceildiv(-x0, dx);
    i64 thi = floordiv(y0, -dy);
    return thi >= tlo ? thi - tlo + 1 : 0;
}

int main(void) {
    i64 pairs[][2] = {{240, 46}, {17, 5}, {0, 9}, {-12, 18}, {35, -14}, {1000000007, 998244353}};
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        i64 x, y, g = egcd(pairs[i][0], pairs[i][1], &x, &y);
        if (pairs[i][0] * x + pairs[i][1] * y != g) { fprintf(stderr, "bezout fails\n"); return 1; }
        printf("gcd(%lld,%lld)=%lld  x=%lld y=%lld\n", pairs[i][0], pairs[i][1], g, x, y);
    }
    /* Diophantine */
    struct { i64 a, b, c; } eq[] = {{6, 15, 9}, {6, 15, 10}, {12, -18, 30}, {21, 14, 7}, {1000, 999, 1}};
    for (size_t i = 0; i < sizeof eq / sizeof eq[0]; i++) {
        i64 x0, y0, dx, dy;
        if (!diophantine(eq[i].a, eq[i].b, eq[i].c, &x0, &y0, &dx, &dy)) {
            printf("%lldx + %lldy = %lld: no solution\n", eq[i].a, eq[i].b, eq[i].c);
            continue;
        }
        for (i64 t = -3; t <= 3; t++) {
            if (eq[i].a * (x0 + t * dx) + eq[i].b * (y0 + t * dy) != eq[i].c) {
                fprintf(stderr, "family broken\n");
                return 1;
            }
        }
        printf("%lldx + %lldy = %lld: (%lld,%lld) + t(%lld,%lld)\n", eq[i].a, eq[i].b, eq[i].c, x0, y0, dx, dy);
    }
    /* non-negative counts vs brute force */
    i64 tests[][3] = {{3, 5, 100}, {7, 11, 1000}, {6, 9, 100}, {4, 6, 101}, {13, 17, 2000}};
    for (size_t i = 0; i < 5; i++) {
        i64 a = tests[i][0], b = tests[i][1], c = tests[i][2], brute = 0;
        for (i64 x = 0; a * x <= c; x++) if ((c - a * x) % b == 0) brute++;
        i64 fast = count_nonneg(a, b, c);
        if (brute != fast) { fprintf(stderr, "count mismatch %lld vs %lld\n", brute, fast); return 1; }
        printf("%lldx + %lldy = %lld has %lld non-negative solutions\n", a, b, c, fast);
    }
    /* Frobenius number for coprime a,b is ab - a - b: largest c with no solution */
    i64 a = 7, b = 12, frob = -1;
    for (i64 c = 0; c < 200; c++) if (count_nonneg(a, b, c) == 0) frob = c;
    printf("largest unreachable amount with coins %lld,%lld: %lld (formula %lld)\n", a, b, frob, a * b - a - b);
    if (frob != a * b - a - b) return 1;
    return 0;
}
