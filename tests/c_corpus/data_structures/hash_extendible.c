/*
 * title: Extendible hashing with directory doubling and buddy merge
 * topic: data_structures
 * covers: extendible hashing, global and local depth, bucket split, directory doubling and halving, buddy merge
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
enum { BCAP = 4 };
typedef struct {
    uint32_t key[BCAP];
    int val[BCAP];
    int n;
    int depth;
} Bucket;
typedef struct {
    Bucket **dir;
    int gd;
    size_t n, nbuckets;
    long splits, doublings, merges, halvings;
} EH;

static size_t di(const EH *t, uint32_t k) { return mix32(k) & ((1u << t->gd) - 1); }

static Bucket *new_bucket(EH *t, int depth) {
    Bucket *b = calloc(1, sizeof *b);
    b->depth = depth;
    t->nbuckets++;
    return b;
}

static void eh_init(EH *t) {
    memset(t, 0, sizeof *t);
    t->gd = 1;
    t->dir = malloc(2 * sizeof(Bucket *));
    t->dir[0] = new_bucket(t, 1);
    t->dir[1] = new_bucket(t, 1);
}

static int b_find(const Bucket *b, uint32_t k) {
    for (int i = 0; i < b->n; i++)
        if (b->key[i] == k)
            return i;
    return -1;
}

static int eh_get(EH *t, uint32_t k, int *out) {
    Bucket *b = t->dir[di(t, k)];
    int i = b_find(b, k);
    if (i < 0)
        return 0;
    *out = b->val[i];
    return 1;
}

static void split(EH *t, size_t idx) {
    Bucket *b = t->dir[idx];
    if (b->depth == t->gd) {
        size_t sz = (size_t)1 << t->gd;
        t->dir = realloc(t->dir, sz * 2 * sizeof(Bucket *));
        memcpy(t->dir + sz, t->dir, sz * sizeof(Bucket *));
        t->gd++;
        t->doublings++;
    }
    b->depth++;
    Bucket *nb = new_bucket(t, b->depth);
    uint32_t bit = 1u << (b->depth - 1);
    for (size_t i = 0; i < ((size_t)1 << t->gd); i++)
        if (t->dir[i] == b && (i & bit))
            t->dir[i] = nb;
    Bucket old = *b;
    b->n = 0;
    for (int i = 0; i < old.n; i++) {
        Bucket *dst = (mix32(old.key[i]) & bit) ? nb : b;
        dst->key[dst->n] = old.key[i];
        dst->val[dst->n++] = old.val[i];
    }
    t->splits++;
}

static int eh_put(EH *t, uint32_t k, int v) {
    Bucket *b = t->dir[di(t, k)];
    int i = b_find(b, k);
    if (i >= 0) {
        b->val[i] = v;
        return 0;
    }
    while (b->n == BCAP) {
        split(t, di(t, k));
        b = t->dir[di(t, k)];
    }
    b->key[b->n] = k;
    b->val[b->n++] = v;
    t->n++;
    return 1;
}

static int eh_del(EH *t, uint32_t k) {
    size_t idx = di(t, k);
    Bucket *b = t->dir[idx];
    int i = b_find(b, k);
    if (i < 0)
        return 0;
    b->key[i] = b->key[b->n - 1];
    b->val[i] = b->val[b->n - 1];
    b->n--;
    t->n--;
    /* merge with buddy while combined contents fit and depths match */
    while (b->depth > 1) {
        size_t buddy_idx = idx ^ ((size_t)1 << (b->depth - 1));
        Bucket *bu = t->dir[buddy_idx];
        if (bu == b || bu->depth != b->depth || b->n + bu->n > BCAP - 1)
            break;
        for (int j = 0; j < bu->n; j++) {
            b->key[b->n] = bu->key[j];
            b->val[b->n++] = bu->val[j];
        }
        for (size_t j = 0; j < ((size_t)1 << t->gd); j++)
            if (t->dir[j] == bu)
                t->dir[j] = b;
        free(bu);
        t->nbuckets--;
        b->depth--;
        t->merges++;
    }
    for (;;) { /* halve the directory when no bucket needs the top bit */
        int need = 0;
        for (size_t j = 0; j < ((size_t)1 << t->gd); j++)
            if (t->dir[j]->depth == t->gd)
                need = 1;
        if (need || t->gd == 1)
            break;
        t->gd--;
        t->dir = realloc(t->dir, ((size_t)1 << t->gd) * sizeof(Bucket *));
        t->halvings++;
    }
    return 1;
}

static void eh_free(EH *t) {
    for (size_t i = 0; i < ((size_t)1 << t->gd); i++) {
        Bucket *b = t->dir[i];
        if (!b)
            continue;
        for (size_t j = i; j < ((size_t)1 << t->gd); j++)
            if (t->dir[j] == b)
                t->dir[j] = NULL;
        free(b);
    }
    free(t->dir);
}

int main(void) {
    EH t;
    eh_init(&t);
    int peak_gd = 1;
    for (int step = 0; step < 36000; step++) {
        uint32_t k = (uint32_t)(rnd() % 2500);
        int ri = ref_find(k);
        int grow = (step / 6000) % 2 == 0;
        int op = (int)(rnd() % 10);
        if (op < (grow ? 8 : 1)) {
            int v = (int)(rnd() & 0xffff);
            check(eh_put(&t, k, v) == (ri < 0), "put");
            ref_put(k, v);
        } else if (op < 9) {
            check(eh_del(&t, k) == (ri >= 0), "del");
            ref_del(k);
        } else {
            int out = 0;
            check(eh_get(&t, k, &out) == (ri >= 0), "get");
            if (ri >= 0)
                check(out == ref_v[ri], "val");
        }
        check(t.n == (size_t)ref_n, "size");
        if (t.gd > peak_gd)
            peak_gd = t.gd;
    }
    /* directory invariants: bucket with depth d is referenced by exactly 2^(gd-d) entries */
    size_t dirsz = (size_t)1 << t.gd;
    for (size_t i = 0; i < dirsz; i++) {
        Bucket *b = t.dir[i];
        size_t refs = 0;
        for (size_t j = 0; j < dirsz; j++)
            refs += t.dir[j] == b;
        check(refs == ((size_t)1 << (t.gd - b->depth)), "directory refs");
        for (int j = 0; j < b->n; j++)
            check((mix32(b->key[j]) & (((uint32_t)1 << b->depth) - 1)) == (i & (((size_t)1 << b->depth) - 1)), "key prefix");
    }
    printf("n=%zu global depth=%d (peak %d) buckets=%zu\n", t.n, t.gd, peak_gd, t.nbuckets);
    printf("splits=%ld doublings=%ld merges=%ld halvings=%ld\n", t.splits, t.doublings, t.merges, t.halvings);
    printf("avg bucket fill=%.4f of %d\n", (double)t.n / (double)t.nbuckets, BCAP);
    eh_free(&t);
    return 0;
}
