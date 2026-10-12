/*
 * title: Set partitions by restricted growth strings and Bell numbers
 * topic: algorithms
 * covers: restricted growth strings, Bell triangle, Stirling second kind via surjection inclusion-exclusion, enumeration, block counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long long i64;

static i64 binom[20][20];

static i64 ipow(i64 b, int e) { i64 r = 1; while (e--) r *= b; return r; }

/* S(n,k) = (1/k!) sum_{j} (-1)^j C(k,j) (k-j)^n */
static i64 stirling2_formula(int n, int k) {
    i64 s = 0, f = 1;
    for (int i = 2; i <= k; i++) f *= i;
    for (int j = 0; j <= k; j++) {
        i64 term = binom[k][j] * ipow(k - j, n);
        s += (j & 1) ? -term : term;
    }
    return s / f;
}

static i64 blocks_hist[10][10];
static i64 total;

/* rgs[i] <= 1 + max(rgs[0..i-1]) */
static void gen(int *a, int i, int n, int mx) {
    if (i == n) {
        total++;
        blocks_hist[n][mx + 1]++;
        return;
    }
    for (int v = 0; v <= mx + 1; v++) {
        a[i] = v;
        gen(a, i + 1, n, v > mx ? v : mx);
    }
}

static int valid_rgs(const int *a, int n) {
    int mx = -1;
    for (int i = 0; i < n; i++) {
        if (a[i] > mx + 1 || a[i] < 0) return 0;
        if (a[i] > mx) mx = a[i];
    }
    return 1;
}

static void print_partition(const int *a, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) if (a[i] + 1 > k) k = a[i] + 1;
    printf("{");
    for (int b = 0; b < k; b++) {
        printf(b ? " |" : "");
        for (int i = 0; i < n; i++) if (a[i] == b) printf(" %c", 'a' + i);
    }
    printf(" }");
}

int main(void) {
    for (int i = 0; i < 20; i++) {
        binom[i][0] = 1;
        for (int j = 1; j <= i; j++) binom[i][j] = binom[i - 1][j - 1] + (j < i ? binom[i - 1][j] : 0);
    }
    int a[10];
    printf("partitions of {a,b,c,d} (15):\n");
    {
        int n = 4;
        /* enumerate and print via nested loops over restricted growth strings */
        int cnt = 0;
        for (int x0 = 0; x0 < 1; x0++)
            for (int x1 = 0; x1 <= 1; x1++)
                for (int x2 = 0; x2 <= (x1 > 0 ? x1 : 0) + 1; x2++)
                    for (int x3 = 0; x3 <= 3; x3++) {
                        a[0] = x0; a[1] = x1; a[2] = x2; a[3] = x3;
                        if (!valid_rgs(a, n)) continue;
                        if (cnt % 3 == 0) printf("  "); 
                        print_partition(a, n);
                        cnt++;
                        printf(cnt % 3 == 0 ? "\n" : " ");
                    }
        if (cnt % 3) printf("\n");
        if (cnt != 15) return 1;
    }
    for (int n = 1; n <= 9; n++) {
        total = 0;
        gen(a, 0, n, -1);
        printf("n=%d Bell=%lld stirling2:", n, total);
        i64 sum = 0;
        for (int k = 1; k <= n; k++) {
            i64 s = stirling2_formula(n, k);
            if (s != blocks_hist[n][k]) { fprintf(stderr, "stirling mismatch n=%d k=%d\n", n, k); return 1; }
            printf(" %lld", s);
            sum += s;
        }
        printf("\n");
        if (sum != total) return 1;
    }
    /* Bell triangle gives Bell numbers along the left edge */
    i64 tri[16][16];
    tri[0][0] = 1;
    printf("Bell numbers from triangle:");
    for (int i = 0; i < 15; i++) {
        if (i > 0) {
            tri[i][0] = tri[i - 1][i - 1];
            for (int j = 1; j <= i; j++) tri[i][j] = tri[i][j - 1] + tri[i - 1][j - 1];
        }
        printf(" %lld", tri[i][0]);
    }
    printf("\n");
    /* Bell via Stirling row sums for larger n uses the formula */
    i64 bell15 = 0;
    for (int k = 1; k <= 15; k++) bell15 += stirling2_formula(15, k) ;
    printf("B(15) via formula: %lld, triangle: %lld\n", bell15, tri[14][14]);
    /* Dobinski-free recurrence B(n+1) = sum C(n,k) B(k) */
    i64 bel[16];
    bel[0] = 1;
    for (int n = 0; n < 15; n++) {
        bel[n + 1] = 0;
        for (int k = 0; k <= n; k++) bel[n + 1] += binom[n][k] * bel[k];
    }
    printf("B(15) via binomial recurrence: %lld\n", bel[15]);
    return bel[15] == bell15 && tri[14][14] == bell15 ? 0 : 1;
}
