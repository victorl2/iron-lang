/*
 * title: Integer partitions: knapsack DP versus Euler pentagonal recurrence
 * topic: algorithms
 * covers: dynamic programming, integer partitions, generalized pentagonal numbers, bounded part counts, distinct parts, Rogers-Ramanujan style identity, big counts in unsigned 64-bit
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 88172645463325252ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

typedef unsigned long long u64;
#define N 120

int main(void) {
    (void)rr;
    static u64 p[N + 1], q[N + 1], distinct[N + 1], odd[N + 1];
    /* knapsack style: parts 1..n, unlimited */
    p[0] = 1;
    for (int part = 1; part <= N; part++)
        for (int s = part; s <= N; s++) p[s] += p[s - part];
    /* Euler: p(n) = sum (-1)^(k+1) [p(n - k(3k-1)/2) + p(n - k(3k+1)/2)] */
    q[0] = 1;
    for (int n = 1; n <= N; n++) {
        long long acc = 0;
        for (int k = 1;; k++) {
            int g1 = k * (3 * k - 1) / 2, g2 = k * (3 * k + 1) / 2;
            if (g1 > n) break;
            long long term = (long long)q[n - g1] + (g2 <= n ? (long long)q[n - g2] : 0);
            acc += (k % 2 ? term : -term);
        }
        q[n] = (u64)acc;
    }
    for (int n = 0; n <= N; n++) CHECK(p[n] == q[n]);
    /* partitions into distinct parts (0/1 knapsack) == partitions into odd parts */
    distinct[0] = 1;
    for (int part = 1; part <= N; part++)
        for (int s = N; s >= part; s--) distinct[s] += distinct[s - part];
    odd[0] = 1;
    for (int part = 1; part <= N; part += 2)
        for (int s = part; s <= N; s++) odd[s] += odd[s - part];
    for (int n = 0; n <= N; n++) CHECK(distinct[n] == odd[n]);
    /* partitions of n into exactly k parts: f(n,k) = f(n-1,k-1) + f(n-k,k) */
    static u64 f[41][41];
    f[0][0] = 1;
    for (int n = 1; n <= 40; n++)
        for (int k = 1; k <= n; k++) f[n][k] = f[n - 1][k - 1] + (n >= k ? f[n - k][k] : 0);
    for (int n = 1; n <= 40; n++) {
        u64 sum = 0;
        for (int k = 1; k <= n; k++) sum += f[n][k];
        CHECK(sum == p[n]);
    }
    /* brute-force enumeration for small n */
    for (int n = 1; n <= 20; n++) {
        int parts[24], np = 1, cnt = 1;
        parts[0] = n;
        /* enumerate partitions in reverse lexicographic order (non-increasing sequences) */
        while (parts[0] > 1) {
            int rem = 0, i = np - 1;
            while (i >= 0 && parts[i] == 1) { rem++; np--; i--; }
            parts[i]--; rem++;
            while (rem > parts[i]) { parts[i + 1] = parts[i]; rem -= parts[i]; i++; np++; }
            parts[i + 1] = rem;
            np = i + 2;
            cnt++;
        }
        CHECK((u64)cnt == p[n]);
    }
    printf("p(n) for n = 0..12:");
    for (int n = 0; n <= 12; n++) printf(" %llu", p[n]);
    printf("\n");
    int marks[] = {20, 50, 80, 100, 120};
    for (int i = 0; i < 5; i++) printf("p(%d)=%llu distinct=%llu\n", marks[i], p[marks[i]], distinct[marks[i]]);
    printf("into exactly 3 parts, n=30: %llu; exactly 5 parts, n=40: %llu\n", f[30][3], f[40][5]);
    return 0;
}
