/*
 * title: Linear probing with backward-shift deletion
 * topic: data_structures
 * covers: open addressing, linear probing, backward shift deletion, cyclic distance, no tombstones
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
    uint8_t *used;
    uint32_t *key;
    int *val;
    size_t cap, n;
    long shifts, probes, ops;
} Tab;

static size_t home(const Tab *t, uint32_t k) { return mix32(k) & (t->cap - 1); }

static void tab_init(Tab *t, size_t cap) {
    t->used = calloc(cap, 1);
    t->key = calloc(cap, sizeof(uint32_t));
    t->val = calloc(cap, sizeof(int));
    t->cap = cap;
    t->n = 0;
    t->shifts = t->probes = t->ops = 0;
}
static void tab_free(Tab *t) {
    free(t->used);
    free(t->key);
    free(t->val);
}

static long tab_find(Tab *t, uint32_t k) {
    size_t i = home(t, k);
    t->ops++;
    for (;;) {
        t->probes++;
        if (!t->used[i])
            return -1;
        if (t->key[i] == k)
            return (long)i;
        i = (i + 1) & (t->cap - 1);
    }
}

static void tab_put(Tab *t, uint32_t k, int v);

static void tab_grow(Tab *t) {
    Tab n;
    tab_init(&n, t->cap * 2);
    for (size_t i = 0; i < t->cap; i++)
        if (t->used[i])
            tab_put(&n, t->key[i], t->val[i]);
    n.shifts = t->shifts;
    n.probes = t->probes;
    n.ops = t->ops;
    tab_free(t);
    *t = n;
}

static void tab_put(Tab *t, uint32_t k, int v) {
    long f = tab_find(t, k);
    if (f >= 0) {
        t->val[f] = v;
        return;
    }
    if ((t->n + 1) * 10 > t->cap * 7)
        tab_grow(t);
    size_t i = home(t, k);
    while (t->used[i])
        i = (i + 1) & (t->cap - 1);
    t->used[i] = 1;
    t->key[i] = k;
    t->val[i] = v;
    t->n++;
}

static int tab_del(Tab *t, uint32_t k) {
    long f = tab_find(t, k);
    if (f < 0)
        return 0;
    size_t mask = t->cap - 1, hole = (size_t)f, j = hole;
    for (;;) {
        j = (j + 1) & mask;
        if (!t->used[j])
            break;
        size_t h = home(t, t->key[j]);
        /* entry at j may move to hole iff its home is not cyclically within (hole, j] */
        size_t dist_home = (j - h) & mask;
        size_t dist_hole = (j - hole) & mask;
        if (dist_home >= dist_hole) {
            t->key[hole] = t->key[j];
            t->val[hole] = t->val[j];
            hole = j;
            t->shifts++;
        }
    }
    t->used[hole] = 0;
    t->n--;
    return 1;
}

static void check_invariant(const Tab *t) {
    /* every key must be reachable from its home without crossing an empty slot */
    for (size_t i = 0; i < t->cap; i++) {
        if (!t->used[i])
            continue;
        size_t h = home(t, t->key[i]);
        for (size_t p = h; p != i; p = (p + 1) & (t->cap - 1))
            check(t->used[p], "no gap between home and slot");
    }
}

int main(void) {
    Tab t;
    tab_init(&t, 8);
    long dels = 0;
    for (int step = 0; step < 40000; step++) {
        uint32_t k = (uint32_t)(rnd() % 2000);
        int op = (int)(rnd() % 10);
        int ri = ref_find(k);
        if (op < 5) {
            int v = (int)(rnd() & 0xffff);
            tab_put(&t, k, v);
            ref_put(k, v);
        } else if (op < 8) {
            int r = tab_del(&t, k);
            check(r == (ri >= 0), "del");
            dels += r;
            ref_del(k);
        } else {
            long f = tab_find(&t, k);
            check((f >= 0) == (ri >= 0), "find");
            if (f >= 0)
                check(t.val[f] == ref_v[ri], "value");
        }
        check(t.n == (size_t)ref_n, "size");
        if (step % 5000 == 0)
            check_invariant(&t);
    }
    check_invariant(&t);
    /* drain everything: the table must end empty with no residue */
    long drained = 0;
    while (ref_n > 0) {
        uint32_t k = ref_k[ref_n - 1];
        check(tab_del(&t, k), "drain del");
        ref_del(k);
        drained++;
    }
    for (size_t i = 0; i < t.cap; i++)
        check(!t.used[i], "empty after drain");
    printf("deletes=%ld drained=%ld shifts=%ld cap=%zu\n", dels, drained, t.shifts, t.cap);
    printf("avg probes=%.4f\n", (double)t.probes / (double)t.ops);
    tab_free(&t);
    return 0;
}
