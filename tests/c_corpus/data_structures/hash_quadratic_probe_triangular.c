/*
 * title: Quadratic probing with triangular numbers
 * topic: data_structures
 * covers: quadratic probing, triangular probe sequence, full-cycle guarantee on power-of-two tables, comparison with linear
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
    uint8_t *st; /* 0 empty, 1 full, 2 dead */
    uint32_t *key;
    size_t cap, n, dead;
    long probes, ops;
    int quad;
} Tab;

static void tab_init(Tab *t, size_t cap, int quad) {
    t->st = calloc(cap, 1);
    t->key = calloc(cap, sizeof(uint32_t));
    t->cap = cap;
    t->n = t->dead = 0;
    t->probes = t->ops = 0;
    t->quad = quad;
}
static void tab_free(Tab *t) {
    free(t->st);
    free(t->key);
}

static size_t step_to(const Tab *t, size_t home, size_t i) {
    size_t mask = t->cap - 1;
    return t->quad ? (home + (i * (i + 1)) / 2) & mask : (home + i) & mask;
}

static long tab_find(Tab *t, uint32_t k) {
    size_t h = mix32(k) & (t->cap - 1);
    t->ops++;
    for (size_t i = 0; i < t->cap; i++) {
        size_t s = step_to(t, h, i);
        t->probes++;
        if (t->st[s] == 0)
            return -1;
        if (t->st[s] == 1 && t->key[s] == k)
            return (long)s;
    }
    return -1;
}

static void tab_insert(Tab *t, uint32_t k) {
    size_t h = mix32(k) & (t->cap - 1);
    for (size_t i = 0; i < t->cap; i++) {
        size_t s = step_to(t, h, i);
        if (t->st[s] != 1) {
            if (t->st[s] == 2)
                t->dead--;
            t->st[s] = 1;
            t->key[s] = k;
            t->n++;
            return;
        }
    }
    check(0, "table full");
}

static int tab_add(Tab *t, uint32_t k) {
    if (tab_find(t, k) >= 0)
        return 0;
    if ((t->n + t->dead + 1) * 2 > t->cap) { /* quadratic needs load <= 0.5 in general */
        Tab nt;
        tab_init(&nt, t->n * 4 > t->cap ? t->cap * 2 : t->cap, t->quad);
        for (size_t i = 0; i < t->cap; i++)
            if (t->st[i] == 1)
                tab_insert(&nt, t->key[i]);
        nt.probes = t->probes;
        nt.ops = t->ops;
        tab_free(t);
        *t = nt;
    }
    tab_insert(t, k);
    return 1;
}

static int tab_remove(Tab *t, uint32_t k) {
    long f = tab_find(t, k);
    if (f < 0)
        return 0;
    t->st[f] = 2;
    t->n--;
    t->dead++;
    return 1;
}

int main(void) {
    /* 1. triangular probing visits every slot exactly once on power-of-two sizes */
    for (size_t cap = 2; cap <= 1024; cap *= 2) {
        uint8_t *seen = calloc(cap, 1);
        for (size_t i = 0; i < cap; i++) {
            size_t s = (5 + (i * (i + 1)) / 2) & (cap - 1);
            check(!seen[s], "triangular permutation");
            seen[s] = 1;
        }
        free(seen);
    }
    printf("triangular sequence is a full permutation for sizes 2..1024\n");

    /* 2. same workload against linear and quadratic tables */
    for (int quad = 0; quad < 2; quad++) {
        rs = 777;
        ref_n = 0;
        Tab t;
        tab_init(&t, 16, quad);
        for (int step = 0; step < 20000; step++) {
            uint32_t k = (uint32_t)(rnd() % 1200);
            int ri = ref_find(k);
            switch (rnd() % 3) {
            case 0:
                check(tab_add(&t, k) == (ri < 0), "add");
                ref_put(k, 0);
                break;
            case 1:
                check(tab_remove(&t, k) == (ri >= 0), "remove");
                ref_del(k);
                break;
            default:
                check((tab_find(&t, k) >= 0) == (ri >= 0), "contains");
            }
            check(t.n == (size_t)ref_n, "size");
        }
        printf("%s: n=%zu cap=%zu dead=%zu avg probes=%.4f\n", quad ? "quadratic" : "linear   ", t.n, t.cap, t.dead,
               (double)t.probes / (double)t.ops);
        tab_free(&t);
    }
    return 0;
}
