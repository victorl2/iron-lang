/*
 * title: Double modulus rolling hash queries
 * topic: algorithms
 * covers: polynomial rolling hash, two moduli, substring equality, LCP by binary search, distinct substrings of fixed length
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 0x9e3779b97f4a7c15ULL;

static inline unsigned rnd(void) {
    unsigned long long z = (rng_s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (unsigned)((z ^ (z >> 31)) >> 16);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline void rand_str(char *s, int n, int alpha) {
    for (int i = 0; i < n; i++)
        s[i] = (char)('a' + rnd() % (unsigned)alpha);
    s[n] = 0;
}

typedef unsigned long long u64;

#define M1 1000000007ULL
#define M2 998244353ULL

typedef struct {
    int n;
    u64 *h1, *h2, *p1, *p2;
} Hasher;

static void hasher_init(Hasher *H, const char *s, int n) {
    const u64 B1 = 911382323ULL, B2 = 972663749ULL;
    H->n = n;
    H->h1 = malloc(sizeof(u64) * (size_t)(n + 1));
    H->h2 = malloc(sizeof(u64) * (size_t)(n + 1));
    H->p1 = malloc(sizeof(u64) * (size_t)(n + 1));
    H->p2 = malloc(sizeof(u64) * (size_t)(n + 1));
    H->h1[0] = H->h2[0] = 0;
    H->p1[0] = H->p2[0] = 1;
    for (int i = 0; i < n; i++) {
        u64 c = (unsigned char)s[i];
        H->h1[i + 1] = (H->h1[i] * B1 + c) % M1;
        H->h2[i + 1] = (H->h2[i] * B2 + c) % M2;
        H->p1[i + 1] = H->p1[i] * B1 % M1;
        H->p2[i + 1] = H->p2[i] * B2 % M2;
    }
}

static void hasher_free(Hasher *H) {
    free(H->h1);
    free(H->h2);
    free(H->p1);
    free(H->p2);
}

/* hash of [l, r) combined into one 64-bit key */
static u64 sub_hash(const Hasher *H, int l, int r) {
    u64 a = (H->h1[r] + M1 * M1 - H->h1[l] * H->p1[r - l]) % M1;
    u64 b = (H->h2[r] + M2 * M2 - H->h2[l] * H->p2[r - l]) % M2;
    return a << 32 | b;
}

static int lcp_hash(const Hasher *H, int i, int j) {
    int lo = 0, hi = H->n - (i > j ? i : j);
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (sub_hash(H, i, i + mid) == sub_hash(H, j, j + mid))
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static int lcp_naive(const char *s, int n, int i, int j) {
    int k = 0;
    while (i + k < n && j + k < n && s[i + k] == s[j + k])
        k++;
    return k;
}

static int cmp_u64(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

static int count_distinct_len(const Hasher *H, int n, int k) {
    int cnt = n - k + 1;
    u64 *v = malloc(sizeof(u64) * (size_t)cnt);
    for (int i = 0; i < cnt; i++)
        v[i] = sub_hash(H, i, i + k);
    qsort(v, (size_t)cnt, sizeof(u64), cmp_u64);
    int d = 1;
    for (int i = 1; i < cnt; i++)
        d += v[i] != v[i - 1];
    free(v);
    return d;
}

int main(void) {
    enum { N = 3000 };
    static char s[N + 1];
    rand_str(s, N, 2);
    /* make it repetitive so long LCPs occur */
    memcpy(s + 1500, s + 200, 400);
    Hasher H;
    hasher_init(&H, s, N);

    int eq_ok = 0, eq_total = 0;
    for (int q = 0; q < 3000; q++) {
        int len = 1 + (int)(rnd() % 12);
        int i = (int)(rnd() % (unsigned)(N - len)), j = (int)(rnd() % (unsigned)(N - len));
        int same = memcmp(s + i, s + j, (size_t)len) == 0;
        check(same == (sub_hash(&H, i, i + len) == sub_hash(&H, j, j + len)), "substring equality");
        eq_total += same;
        eq_ok++;
    }
    printf("equality queries: %d, equal pairs: %d\n", eq_ok, eq_total);

    long lsum = 0;
    int lmax = 0;
    for (int q = 0; q < 2000; q++) {
        int i = (int)(rnd() % N), j = (int)(rnd() % N);
        int a = lcp_hash(&H, i, j), b = lcp_naive(s, N, i, j);
        check(a == b, "lcp binary search vs naive");
        lsum += a;
        if (a > lmax)
            lmax = a;
    }
    int planted = lcp_hash(&H, 200, 1500);
    check(planted >= 400, "planted repeat");
    printf("random lcp sum=%ld max=%d, planted lcp(200,1500)=%d\n", lsum, lmax, planted);

    for (int k = 1; k <= 16; k *= 2) {
        int d = count_distinct_len(&H, N, k);
        printf("distinct substrings of length %2d: %d\n", k, d);
        check(d <= (1 << k) && d <= N - k + 1, "distinct bound");
    }
    hasher_free(&H);
    return 0;
}
