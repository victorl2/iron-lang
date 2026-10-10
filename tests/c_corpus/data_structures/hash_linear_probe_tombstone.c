/*
 * title: Linear probing with tombstones and cleanup rehash
 * topic: data_structures
 * covers: open addressing, linear probing, tombstones, rehash on tombstone pressure, probe statistics
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
enum { EMPTY = 0, FULL = 1, DEAD = 2 };
typedef struct {
    uint8_t *st;
    uint32_t *key;
    int *val;
    size_t cap, live, dead;
    long probes, ops, rehash_grow, rehash_clean;
} Tab;

static void tab_init(Tab *t, size_t cap) {
    t->st = calloc(cap, 1);
    t->key = calloc(cap, sizeof(uint32_t));
    t->val = calloc(cap, sizeof(int));
    t->cap = cap;
    t->live = t->dead = 0;
    t->probes = t->ops = t->rehash_grow = t->rehash_clean = 0;
}
static void tab_free(Tab *t) {
    free(t->st);
    free(t->key);
    free(t->val);
}

static void tab_insert_raw(Tab *t, uint32_t k, int v) {
    size_t i = mix32(k) & (t->cap - 1);
    while (t->st[i] == FULL)
        i = (i + 1) & (t->cap - 1);
    t->st[i] = FULL;
    t->key[i] = k;
    t->val[i] = v;
}

static void tab_rehash(Tab *t, size_t ncap) {
    Tab n;
    tab_init(&n, ncap);
    for (size_t i = 0; i < t->cap; i++)
        if (t->st[i] == FULL)
            tab_insert_raw(&n, t->key[i], t->val[i]);
    n.live = t->live;
    n.probes = t->probes;
    n.ops = t->ops;
    n.rehash_grow = t->rehash_grow;
    n.rehash_clean = t->rehash_clean;
    tab_free(t);
    *t = n;
}

/* returns slot index or -1 */
static long tab_find(Tab *t, uint32_t k) {
    size_t i = mix32(k) & (t->cap - 1);
    t->ops++;
    for (;;) {
        t->probes++;
        if (t->st[i] == EMPTY)
            return -1;
        if (t->st[i] == FULL && t->key[i] == k)
            return (long)i;
        i = (i + 1) & (t->cap - 1);
    }
}

static int tab_put(Tab *t, uint32_t k, int v) {
    long f = tab_find(t, k);
    if (f >= 0) {
        t->val[f] = v;
        return 0;
    }
    if ((t->live + t->dead + 1) * 8 > t->cap * 6) { /* used > 0.75 */
        if (t->live * 8 > t->cap * 3) {
            tab_rehash(t, t->cap * 2);
            t->rehash_grow++;
        } else {
            tab_rehash(t, t->cap);
            t->rehash_clean++;
        }
        t->dead = 0;
    }
    size_t i = mix32(k) & (t->cap - 1);
    while (t->st[i] == FULL)
        i = (i + 1) & (t->cap - 1);
    if (t->st[i] == DEAD)
        t->dead--;
    t->st[i] = FULL;
    t->key[i] = k;
    t->val[i] = v;
    t->live++;
    return 1;
}

static int tab_del(Tab *t, uint32_t k) {
    long f = tab_find(t, k);
    if (f < 0)
        return 0;
    t->st[f] = DEAD;
    t->live--;
    t->dead++;
    return 1;
}

int main(void) {
    Tab t;
    tab_init(&t, 16);
    long maxdead = 0;
    for (int step = 0; step < 30000; step++) {
        uint32_t k = (uint32_t)(rnd() % 3000);
        int op = (int)(rnd() % 10);
        int ri = ref_find(k);
        if (op < 4) {
            int v = (int)(rnd() & 0xfffff);
            check(tab_put(&t, k, v) == (ri < 0), "put");
            ref_put(k, v);
        } else if (op < 7) {
            check(tab_del(&t, k) == (ri >= 0), "del");
            ref_del(k);
        } else {
            long f = tab_find(&t, k);
            check((f >= 0) == (ri >= 0), "find");
            if (f >= 0)
                check(t.val[f] == ref_v[ri], "value");
        }
        check(t.live == (size_t)ref_n, "size");
        if ((long)t.dead > maxdead)
            maxdead = (long)t.dead;
    }
    for (int i = 0; i < ref_n; i++) {
        long f = tab_find(&t, ref_k[i]);
        check(f >= 0 && t.val[f] == ref_v[i], "final sweep");
    }
    size_t run = 0, maxrun = 0;
    for (size_t i = 0; i < t.cap; i++) {
        if (t.st[i] != EMPTY) {
            run++;
            if (run > maxrun)
                maxrun = run;
        } else
            run = 0;
    }
    printf("live=%zu dead=%zu cap=%zu load=%.4f\n", t.live, t.dead, t.cap, (double)t.live / (double)t.cap);
    printf("grow rehashes=%ld cleanup rehashes=%ld max tombstones=%ld\n", t.rehash_grow, t.rehash_clean, maxdead);
    printf("avg probes=%.4f longest occupied run=%zu\n", (double)t.probes / (double)t.ops, maxrun);
    tab_free(&t);
    return 0;
}
