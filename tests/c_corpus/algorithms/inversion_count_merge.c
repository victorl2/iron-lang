/*
 * title: Counting inversions and Kendall tau distance
 * topic: algorithms
 * covers: merge sort inversion counting, 64-bit counters, Kendall tau distance, Fenwick tree cross-check, brute force verification
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t st = 1357911u;
static uint32_t rng(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static uint64_t count_merge(int *a, int *tmp, int n) {
    if (n < 2)
        return 0;
    int m = n / 2;
    uint64_t inv = count_merge(a, tmp, m) + count_merge(a + m, tmp, n - m);
    int i = 0, j = m, k = 0;
    while (i < m && j < n) {
        if (a[j] < a[i]) {
            inv += (uint64_t)(m - i); /* a[j] is smaller than all remaining left elements */
            tmp[k++] = a[j++];
        } else {
            tmp[k++] = a[i++];
        }
    }
    while (i < m)
        tmp[k++] = a[i++];
    while (j < n)
        tmp[k++] = a[j++];
    memcpy(a, tmp, sizeof(int) * (size_t)n);
    return inv;
}

static uint64_t count_brute(const int *a, int n) {
    uint64_t c = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            c += a[i] > a[j];
    return c;
}

/* Fenwick tree over values 0..m-1 */
static uint64_t count_bit(const int *a, int n, int m) {
    int *bit = calloc((size_t)(unsigned)m + 1, sizeof(int));
    check(bit != NULL, "alloc");
    uint64_t inv = 0;
    for (int i = n - 1; i >= 0; i--) {
        /* count already-seen (to the right) values strictly smaller than a[i] */
        for (int x = a[i]; x > 0; x -= x & -x)
            inv += (uint64_t)bit[x];
        for (int x = a[i] + 1; x <= m; x += x & -x)
            bit[x]++;
    }
    free(bit);
    return inv;
}

/* Kendall tau: discordant pairs between rankings p and q of the same items. */
static uint64_t kendall_tau(const int *p, const int *q, int n) {
    int *pos_q = malloc(sizeof(int) * (size_t)n), *seq = malloc(sizeof(int) * (size_t)n);
    int *tmp = malloc(sizeof(int) * (size_t)n);
    check(pos_q && seq && tmp, "alloc");
    for (int i = 0; i < n; i++)
        pos_q[q[i]] = i;
    for (int i = 0; i < n; i++)
        seq[i] = pos_q[p[i]];
    uint64_t d = count_merge(seq, tmp, n);
    free(pos_q);
    free(seq);
    free(tmp);
    return d;
}

static void shuffle(int *a, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(rng() % (uint32_t)(i + 1));
        int t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

int main(void) {
    static const int sizes[] = {0, 1, 2, 10, 100, 1000, 20000};
    for (int s = 0; s < 7; s++) {
        int n = sizes[s];
        int *a = malloc(sizeof(int) * (size_t)(n + 1)), *b = malloc(sizeof(int) * (size_t)(n + 1));
        int *tmp = malloc(sizeof(int) * (size_t)(n + 1));
        check(a && b && tmp, "alloc");
        for (int i = 0; i < n; i++)
            a[i] = i;
        shuffle(a, n);
        memcpy(b, a, sizeof(int) * (size_t)n);
        uint64_t bit = count_bit(a, n, n ? n : 1);
        uint64_t mg = count_merge(b, tmp, n);
        if (n <= 1000)
            check(count_brute(a, n) == mg, "brute force agrees");
        check(bit == mg, "Fenwick agrees");
        for (int i = 0; i < n; i++)
            check(b[i] == i, "merge left array sorted");
        uint64_t maxinv = (uint64_t)n * (n ? (uint64_t)(n - 1) : 0) / 2;
        printf("n=%-6d inversions=%-10llu of max %-10llu\n", n, (unsigned long long)mg, (unsigned long long)maxinv);
        free(a);
        free(b);
        free(tmp);
    }
    /* Kendall tau on rankings */
    enum { R = 12 };
    int p[R], q[R], rev[R];
    for (int i = 0; i < R; i++)
        p[i] = q[i] = i, rev[i] = R - 1 - i;
    check(kendall_tau(p, q, R) == 0, "identical rankings");
    check(kendall_tau(p, rev, R) == R * (R - 1) / 2, "reversed rankings");
    shuffle(q, R);
    uint64_t d = kendall_tau(p, q, R);
    uint64_t brute = 0;
    for (int i = 0; i < R; i++)
        for (int j = i + 1; j < R; j++) {
            int pi = -1, pj = -1, qi = -1, qj = -1;
            for (int k = 0; k < R; k++) {
                if (p[k] == i) pi = k;
                if (p[k] == j) pj = k;
                if (q[k] == i) qi = k;
                if (q[k] == j) qj = k;
            }
            brute += (pi < pj) != (qi < qj);
        }
    check(d == brute, "kendall tau matches brute force");
    printf("kendall tau of random ranking vs identity: %llu (of %d)\n", (unsigned long long)d, R * (R - 1) / 2);
    return 0;
}
