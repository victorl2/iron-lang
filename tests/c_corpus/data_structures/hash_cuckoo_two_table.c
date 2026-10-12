/*
 * title: Classic two-table cuckoo hashing with rehash
 * topic: data_structures
 * covers: cuckoo hashing, eviction chains, kick limit, rehash with new seeds, worst-case O(1) lookup
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
    uint32_t key;
    int val;
    uint8_t used;
} Slot;
typedef struct {
    Slot *t[2];
    size_t cap, n;
    uint32_t seed[2];
    long kicks, rehashes, growths, maxchain;
} Cuckoo;

static size_t hf(const Cuckoo *c, int w, uint32_t k) { return mix32(k ^ c->seed[w]) % c->cap; }

static void cu_alloc(Cuckoo *c, size_t cap) {
    c->cap = cap;
    c->n = 0;
    for (int w = 0; w < 2; w++)
        c->t[w] = calloc(cap, sizeof(Slot));
    c->seed[0] = (uint32_t)rnd();
    c->seed[1] = (uint32_t)rnd();
}

static Slot *cu_find(Cuckoo *c, uint32_t k) {
    for (int w = 0; w < 2; w++) {
        Slot *s = &c->t[w][hf(c, w, k)];
        if (s->used && s->key == k)
            return s;
    }
    return NULL;
}

/* returns 1 on success, 0 if the kick limit was exceeded (item left in *ck,*cv) */
static int cu_place(Cuckoo *c, uint32_t *ck, int *cv) {
    int w = 0;
    long chain = 0;
    for (int it = 0; it < 40; it++) {
        Slot *s = &c->t[w][hf(c, w, *ck)];
        if (!s->used) {
            s->used = 1;
            s->key = *ck;
            s->val = *cv;
            c->n++;
            if (chain > c->maxchain)
                c->maxchain = chain;
            return 1;
        }
        uint32_t k2 = s->key;
        int v2 = s->val;
        s->key = *ck;
        s->val = *cv;
        *ck = k2;
        *cv = v2;
        c->kicks++;
        chain++;
        w ^= 1;
    }
    return 0;
}

static void cu_put(Cuckoo *c, uint32_t k, int v);

static void cu_rebuild(Cuckoo *c, size_t cap, uint32_t pk, int pv) {
    Cuckoo old = *c;
    long kicks = c->kicks, reh = c->rehashes + 1, gr = c->growths, mc = c->maxchain;
    cu_alloc(c, cap);
    c->kicks = kicks;
    c->rehashes = reh;
    c->growths = gr + (cap != old.cap);
    c->maxchain = mc;
    for (int w = 0; w < 2; w++) {
        for (size_t i = 0; i < old.cap; i++)
            if (old.t[w][i].used)
                cu_put(c, old.t[w][i].key, old.t[w][i].val);
        free(old.t[w]);
    }
    cu_put(c, pk, pv);
}

static void cu_put(Cuckoo *c, uint32_t k, int v) {
    Slot *s = cu_find(c, k);
    if (s) {
        s->val = v;
        return;
    }
    if (c->n * 100 >= c->cap * 2 * 45) { /* keep total load below ~45% of 2*cap */
        cu_rebuild(c, c->cap * 2, k, v);
        return;
    }
    if (!cu_place(c, &k, &v))
        cu_rebuild(c, c->cap, k, v);
}

static int cu_del(Cuckoo *c, uint32_t k) {
    Slot *s = cu_find(c, k);
    if (!s)
        return 0;
    s->used = 0;
    c->n--;
    return 1;
}

int main(void) {
    Cuckoo c;
    memset(&c, 0, sizeof c);
    cu_alloc(&c, 16);
    for (int step = 0; step < 30000; step++) {
        uint32_t k = (uint32_t)(rnd() % 2500);
        int ri = ref_find(k);
        int op = (int)(rnd() % 10);
        if (op < 5) {
            int v = (int)(rnd() & 0xffff);
            cu_put(&c, k, v);
            ref_put(k, v);
        } else if (op < 7) {
            check(cu_del(&c, k) == (ri >= 0), "del");
            ref_del(k);
        } else {
            Slot *s = cu_find(&c, k);
            check((s != NULL) == (ri >= 0), "find");
            if (s)
                check(s->val == ref_v[ri], "value");
        }
        check(c.n == (size_t)ref_n, "size");
    }
    for (int i = 0; i < ref_n; i++) {
        Slot *s = cu_find(&c, ref_k[i]);
        check(s && s->val == ref_v[i], "sweep");
    }
    printf("n=%zu cap/table=%zu load=%.4f\n", c.n, c.cap, (double)c.n / (double)(2 * c.cap));
    printf("kicks=%ld rehashes=%ld growths=%ld longest eviction chain=%ld\n", c.kicks, c.rehashes, c.growths, c.maxchain);
    free(c.t[0]);
    free(c.t[1]);
    return 0;
}
