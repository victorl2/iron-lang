/*
 * title: Universal hash families and collision counts
 * topic: data_structures
 * covers: universal hashing, multiply-shift, Carter-Wegman mod prime, simple tabulation, expected collisions, adversarial keys
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED __attribute__((unused))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static UNUSED uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static UNUSED void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}
static UNUSED uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static UNUSED uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Reference model: unordered array with linear scan. */
enum { REF_CAP = 1 << 14 };
static uint32_t ref_k[REF_CAP];
static int ref_v[REF_CAP];
static int ref_n;
static UNUSED int ref_find(uint32_t k) {
    for (int i = 0; i < ref_n; i++)
        if (ref_k[i] == k)
            return i;
    return -1;
}
static UNUSED int ref_put(uint32_t k, int v) { /* 1 if new */
    int i = ref_find(k);
    if (i >= 0) {
        ref_v[i] = v;
        return 0;
    }
    check(ref_n < REF_CAP, "ref capacity");
    ref_k[ref_n] = k;
    ref_v[ref_n++] = v;
    return 1;
}
static UNUSED int ref_del(uint32_t k) {
    int i = ref_find(k);
    if (i < 0)
        return 0;
    ref_k[i] = ref_k[ref_n - 1];
    ref_v[i] = ref_v[ref_n - 1];
    ref_n--;
    return 1;
}
#define P61 2305843009213693951ull /* 2^61 - 1 */
enum { N = 3000, M_BITS = 12 };

/* (a*b) mod (2^61-1) for a,b < 2^61 using 32-bit limbs (no 128-bit types) */
static uint64_t mulmod61(uint64_t a, uint64_t b) {
    uint64_t a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    uint64_t mid = a1 * b0 + a0 * b1; /* < 2^62 */
    uint64_t lo = a0 * b0;
    uint64_t s = 8 * (a1 * b1) + (mid >> 29) + ((mid & 0x1fffffffu) << 32) + (lo & P61) + (lo >> 61);
    s = (s & P61) + (s >> 61);
    s = (s & P61) + (s >> 61);
    return s >= P61 ? s - P61 : s;
}

typedef struct {
    uint64_t a, b;
    uint32_t tab[4][256];
} Fam;

/* 1. multiply-shift (Dietzfelbinger): h(x) = (a*x) >> (64 - m), a odd */
static uint32_t h_ms(const Fam *f, uint32_t x) { return (uint32_t)((f->a | 1u) * (uint64_t)x >> (64 - M_BITS)); }
/* 2. Carter-Wegman: ((a*x + b) mod p) mod m */
static uint32_t h_cw(const Fam *f, uint32_t x) {
    uint64_t v = mulmod61(f->a % (P61 - 1) + 1, x) + f->b % P61;
    if (v >= P61)
        v -= P61;
    return (uint32_t)(v & ((1u << M_BITS) - 1));
}
/* 3. simple tabulation on 4 bytes */
static uint32_t h_tab(const Fam *f, uint32_t x) {
    uint32_t h = 0;
    for (int i = 0; i < 4; i++)
        h ^= f->tab[i][(x >> (8 * i)) & 255];
    return h & ((1u << M_BITS) - 1);
}
/* 4. non-universal baseline: low bits of the key */
static uint32_t h_low(const Fam *f, uint32_t x) { (void)f; return x & ((1u << M_BITS) - 1); }

static long collisions(uint32_t (*h)(const Fam *, uint32_t), const Fam *f, const uint32_t *keys, int n) {
    static uint16_t cnt[1 << M_BITS];
    memset(cnt, 0, sizeof cnt);
    long pairs = 0;
    for (int i = 0; i < n; i++) {
        uint32_t b = h(f, keys[i]);
        pairs += cnt[b]; /* number of earlier keys in the same bucket */
        cnt[b]++;
    }
    return pairs;
}

static long collisions_brute(uint32_t (*h)(const Fam *, uint32_t), const Fam *f, const uint32_t *keys, int n) {
    long pairs = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            pairs += h(f, keys[i]) == h(f, keys[j]);
    return pairs;
}

int main(void) {
    /* mulmod61 against slow double-and-add */
    for (int i = 0; i < 2000; i++) {
        uint64_t x = rnd() % P61, y = rnd() % P61, acc = 0, base = x;
        for (uint64_t e = y; e; e >>= 1) {
            if (e & 1) {
                acc += base;
                if (acc >= P61)
                    acc -= P61;
            }
            base += base;
            if (base >= P61)
                base -= P61;
        }
        check(mulmod61(x, y) == acc, "mulmod61");
    }
    static const char *names[4] = {"multiply-shift", "carter-wegman", "tabulation", "low bits"};
    uint32_t (*hs[4])(const Fam *, uint32_t) = {h_ms, h_cw, h_tab, h_low};
    static uint32_t keysets[3][N];
    const char *setnames[3] = {"random", "multiples of 4096", "byte-aligned 0x01010101*i"};
    for (int i = 0; i < N; i++) {
        keysets[0][i] = (uint32_t)rnd();
        keysets[1][i] = (uint32_t)i * 4096u;
        keysets[2][i] = 0x01010101u * (uint32_t)(i & 255) + ((uint32_t)i >> 8) * 0x10000u;
    }
    double expected = (double)N * (N - 1) / 2.0 / (double)(1 << M_BITS);
    printf("n=%d keys, m=%d buckets: expected colliding pairs for an ideal hash = %.1f\n", N, 1 << M_BITS, expected);
    enum { FAMILIES = 40 };
    for (int s = 0; s < 3; s++) {
        printf("key set: %s\n", setnames[s]);
        for (int h = 0; h < 4; h++) {
            long sum = 0, worst = 0;
            for (int t = 0; t < FAMILIES; t++) {
                Fam *f = malloc(sizeof *f); /* fresh random member of the family */
                f->a = rnd();
                f->b = rnd();
                for (int i = 0; i < 4; i++)
                    for (int j = 0; j < 256; j++)
                        f->tab[i][j] = (uint32_t)rnd();
                long c = collisions(hs[h], f, keysets[s], N);
                if (t == 0)
                    check(c == collisions_brute(hs[h], f, keysets[s], N), "pair counting matches brute force");
                sum += c;
                if (c > worst)
                    worst = c;
                free(f);
            }
            printf("  %-15s mean pairs=%8.1f worst=%ld\n", names[h], (double)sum / FAMILIES, worst);
        }
    }
    return 0;
}
