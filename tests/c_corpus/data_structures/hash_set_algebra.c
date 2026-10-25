/*
 * title: Open-addressing hash set with union, intersection and difference
 * topic: data_structures
 * covers: hash set, set algebra, subset test, symmetric difference, iteration, bitmap reference model
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
typedef struct {
    uint64_t *slot; /* stores key+1, 0 = empty; keys are < 2^63 */
    size_t cap, n;
} Set;

static void set_init(Set *s, size_t cap) {
    s->slot = calloc(cap, sizeof(uint64_t));
    s->cap = cap;
    s->n = 0;
}
static void set_free(Set *s) { free(s->slot); }

static int set_has(const Set *s, uint64_t k) {
    size_t i = mix64(k) & (s->cap - 1);
    while (s->slot[i]) {
        if (s->slot[i] == k + 1)
            return 1;
        i = (i + 1) & (s->cap - 1);
    }
    return 0;
}

static int set_add(Set *s, uint64_t k) {
    if (set_has(s, k))
        return 0;
    if ((s->n + 1) * 2 > s->cap) {
        Set n;
        set_init(&n, s->cap * 2);
        for (size_t i = 0; i < s->cap; i++)
            if (s->slot[i])
                set_add(&n, s->slot[i] - 1);
        set_free(s);
        *s = n;
    }
    size_t i = mix64(k) & (s->cap - 1);
    while (s->slot[i])
        i = (i + 1) & (s->cap - 1);
    s->slot[i] = k + 1;
    s->n++;
    return 1;
}

typedef int (*Pred)(const Set *, const Set *, uint64_t);
static int p_or(const Set *a, const Set *b, uint64_t k) { return set_has(a, k) || set_has(b, k); }
static int p_and(const Set *a, const Set *b, uint64_t k) { return set_has(a, k) && set_has(b, k); }
static int p_diff(const Set *a, const Set *b, uint64_t k) { return set_has(a, k) && !set_has(b, k); }
static int p_xor(const Set *a, const Set *b, uint64_t k) { return set_has(a, k) != set_has(b, k); }

static void combine(Set *out, const Set *a, const Set *b, Pred p) {
    set_init(out, 16);
    const Set *src[2] = {a, b};
    for (int w = 0; w < 2; w++)
        for (size_t i = 0; i < src[w]->cap; i++)
            if (src[w]->slot[i] && p(a, b, src[w]->slot[i] - 1))
                set_add(out, src[w]->slot[i] - 1);
}

static int is_subset(const Set *a, const Set *b) {
    for (size_t i = 0; i < a->cap; i++)
        if (a->slot[i] && !set_has(b, a->slot[i] - 1))
            return 0;
    return 1;
}

enum { U = 300 };
int main(void) {
    static const char *names[4] = {"union", "intersection", "difference", "symmetric difference"};
    Pred preds[4] = {p_or, p_and, p_diff, p_xor};
    for (int round = 0; round < 4; round++) {
        Set a, b;
        set_init(&a, 8);
        set_init(&b, 8);
        uint8_t ra[U] = {0}, rb[U] = {0}; /* reference: membership bitmaps */
        int na = 40 + (int)(rnd() % 120), nb = 40 + (int)(rnd() % 120);
        for (int i = 0; i < na; i++) {
            uint64_t k = rnd() % U;
            check(set_add(&a, k) == !ra[k], "add a");
            ra[k] = 1;
        }
        for (int i = 0; i < nb; i++) {
            uint64_t k = rnd() % U;
            check(set_add(&b, k) == !rb[k], "add b");
            rb[k] = 1;
        }
        printf("round %d: |A|=%zu |B|=%zu", round, a.n, b.n);
        for (int op = 0; op < 4; op++) {
            Set r;
            combine(&r, &a, &b, preds[op]);
            size_t expect = 0;
            for (int k = 0; k < U; k++) {
                int want = op == 0 ? (ra[k] || rb[k]) : op == 1 ? (ra[k] && rb[k]) : op == 2 ? (ra[k] && !rb[k]) : (ra[k] != rb[k]);
                check(set_has(&r, (uint64_t)k) == want, "membership");
                expect += (size_t)want;
            }
            check(r.n == expect, "cardinality");
            printf(" %s=%zu", op == 3 ? "xor" : names[op], r.n);
            set_free(&r);
        }
        Set inter;
        combine(&inter, &a, &b, p_and);
        check(is_subset(&inter, &a) && is_subset(&inter, &b), "intersection subset");
        Set uni;
        combine(&uni, &a, &b, p_or);
        check(is_subset(&a, &uni) && is_subset(&b, &uni), "union superset");
        check(uni.n + inter.n == a.n + b.n, "inclusion-exclusion");
        printf(" jaccard=%.4f\n", (double)inter.n / (double)uni.n);
        set_free(&inter);
        set_free(&uni);
        set_free(&a);
        set_free(&b);
    }
    return 0;
}
