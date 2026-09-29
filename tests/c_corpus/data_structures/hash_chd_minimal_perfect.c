/*
 * title: CHD minimal perfect hash (hash, displace, compress)
 * topic: data_structures
 * covers: minimal perfect hashing, CHD, bucket ordering by size, displacement search, bijection check, string keys
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
    uint32_t nb, m;
    uint32_t *disp; /* per-bucket displacement seed */
    long tries, maxd;
} MPH;

static uint64_t hstr(const char *s, uint64_t seed) {
    uint64_t h = 1469598103934665603ull ^ seed;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return mix64(h);
}

static uint32_t slot_of(const MPH *f, const char *k, uint32_t d) { return (uint32_t)(hstr(k, 0x51ED270Bull * (d + 1)) % f->m); }
static uint32_t bucket_of(const MPH *f, const char *k) { return (uint32_t)(hstr(k, 0) % f->nb); }

typedef struct {
    uint32_t id, size;
} BSize;
static int cmp_bs(const void *x, const void *y) {
    const BSize *a = x, *b = y;
    if (a->size != b->size)
        return a->size > b->size ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id;
}

static int mph_build(MPH *f, char **keys, uint32_t n, uint32_t lambda) {
    f->m = n;
    f->nb = (n + lambda - 1) / lambda;
    f->disp = calloc(f->nb, sizeof(uint32_t));
    f->tries = f->maxd = 0;
    uint32_t **bk = calloc(f->nb, sizeof(uint32_t *));
    BSize *bs = calloc(f->nb, sizeof(BSize));
    for (uint32_t b = 0; b < f->nb; b++)
        bs[b].id = b;
    for (uint32_t i = 0; i < n; i++)
        bs[bucket_of(f, keys[i])].size++;
    for (uint32_t b = 0; b < f->nb; b++)
        bk[b] = malloc((bs[b].size + 1) * sizeof(uint32_t));
    uint32_t *fill = calloc(f->nb, sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = bucket_of(f, keys[i]);
        bk[b][fill[b]++] = i;
    }
    BSize *order = malloc(f->nb * sizeof(BSize));
    memcpy(order, bs, f->nb * sizeof(BSize));
    qsort(order, f->nb, sizeof(BSize), cmp_bs); /* total order: size desc, id asc */
    uint8_t *taken = calloc(n, 1);
    uint32_t *tmp = malloc(64 * sizeof(uint32_t));
    int ok = 1;
    for (uint32_t oi = 0; oi < f->nb && ok; oi++) {
        uint32_t b = order[oi].id, sz = order[oi].size;
        if (sz == 0)
            continue;
        check(sz <= 64, "bucket size bound");
        int placed = 0;
        for (uint32_t d = 0; d < 4000000 && !placed; d++) {
            f->tries++;
            int good = 1;
            for (uint32_t j = 0; j < sz && good; j++) {
                uint32_t s = slot_of(f, keys[bk[b][j]], d);
                if (taken[s])
                    good = 0;
                for (uint32_t q = 0; q < j && good; q++)
                    if (tmp[q] == s)
                        good = 0;
                tmp[j] = s;
            }
            if (good) {
                for (uint32_t j = 0; j < sz; j++)
                    taken[tmp[j]] = 1;
                f->disp[b] = d;
                if ((long)d > f->maxd)
                    f->maxd = d;
                placed = 1;
            }
        }
        ok = placed;
    }
    for (uint32_t b = 0; b < f->nb; b++)
        free(bk[b]);
    free(bk);
    free(bs);
    free(order);
    free(taken);
    free(tmp);
    free(fill);
    return ok;
}

static uint32_t mph_index(const MPH *f, const char *k) { return slot_of(f, k, f->disp[bucket_of(f, k)]); }

int main(void) {
    enum { N = 1500 };
    static char store[N][14];
    char *keys[N];
    for (int i = 0; i < N; i++) {
        snprintf(store[i], sizeof store[i], "%c%c-%u", (int)('a' + rnd() % 26), (int)('a' + rnd() % 26), (unsigned)(rnd() % 100000) + 100000u * (unsigned)i);
        keys[i] = store[i];
    }
    static const uint32_t lambdas[3] = {2, 4, 5};
    for (int t = 0; t < 3; t++) {
        MPH f;
        int ok = mph_build(&f, keys, N, lambdas[t]);
        check(ok, "construction");
        uint8_t *seen = calloc(N, 1);
        for (int i = 0; i < N; i++) {
            uint32_t ix = mph_index(&f, keys[i]);
            check(ix < N && !seen[ix], "bijection onto [0,n)");
            seen[ix] = 1;
        }
        for (int i = 0; i < N; i++)
            check(seen[i], "minimal: every slot used");
        int bits = 0;
        while ((1L << bits) <= f.maxd)
            bits++;
        printf("lambda=%u buckets=%u max displacement=%ld search steps=%ld (%.2f bits/key at %d-bit seeds)\n", lambdas[t], f.nb, f.maxd,
               f.tries, (double)bits * f.nb / N, bits);
        free(seen);
        free(f.disp);
    }
    /* use the index as a dense array position: value lookup by key */
    MPH f;
    check(mph_build(&f, keys, N, 4), "rebuild");
    int *vals = malloc(N * sizeof(int));
    for (int i = 0; i < N; i++)
        vals[mph_index(&f, keys[i])] = i * 3 + 1;
    long sum = 0;
    for (int i = 0; i < N; i++) {
        check(vals[mph_index(&f, keys[i])] == i * 3 + 1, "dense value array");
        sum += vals[mph_index(&f, keys[i])];
    }
    printf("value checksum=%ld example: %s -> slot %u\n", sum, keys[0], mph_index(&f, keys[0]));
    free(vals);
    free(f.disp);
    return 0;
}
