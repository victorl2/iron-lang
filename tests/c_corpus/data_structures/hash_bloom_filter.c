/*
 * title: Bloom filter with double hashing and false positive sweep
 * topic: data_structures
 * covers: bloom filter, Kirsch-Mitzenmacher double hashing, optimal k, false positive rate, filter union, fill ratio
 * deps: libc, libm
 */
#include <math.h>
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
typedef struct {
    uint64_t *bits;
    size_t m; /* number of bits */
    int k;
    size_t set_bits;
} Bloom;

static void bf_init(Bloom *b, size_t m, int k) {
    b->bits = calloc((m + 63) / 64, sizeof(uint64_t));
    b->m = m;
    b->k = k;
    b->set_bits = 0;
}
static void bf_free(Bloom *b) { free(b->bits); }

static void positions(const Bloom *b, uint64_t key, size_t *pos) {
    uint64_t h = mix64(key);
    uint64_t h1 = h & 0xffffffffu, h2 = (h >> 32) | 1u;
    for (int i = 0; i < b->k; i++)
        pos[i] = (size_t)((h1 + (uint64_t)i * h2) % b->m);
}

static void bf_add(Bloom *b, uint64_t key) {
    size_t pos[32];
    positions(b, key, pos);
    for (int i = 0; i < b->k; i++) {
        uint64_t mask = 1ull << (pos[i] & 63);
        if (!(b->bits[pos[i] >> 6] & mask))
            b->set_bits++;
        b->bits[pos[i] >> 6] |= mask;
    }
}

static int bf_maybe(const Bloom *b, uint64_t key) {
    size_t pos[32];
    positions(b, key, pos);
    for (int i = 0; i < b->k; i++)
        if (!(b->bits[pos[i] >> 6] & (1ull << (pos[i] & 63))))
            return 0;
    return 1;
}

static void bf_union(Bloom *dst, const Bloom *a, const Bloom *b) {
    bf_init(dst, a->m, a->k);
    for (size_t i = 0; i < (a->m + 63) / 64; i++) {
        dst->bits[i] = a->bits[i] | b->bits[i];
        dst->set_bits += (size_t)__builtin_popcountll(dst->bits[i]);
    }
}

int main(void) {
    enum { N = 2000, Q = 40000 };
    static uint64_t keys[N];
    for (int i = 0; i < N; i++)
        keys[i] = (uint64_t)i * 2 + 1; /* members are odd numbers, non-members are even */
    static const int bits_per_key[4] = {4, 8, 12, 16};
    for (int c = 0; c < 4; c++) {
        size_t m = (size_t)N * (size_t)bits_per_key[c];
        int k = (int)(0.6931471805599453 * bits_per_key[c] + 0.5);
        Bloom b;
        bf_init(&b, m, k);
        for (int i = 0; i < N; i++)
            bf_add(&b, keys[i]);
        for (int i = 0; i < N; i++)
            check(bf_maybe(&b, keys[i]), "no false negatives");
        long fp = 0;
        for (int q = 0; q < Q; q++)
            fp += bf_maybe(&b, (uint64_t)(rnd() % 1000000) * 2);
        double theory = pow(1.0 - exp(-(double)k * N / (double)m), k);
        double measured = (double)fp / Q;
        printf("bits/key=%2d k=%d fill=%.4f measured fpr=%.4f theory=%.4f\n", bits_per_key[c], k, (double)b.set_bits / (double)m, measured,
               theory);
        check(fabs(measured - theory) < 0.02 + 0.3 * theory, "measured near theory");
        bf_free(&b);
    }
    /* union of two filters equals the filter of the union set */
    Bloom a, b, u, whole;
    bf_init(&a, 20000, 5);
    bf_init(&b, 20000, 5);
    bf_init(&whole, 20000, 5);
    for (int i = 0; i < N; i++) {
        bf_add(i % 3 == 0 ? &a : &b, keys[i]);
        bf_add(&whole, keys[i]);
    }
    bf_union(&u, &a, &b);
    check(memcmp(u.bits, whole.bits, (20000 + 63) / 64 * 8) == 0, "union equals filter of union");
    printf("union check: bits set=%zu (a=%zu, b=%zu)\n", u.set_bits, a.set_bits, b.set_bits);
    bf_free(&a);
    bf_free(&b);
    bf_free(&u);
    bf_free(&whole);
    return 0;
}
