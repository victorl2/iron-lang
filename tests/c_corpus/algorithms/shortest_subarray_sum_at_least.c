/*
 * title: Shortest subarray with sum at least K
 * topic: algorithms
 * covers: prefix sums, monotonic deque of prefix indices, positive-only two-pointer variant, negative numbers, 64-bit sums
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 1010101u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

/* works with negative values: deque of increasing prefix sums */
static int shortest_general(const int *a, int n, long k) {
    long *p = malloc((size_t)(n + 1) * sizeof(long));
    int *dq = malloc((size_t)(n + 1) * sizeof(int));
    if (!p || !dq)
        fail("alloc");
    p[0] = 0;
    for (int i = 0; i < n; i++)
        p[i + 1] = p[i] + a[i];
    int head = 0, tail = 0, best = -1;
    for (int i = 0; i <= n; i++) {
        while (tail > head && p[i] - p[dq[head]] >= k) {
            int len = i - dq[head++];
            if (best < 0 || len < best)
                best = len;
        }
        while (tail > head && p[dq[tail - 1]] >= p[i])
            tail--;
        dq[tail++] = i;
    }
    free(p);
    free(dq);
    return best;
}

/* positive-only: classic shrinking window */
static int shortest_positive(const int *a, int n, long k) {
    long sum = 0;
    int left = 0, best = -1;
    for (int r = 0; r < n; r++) {
        sum += a[r];
        while (sum >= k && left <= r) {
            if (best < 0 || r - left + 1 < best)
                best = r - left + 1;
            sum -= a[left++];
        }
    }
    return best;
}

static int brute(const int *a, int n, long k) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        long s = 0;
        for (int j = i; j < n; j++) {
            s += a[j];
            if (s >= k) {
                if (best < 0 || j - i + 1 < best)
                    best = j - i + 1;
                break;
            }
        }
    }
    return best;
}

int main(void) {
    int a[200];
    long ck = 0;
    for (int trial = 0; trial < 500; trial++) {
        int n = 1 + (int)(rnd() % 150);
        int positive = trial % 2;
        for (int i = 0; i < n; i++)
            a[i] = positive ? 1 + (int)(rnd() % 20) : (int)(rnd() % 41) - 15;
        long k = (long)(rnd() % 200) + 1;
        int want = brute(a, n, k);
        int g = shortest_general(a, n, k);
        if (g != want)
            fail("general");
        if (positive && shortest_positive(a, n, k) != want)
            fail("positive");
        ck += g;
    }
    printf("500 trials verified, length checksum %ld\n", ck);

    int d1[] = {2, -1, 2};
    int d2[] = {1, 2};
    int d3[] = {84, -37, 32, 40, 95};
    printf("[2,-1,2] k=3 -> %d\n", shortest_general(d1, 3, 3));
    printf("[1,2] k=4 -> %d\n", shortest_general(d2, 2, 4));
    printf("[84,-37,32,40,95] k=167 -> %d\n", shortest_general(d3, 5, 167));
    int big[3] = {2000000000, 2000000000, 2000000000};
    printf("64-bit sums k=6e9 -> %d\n", shortest_general(big, 3, 6000000000L));
    return 0;
}
