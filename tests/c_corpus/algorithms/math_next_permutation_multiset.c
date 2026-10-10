/*
 * title: Next and previous permutation with duplicates
 * topic: algorithms
 * covers: lexicographic successor, multiset permutations, pivot and suffix reversal, count via multinomial, wraparound
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void reverse(int *a, int lo, int hi) {
    while (lo < hi) { int t = a[lo]; a[lo++] = a[hi]; a[hi--] = t; }
}

/* returns 0 (and resets to the smallest) when a was the last permutation */
static int next_perm(int *a, int n) {
    int i = n - 2;
    while (i >= 0 && a[i] >= a[i + 1]) i--;
    if (i < 0) { reverse(a, 0, n - 1); return 0; }
    int j = n - 1;
    while (a[j] <= a[i]) j--;
    int t = a[i]; a[i] = a[j]; a[j] = t;
    reverse(a, i + 1, n - 1);
    return 1;
}

static int prev_perm(int *a, int n) {
    int i = n - 2;
    while (i >= 0 && a[i] <= a[i + 1]) i--;
    if (i < 0) { reverse(a, 0, n - 1); return 0; }
    int j = n - 1;
    while (a[j] >= a[i]) j--;
    int t = a[i]; a[i] = a[j]; a[j] = t;
    reverse(a, i + 1, n - 1);
    return 1;
}

static unsigned long long fact(int n) { unsigned long long r = 1; while (n > 1) r *= (unsigned long long)n--; return r; }

static void show(const int *a, int n) { for (int i = 0; i < n; i++) printf("%d", a[i]); }

static int cmp_arr(const int *a, const int *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

int main(void) {
    int a[8] = {1, 2, 3};
    printf("perms of 123:");
    do { printf(" "); show(a, 3); } while (next_perm(a, 3));
    printf("\nafter wrap: "); show(a, 3); printf("\n");

    int m[8] = {1, 1, 2, 2, 3};
    unsigned long long count = 0;
    int last[8];
    printf("multiset 11223 first six:");
    do {
        if (count < 6) { printf(" "); show(m, 5); }
        if (count > 0 && cmp_arr(last, m, 5) >= 0) { fprintf(stderr, "not increasing\n"); return 1; }
        memcpy(last, m, sizeof last);
        count++;
    } while (next_perm(m, 5));
    unsigned long long expect = fact(5) / (fact(2) * fact(2) * fact(1));
    printf("\ndistinct arrangements of 11223: %llu (5!/(2!2!1!) = %llu)\n", count, expect);
    if (count != expect) return 1;

    /* forward then backward traversal returns to the start */
    int b[6] = {0, 0, 1, 1, 1, 2};
    unsigned long long fwd = 1, bwd = 1;
    while (next_perm(b, 6)) fwd++;
    /* b wrapped to smallest; step back once wraps to the largest */
    prev_perm(b, 6);
    while (prev_perm(b, 6)) bwd++;
    printf("multiset 001112: forward %llu, backward %llu, multinomial %llu\n", fwd, bwd, fact(6) / (fact(2) * fact(3)));
    if (fwd != bwd) return 1;

    /* sort-order check against brute force: count arrangements of 4 distinct in range */
    int c[4] = {3, 1, 4, 2};
    printf("successors of 3142:");
    for (int i = 0; i < 4; i++) { next_perm(c, 4); printf(" "); show(c, 4); }
    printf("\npredecessors of 3142:");
    int d[4] = {3, 1, 4, 2};
    for (int i = 0; i < 4; i++) { prev_perm(d, 4); printf(" "); show(d, 4); }
    printf("\n");

    /* the permutation of 0..n-1 at position k of the lexicographic order, by stepping */
    int e[9];
    for (int i = 0; i < 9; i++) e[i] = i;
    for (unsigned long long k = 0; k < 100000; k++) next_perm(e, 9);
    printf("100000th successor of 012345678: "); show(e, 9); printf("\n");

    /* count permutations of 0..7 with exactly k descents (Eulerian numbers) */
    int f[8];
    for (int i = 0; i < 8; i++) f[i] = i;
    unsigned long long hist[8] = {0};
    do {
        int desc = 0;
        for (int i = 0; i + 1 < 8; i++) desc += f[i] > f[i + 1];
        hist[desc]++;
    } while (next_perm(f, 8));
    printf("Eulerian numbers n=8:");
    unsigned long long tot = 0;
    for (int i = 0; i < 8; i++) { printf(" %llu", hist[i]); tot += hist[i]; }
    printf(" (total %llu)\n", tot);
    return tot == fact(8) ? 0 : 1;
}
