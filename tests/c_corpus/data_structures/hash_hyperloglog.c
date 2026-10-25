/*
 * title: HyperLogLog cardinality estimator with merge
 * topic: data_structures
 * covers: hyperloglog, register max of leading zeros, harmonic mean, small range linear counting, sketch merge, relative error
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
    int p;
    uint8_t *reg;
} HLL;

static void hll_init(HLL *h, int p) {
    h->p = p;
    h->reg = calloc((size_t)1 << p, 1);
}
static void hll_free(HLL *h) { free(h->reg); }

static void hll_add(HLL *h, uint64_t item) {
    uint64_t x = mix64(item + 0x1234567ull);
    size_t j = (size_t)(x >> (64 - h->p));
    uint64_t rest = x << h->p; /* remaining 64-p bits, left aligned */
    int rho = 1;
    while (rho <= 64 - h->p && !(rest & (1ull << 63))) {
        rho++;
        rest <<= 1;
    }
    if (rho > h->reg[j])
        h->reg[j] = (uint8_t)rho;
}

static double hll_estimate(const HLL *h) {
    double m = (double)((size_t)1 << h->p);
    double alpha = h->p == 4 ? 0.673 : h->p == 5 ? 0.697 : h->p == 6 ? 0.709 : 0.7213 / (1.0 + 1.079 / m);
    double sum = 0;
    size_t zeros = 0;
    for (size_t i = 0; i < (size_t)m; i++) {
        sum += ldexp(1.0, -(int)h->reg[i]);
        zeros += h->reg[i] == 0;
    }
    double e = alpha * m * m / sum;
    if (e <= 2.5 * m && zeros)
        e = m * log(m / (double)zeros); /* linear counting for small cardinalities */
    return e;
}

static void hll_merge(HLL *dst, const HLL *a, const HLL *b) {
    hll_init(dst, a->p);
    for (size_t i = 0; i < (size_t)1 << a->p; i++)
        dst->reg[i] = a->reg[i] > b->reg[i] ? a->reg[i] : b->reg[i];
}

int main(void) {
    static const int ps[3] = {6, 10, 12};
    static const uint64_t ns[5] = {10, 100, 1000, 20000, 200000};
    printf("precision -> std error bound 1.04/sqrt(m)\n");
    for (int c = 0; c < 3; c++) {
        HLL h;
        hll_init(&h, ps[c]);
        uint64_t added = 0;
        printf("p=%2d (m=%4d, bound %.4f):", ps[c], 1 << ps[c], 1.04 / sqrt((double)(1 << ps[c])));
        for (int t = 0; t < 5; t++) {
            while (added < ns[t]) {
                hll_add(&h, added * 3 + 7);
                if (added % 5 == 0)
                    hll_add(&h, added * 3 + 7); /* duplicates must not matter */
                added++;
            }
            double est = hll_estimate(&h);
            double rel = fabs(est - (double)ns[t]) / (double)ns[t];
            printf(" n=%llu err=%.3f", (unsigned long long)ns[t], rel);
            check(rel < 4.5 * 1.04 / sqrt((double)(1 << ps[c])) + 0.02, "within a few standard errors");
        }
        printf("\n");
        hll_free(&h);
    }
    /* merge: overlapping ranges [0,30000) and [20000,50000) -> union 50000 */
    HLL a, b, m, whole;
    hll_init(&a, 12);
    hll_init(&b, 12);
    hll_init(&whole, 12);
    for (uint64_t i = 0; i < 30000; i++) {
        hll_add(&a, i);
        hll_add(&whole, i);
    }
    for (uint64_t i = 20000; i < 50000; i++) {
        hll_add(&b, i);
        hll_add(&whole, i);
    }
    hll_merge(&m, &a, &b);
    check(memcmp(m.reg, whole.reg, (size_t)1 << 12) == 0, "merge equals sketch of union");
    double ea = hll_estimate(&a), eb = hll_estimate(&b), eu = hll_estimate(&m);
    double inter = ea + eb - eu; /* inclusion-exclusion estimate of the overlap */
    printf("merge: |A|~%.0f |B|~%.0f |A u B|~%.0f overlap~%.0f (true 10000)\n", ea, eb, eu, inter);
    check(fabs(eu - 50000.0) / 50000.0 < 0.05, "union estimate");
    /* register histogram of the union sketch */
    int hist[20] = {0};
    for (int i = 0; i < 1 << 12; i++)
        hist[m.reg[i] < 19 ? m.reg[i] : 19]++;
    printf("register value histogram:");
    for (int v = 0; v < 16; v++)
        printf(" %d:%d", v, hist[v]);
    printf("\n");
    hll_free(&a);
    hll_free(&b);
    hll_free(&m);
    hll_free(&whole);
    return 0;
}
