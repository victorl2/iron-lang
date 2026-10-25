/*
 * title: FKS two-level perfect hashing for a static key set
 * topic: data_structures
 * covers: perfect hashing, FKS, universal hash family, sum of squares bound, quadratic second level, retry loops
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
#define P 4294967311ull /* prime just above 2^32 */

typedef struct {
    uint64_t a, b;
} Hash;
typedef struct {
    Hash h;
    uint32_t m; /* slots = size^2 */
    uint32_t *keys;
    uint8_t *used;
} Sub;
typedef struct {
    Hash h;
    uint32_t nb;
    Sub *sub;
    long l1_tries, l2_tries, slots;
} FKS;

static Hash new_hash(void) {
    Hash h;
    h.a = 1 + rnd() % (P - 1);
    h.b = rnd() % P;
    return h;
}
static uint32_t apply(Hash h, uint32_t k, uint32_t m) { return (uint32_t)(((h.a * k + h.b) % P) % m); }

static int cmp_u32(const void *x, const void *y) {
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return a < b ? -1 : a > b;
}

static void fks_build(FKS *f, const uint32_t *keys, uint32_t n) {
    memset(f, 0, sizeof *f);
    f->nb = n;
    uint32_t *cnt = calloc(n, sizeof(uint32_t));
    for (;;) {
        f->l1_tries++;
        f->h = new_hash();
        memset(cnt, 0, n * sizeof(uint32_t));
        for (uint32_t i = 0; i < n; i++)
            cnt[apply(f->h, keys[i], n)]++;
        uint64_t sq = 0;
        for (uint32_t i = 0; i < n; i++)
            sq += (uint64_t)cnt[i] * cnt[i];
        if (sq < 4ull * n)
            break;
    }
    f->sub = calloc(n, sizeof(Sub));
    uint32_t **members = malloc(n * sizeof(uint32_t *));
    uint32_t *fill = calloc(n, sizeof(uint32_t));
    for (uint32_t b = 0; b < n; b++)
        members[b] = malloc((cnt[b] + 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = apply(f->h, keys[i], n);
        members[b][fill[b]++] = keys[i];
    }
    for (uint32_t b = 0; b < n; b++) {
        Sub *s = &f->sub[b];
        s->m = cnt[b] * cnt[b];
        f->slots += s->m;
        if (!s->m)
            continue;
        s->keys = malloc(s->m * sizeof(uint32_t));
        s->used = malloc(s->m);
        for (;;) {
            f->l2_tries++;
            s->h = new_hash();
            memset(s->used, 0, s->m);
            int ok = 1;
            for (uint32_t j = 0; j < cnt[b] && ok; j++) {
                uint32_t pos = apply(s->h, members[b][j], s->m);
                if (s->used[pos])
                    ok = 0;
                else {
                    s->used[pos] = 1;
                    s->keys[pos] = members[b][j];
                }
            }
            if (ok)
                break;
        }
        free(members[b]);
    }
    for (uint32_t b = 0; b < n; b++)
        if (!f->sub[b].m)
            free(members[b]);
    free(members);
    free(fill);
    free(cnt);
}

static int fks_has(const FKS *f, uint32_t k) {
    const Sub *s = &f->sub[apply(f->h, k, f->nb)];
    if (!s->m)
        return 0;
    uint32_t pos = apply(s->h, k, s->m);
    return s->used[pos] && s->keys[pos] == k;
}

static void fks_free(FKS *f) {
    for (uint32_t b = 0; b < f->nb; b++) {
        free(f->sub[b].keys);
        free(f->sub[b].used);
    }
    free(f->sub);
}

int main(void) {
    static const uint32_t sizes[3] = {50, 500, 3000};
    for (int t = 0; t < 3; t++) {
        uint32_t n = sizes[t];
        uint32_t *keys = malloc(n * sizeof(uint32_t));
        for (uint32_t i = 0; i < n;) {
            uint32_t k = (uint32_t)(rnd() & 0x3fffffff);
            int dup = 0;
            for (uint32_t j = 0; j < i && !dup; j++)
                dup = keys[j] == k;
            if (!dup)
                keys[i++] = k;
        }
        FKS f;
        fks_build(&f, keys, n);
        for (uint32_t i = 0; i < n; i++)
            check(fks_has(&f, keys[i]), "member found");
        /* non-members checked against a sorted array reference */
        uint32_t *sorted = malloc(n * sizeof(uint32_t));
        memcpy(sorted, keys, n * sizeof(uint32_t));
        qsort(sorted, n, sizeof(uint32_t), cmp_u32);
        long nonmem = 0;
        for (int q = 0; q < 20000; q++) {
            uint32_t k = (uint32_t)(rnd() & 0x3fffffff);
            int want = bsearch(&k, sorted, n, sizeof(uint32_t), cmp_u32) != NULL;
            check(fks_has(&f, k) == want, "query matches reference");
            nonmem += !want;
        }
        printf("n=%u: level1 tries=%ld level2 tries=%ld total second-level slots=%ld (%.2f per key) nonmember queries=%ld\n", n,
               f.l1_tries, f.l2_tries, f.slots, (double)f.slots / (double)n, nonmem);
        check(f.slots < 4 * (long)n, "expected linear space");
        fks_free(&f);
        free(sorted);
        free(keys);
    }
    return 0;
}
