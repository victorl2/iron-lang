/*
 * title: Sum over subsets (SOS) DP and Mobius inversion
 * topic: algorithms
 * covers: dynamic programming, bitmask, sum over subsets, superset sums, Mobius inversion, subset convolution counts, AND-count via superset zeta, brute force
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

typedef long long ll;
#define B 10
#define M (1 << B)

int main(void) {
    for (int t = 0; t < 4; t++) {
        int bits = 6 + t * 1;
        int size = 1 << bits;
        static ll f[M], sub[M], sup[M], back[M];
        for (int m = 0; m < size; m++) f[m] = rr(100) - 30;
        memcpy(sub, f, sizeof f);
        memcpy(sup, f, sizeof f);
        for (int b = 0; b < bits; b++)
            for (int m = 0; m < size; m++) {
                if (m >> b & 1) sub[m] += sub[m ^ (1 << b)];
                else sup[m] += sup[m | (1 << b)];
            }
        /* brute */
        for (int m = 0; m < size; m += 7) {
            ll a = 0, c = 0;
            for (int x = 0; x < size; x++) {
                if ((x & m) == x) a += f[x];
                if ((x & m) == m) c += f[x];
            }
            CHECK(a == sub[m] && c == sup[m]);
        }
        /* Mobius inversion recovers f */
        memcpy(back, sub, sizeof sub);
        for (int b = 0; b < bits; b++)
            for (int m = 0; m < size; m++) if (m >> b & 1) back[m] -= back[m ^ (1 << b)];
        for (int m = 0; m < size; m++) CHECK(back[m] == f[m]);
        ll checksum = 0;
        for (int m = 0; m < size; m++) checksum += (ll)(m + 1) * sub[m] - sup[m];
        printf("bits=%d sub[full]=%lld sup[0]=%lld sub[5]=%lld sup[5]=%lld checksum=%lld\n", bits, sub[size - 1], sup[0],
               sub[5], sup[5], checksum);
    }
    /* count pairs (i,j) with a[i] & a[j] == 0 using subset sums over complements */
    int n = 300, bits = 8, size = 1 << bits;
    int a[300];
    static ll cnt[M];
    for (int i = 0; i < n; i++) { a[i] = rr(size); cnt[a[i]]++; }
    for (int b = 0; b < bits; b++)
        for (int m = 0; m < size; m++) if (m >> b & 1) cnt[m] += cnt[m ^ (1 << b)];
    ll pairs = 0, slow = 0;
    for (int i = 0; i < n; i++) pairs += cnt[(size - 1) ^ a[i]];
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) if ((a[i] & a[j]) == 0) slow++;
    CHECK(pairs == slow);
    printf("ordered pairs with zero AND among %d values: %lld\n", n, pairs);
    return 0;
}
