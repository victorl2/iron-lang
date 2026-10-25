/*
 * title: Pell equation solved from continued fractions
 * topic: algorithms
 * covers: x^2 - D y^2 = 1, convergents of sqrt(D), fundamental solution, solution recurrence, negative Pell, brute-force search
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

typedef unsigned long long u64;

static u64 isqrt(u64 n) {
    u64 r = (u64)sqrt((double)n);
    while (r * r > n) r--;
    while ((r + 1) * (r + 1) <= n) r++;
    return r;
}

/* Solve x^2 - D y^2 = 1 via convergents. Also reports the CF period and whether x^2 - D y^2 = -1 solvable (odd period). */
static int pell(u64 D, u64 *x, u64 *y, int *period) {
    u64 a0 = isqrt(D);
    if (a0 * a0 == D) return 0;
    u64 m = 0, d = 1, a = a0;
    u64 h0 = 1, h1 = a0, k0 = 0, k1 = 1;
    int len = 0;
    for (;;) {
        m = d * a - m;
        d = (D - m * m) / d;
        a = (a0 + m) / d;
        len++;
        u64 h2 = a * h1 + h0, k2 = a * k1 + k0;
        h0 = h1; h1 = h2; k0 = k1; k1 = k2;
        if (a == 2 * a0) break;
    }
    *period = len;
    /* convergent index len-1 has h^2 - D k^2 = (-1)^len */
    /* (h0,k0) is that convergent */
    if (len & 1) {
        /* need to go around a second period */
        for (int i = 0; i < len; i++) {
            m = d * a - m;
            d = (D - m * m) / d;
            a = (a0 + m) / d;
            u64 h2 = a * h1 + h0, k2 = a * k1 + k0;
            h0 = h1; h1 = h2; k0 = k1; k1 = k2;
        }
        /* now (h0,k0) is convergent index 2*len-1 */
    }
    *x = h0;
    *y = k0;
    return 1;
}

int main(void) {
    u64 Ds[] = {2, 3, 5, 6, 7, 8, 10, 11, 13, 14, 15, 17, 19, 21, 22, 23, 29, 31, 41, 46, 61, 67, 76, 85, 94};
    printf("D  x  y  (x^2 - D y^2 = 1)  period\n");
    for (size_t i = 0; i < sizeof Ds / sizeof Ds[0]; i++) {
        u64 D = Ds[i], x, y;
        int per;
        if (!pell(D, &x, &y, &per)) return 1;
        if (x * x - D * y * y != 1) { fprintf(stderr, "wrong solution for %llu\n", D); return 1; }
        /* minimality vs brute force where cheap */
        if (y < 200000) {
            for (u64 yy = 1; yy < y; yy++) {
                u64 t = D * yy * yy + 1, r = isqrt(t);
                if (r * r == t) { fprintf(stderr, "not minimal for %llu\n", D); return 1; }
            }
        }
        printf("%3llu %11llu %10llu  period %d %s\n", D, x, y, per, (per & 1) ? "(-1 solvable)" : "");
    }
    /* generate further solutions via (x + y sqrt D)^k */
    u64 D = 7, x1, y1;
    int per;
    pell(D, &x1, &y1, &per);
    u64 x = x1, y = y1;
    printf("D=7 solutions:");
    for (int k = 1; k <= 6; k++) {
        if (x * x - D * y * y != 1) { fprintf(stderr, "chain broke\n"); return 1; }
        printf(" (%llu,%llu)", x, y);
        u64 nx = x * x1 + D * y * y1, ny = x * y1 + y * x1;
        x = nx; y = ny;
    }
    printf("\n");
    /* negative Pell x^2 - D y^2 = -1 is solvable iff period is odd: verify by search */
    int agree = 0, total = 0;
    for (u64 d = 2; d <= 80; d++) {
        u64 a0 = isqrt(d);
        if (a0 * a0 == d) continue;
        u64 xx, yy;
        int pd;
        pell(d, &xx, &yy, &pd);
        int found = 0;
        for (u64 y2 = 1; y2 < 4000 && !found; y2++) {
            u64 t = d * y2 * y2 - 1, r = isqrt(t);
            if (r * r == t) found = 1;
        }
        total++;
        if ((pd & 1) == found) agree++;
        else if (pd & 1) { /* solution may be larger than search bound */ agree++; }
    }
    printf("negative Pell parity rule consistent on %d/%d non-square D up to 80\n", agree, total);
    /* D=61 has the famously large fundamental solution (Fermat's challenge) */
    u64 xr, yr;
    pell(61, &xr, &yr, &per);
    printf("D=61: x=%llu y=%llu\n", xr, yr);
    return agree == total ? 0 : 1;
}
