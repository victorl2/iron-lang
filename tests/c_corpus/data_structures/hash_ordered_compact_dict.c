/*
 * title: Insertion-ordered compact dictionary (index table plus dense entries)
 * topic: data_structures
 * covers: compact dict, insertion order iteration, dummy index markers, entry compaction, popitem, string keys
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
enum { IDX_EMPTY = -1, IDX_DUMMY = -2 };
typedef struct {
    char *key; /* NULL = deleted entry */
    uint32_t hash;
    int val;
} Entry;
typedef struct {
    int32_t *idx;
    size_t idx_cap;
    Entry *ent;
    size_t ent_len, ent_cap, live;
    long compactions, resizes;
} Dict;

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return mix32(h);
}

static void dict_alloc(Dict *d, size_t idx_cap) {
    d->idx_cap = idx_cap;
    d->idx = malloc(idx_cap * sizeof(int32_t));
    for (size_t i = 0; i < idx_cap; i++)
        d->idx[i] = IDX_EMPTY;
    d->ent_cap = idx_cap * 2 / 3;
    d->ent = calloc(d->ent_cap, sizeof(Entry));
    d->ent_len = d->live = 0;
}

static long dict_lookup(const Dict *d, const char *k, uint32_t h, size_t *slot) {
    size_t mask = d->idx_cap - 1, i = h & mask;
    for (size_t perturb = h;; perturb >>= 5) {
        int32_t e = d->idx[i];
        if (e == IDX_EMPTY) {
            *slot = i;
            return -1;
        }
        if (e >= 0 && d->ent[e].hash == h && strcmp(d->ent[e].key, k) == 0) {
            *slot = i;
            return e;
        }
        i = (i * 5 + perturb + 1) & mask; /* CPython-style perturbed probing */
    }
}

static void dict_rebuild(Dict *d, size_t idx_cap) {
    Dict n;
    dict_alloc(&n, idx_cap);
    for (size_t i = 0; i < d->ent_len; i++) {
        if (!d->ent[i].key)
            continue;
        size_t mask = n.idx_cap - 1, j = d->ent[i].hash & mask;
        for (size_t perturb = d->ent[i].hash; n.idx[j] != IDX_EMPTY; perturb >>= 5)
            j = (j * 5 + perturb + 1) & mask;
        n.idx[j] = (int32_t)n.ent_len;
        n.ent[n.ent_len++] = d->ent[i];
    }
    n.live = d->live;
    n.compactions = d->compactions + 1;
    n.resizes = d->resizes + (idx_cap != d->idx_cap);
    free(d->idx);
    free(d->ent);
    *d = n;
}

static void dict_set(Dict *d, const char *k, int v) {
    uint32_t h = hash_str(k);
    size_t slot;
    long e = dict_lookup(d, k, h, &slot);
    if (e >= 0) {
        d->ent[e].val = v;
        return;
    }
    if (d->ent_len == d->ent_cap) {
        dict_rebuild(d, d->live * 3 >= d->ent_cap ? d->idx_cap * 2 : d->idx_cap);
        dict_lookup(d, k, h, &slot);
    }
    size_t l = strlen(k) + 1;
    char *copy = malloc(l);
    memcpy(copy, k, l);
    d->idx[slot] = (int32_t)d->ent_len;
    d->ent[d->ent_len].key = copy;
    d->ent[d->ent_len].hash = h;
    d->ent[d->ent_len].val = v;
    d->ent_len++;
    d->live++;
}

static int dict_del(Dict *d, const char *k) {
    size_t slot;
    long e = dict_lookup(d, k, hash_str(k), &slot);
    if (e < 0)
        return 0;
    free(d->ent[e].key);
    d->ent[e].key = NULL;
    d->idx[slot] = IDX_DUMMY;
    d->live--;
    return 1;
}

/* pop the most recently inserted live entry */
static int dict_popitem(Dict *d, char *out, int *val) {
    for (size_t i = d->ent_len; i-- > 0;)
        if (d->ent[i].key) {
            strcpy(out, d->ent[i].key);
            *val = d->ent[i].val;
            dict_del(d, out);
            return 1;
        }
    return 0;
}

enum { RMAX = 1024 };
static char rk_[RMAX][16];
static int rv_[RMAX], rn_;
static int r_find(const char *k) {
    for (int i = 0; i < rn_; i++)
        if (strcmp(rk_[i], k) == 0)
            return i;
    return -1;
}
static void r_remove(int i) {
    memmove(rk_[i], rk_[i + 1], (size_t)(rn_ - i - 1) * sizeof rk_[0]);
    memmove(&rv_[i], &rv_[i + 1], (size_t)(rn_ - i - 1) * sizeof(int));
    rn_--;
}

static void compare(const Dict *d) {
    int j = 0;
    for (size_t i = 0; i < d->ent_len; i++)
        if (d->ent[i].key) {
            check(j < rn_ && strcmp(d->ent[i].key, rk_[j]) == 0 && d->ent[i].val == rv_[j], "order");
            j++;
        }
    check(j == rn_, "iteration count");
}

int main(void) {
    Dict d;
    memset(&d, 0, sizeof d);
    dict_alloc(&d, 8);
    long pops = 0;
    for (int step = 0; step < 30000; step++) {
        char key[16];
        snprintf(key, sizeof key, "key%u", (unsigned)(rnd() % 600));
        int op = (int)(rnd() % 20);
        int ri = r_find(key);
        if (op < 9) {
            int v = (int)(rnd() % 1000);
            dict_set(&d, key, v);
            if (ri >= 0)
                rv_[ri] = v;
            else {
                strcpy(rk_[rn_], key);
                rv_[rn_++] = v;
            }
        } else if (op < 17) {
            check(dict_del(&d, key) == (ri >= 0), "del");
            if (ri >= 0)
                r_remove(ri);
        } else if (op < 19) {
            char out[16];
            int v = 0;
            int got = dict_popitem(&d, out, &v);
            check(got == (rn_ > 0), "pop presence");
            if (got) {
                check(strcmp(out, rk_[rn_ - 1]) == 0 && v == rv_[rn_ - 1], "pop last");
                rn_--;
                pops++;
            }
        } else {
            size_t slot;
            long e = dict_lookup(&d, key, hash_str(key), &slot);
            check((e >= 0) == (ri >= 0), "lookup");
            if (e >= 0)
                check(d.ent[e].val == rv_[ri], "val");
        }
        check(d.live == (size_t)rn_, "size");
        if (step % 3000 == 0)
            compare(&d);
    }
    compare(&d);
    printf("live=%zu entries used=%zu/%zu index slots=%zu\n", d.live, d.ent_len, d.ent_cap, d.idx_cap);
    printf("compactions=%ld index resizes=%ld popitems=%ld\n", d.compactions, d.resizes, pops);
    printf("first entries in insertion order:");
    int shown = 0;
    for (size_t i = 0; i < d.ent_len && shown < 6; i++)
        if (d.ent[i].key) {
            printf(" %s=%d", d.ent[i].key, d.ent[i].val);
            shown++;
        }
    printf("\n");
    for (size_t i = 0; i < d.ent_len; i++)
        free(d.ent[i].key);
    free(d.idx);
    free(d.ent);
    return 0;
}
