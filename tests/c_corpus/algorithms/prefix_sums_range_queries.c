/*
 * title: Prefix sums, xor and counts for range queries
 * topic: algorithms
 * covers: prefix arrays, O(1) range sum, prefix xor, per-value prefix counts, unsigned wraparound, brute-force checking
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned long long rs = 0x123456789ABCDEFull;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 20);
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

enum { N = 3000, K = 5 };

static long long psum[N + 1];
static unsigned pxor[N + 1];
static unsigned pcnt[K][N + 1]; /* pcnt[v][i] = occurrences of v in a[0..i) */
static unsigned long long pmix[N + 1]; /* wraps modulo 2^64 by design */

int main(void) {
    static int a[N];
    for (int i = 0; i < N; i++)
        a[i] = (int)(rnd() % 2001) - 1000;
    static int cls[N];
    for (int i = 0; i < N; i++)
        cls[i] = (int)(rnd() % K);

    for (int i = 0; i < N; i++) {
        psum[i + 1] = psum[i] + a[i];
        pxor[i + 1] = pxor[i] ^ (unsigned)a[i];
        pmix[i + 1] = pmix[i] * 1000003ull + (unsigned long long)(unsigned)a[i];
        for (int v = 0; v < K; v++)
            pcnt[v][i + 1] = pcnt[v][i] + (cls[i] == v);
    }

    /* range hash via prefix polynomial: hash(l,r) = pmix[r] - pmix[l]*B^(r-l) */
    static unsigned long long pw[N + 1];
    pw[0] = 1;
    for (int i = 1; i <= N; i++)
        pw[i] = pw[i - 1] * 1000003ull;

    long long chk_sum = 0;
    unsigned chk_xor = 0;
    unsigned long long chk_hash = 0;
    for (int q = 0; q < 4000; q++) {
        int l = (int)(rnd() % N), r = (int)(rnd() % N);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        r++; /* half-open [l, r) */
        long long s = 0;
        unsigned x = 0;
        unsigned long long h = 0;
        unsigned c[K] = {0};
        for (int i = l; i < r; i++) {
            s += a[i];
            x ^= (unsigned)a[i];
            h = h * 1000003ull + (unsigned long long)(unsigned)a[i];
            c[cls[i]]++;
        }
        if (psum[r] - psum[l] != s)
            fail("sum");
        if ((pxor[r] ^ pxor[l]) != x)
            fail("xor");
        if (pmix[r] - pmix[l] * pw[r - l] != h)
            fail("hash");
        for (int v = 0; v < K; v++)
            if (pcnt[v][r] - pcnt[v][l] != c[v])
                fail("count");
        chk_sum += s;
        chk_xor ^= x;
        chk_hash ^= h;
    }
    printf("total of 4000 range sums: %lld\n", chk_sum);
    printf("xor of range xors: %08x\n", chk_xor);
    printf("xor of range hashes: %016llx\n", chk_hash);
    printf("whole-array sum %lld, class counts", psum[N]);
    for (int v = 0; v < K; v++)
        printf(" %u", pcnt[v][N]);
    printf("\n");
    return 0;
}
