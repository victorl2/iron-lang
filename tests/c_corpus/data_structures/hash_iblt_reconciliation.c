/*
 * title: Invertible Bloom lookup table for set reconciliation
 * topic: data_structures
 * covers: IBLT, XOR key sums, pure cell peeling, subtraction of tables, symmetric difference recovery, decode success versus table size
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
enum { HASHES = 3, MAXCELLS = 1024 };
typedef struct {
    int32_t count;
    uint32_t keysum, chk;
} Cell;
typedef struct {
    Cell c[MAXCELLS];
    int m; /* cells; divisible by HASHES */
} IBLT;

static uint32_t chk(uint32_t k) { return mix32(k ^ 0xA5A5A5A5u) | 1u; }
static int cell_of(const IBLT *t, uint32_t key, int j) {
    int sub = t->m / HASHES;
    return j * sub + (int)(mix32(key * 0x9E3779B1u + (uint32_t)j * 0x85EBCA6Bu) % (uint32_t)sub);
}

static void iblt_init(IBLT *t, int m) {
    memset(t, 0, sizeof *t);
    t->m = m - m % HASHES;
}
static void iblt_toggle(IBLT *t, uint32_t key, int delta) {
    for (int j = 0; j < HASHES; j++) {
        Cell *c = &t->c[cell_of(t, key, j)];
        c->count += delta;
        c->keysum ^= key;
        c->chk ^= chk(key);
    }
}
static void iblt_subtract(IBLT *a, const IBLT *b) {
    for (int i = 0; i < a->m; i++) {
        a->c[i].count -= b->c[i].count;
        a->c[i].keysum ^= b->c[i].keysum;
        a->c[i].chk ^= b->c[i].chk;
    }
}

/* peel; returns 1 if the table decodes fully. only_a: keys with +1, only_b: keys with -1 */
static int iblt_decode(IBLT *t, uint32_t *only_a, int *na, uint32_t *only_b, int *nb) {
    *na = *nb = 0;
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < t->m; i++) {
            Cell *c = &t->c[i];
            if ((c->count == 1 || c->count == -1) && chk(c->keysum) == c->chk) {
                uint32_t key = c->keysum;
                int sign = c->count;
                if (sign == 1)
                    only_a[(*na)++] = key;
                else
                    only_b[(*nb)++] = key;
                iblt_toggle(t, key, -sign);
                progress = 1;
            }
        }
    }
    for (int i = 0; i < t->m; i++)
        if (t->c[i].count || t->c[i].keysum || t->c[i].chk)
            return 0;
    return 1;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

int main(void) {
    enum { COMMON = 3000, TRIALS = 200 };
    static uint32_t common[COMMON];
    for (int i = 0; i < COMMON; i++)
        common[i] = (uint32_t)i * 2654435761u + 17u;
    static const int diffs[3] = {10, 50, 200};
    static const int ratio_x100[5] = {90, 120, 150, 200, 300};
    printf("decode success out of %d trials (cells = ratio * symmetric difference size)\n", TRIALS);
    printf("%8s", "d \\ ratio");
    for (int r = 0; r < 5; r++)
        printf(" %6.2f", ratio_x100[r] / 100.0);
    printf("\n");
    for (int di = 0; di < 3; di++) {
        int d = diffs[di];
        printf("%8d", d);
        for (int r = 0; r < 5; r++) {
            int cells = d * ratio_x100[r] / 100;
            if (cells > MAXCELLS)
                cells = MAXCELLS;
            int ok = 0;
            for (int t = 0; t < TRIALS; t++) {
                static IBLT a, b;
                iblt_init(&a, cells);
                iblt_init(&b, cells);
                for (int i = 0; i < COMMON; i++) {
                    iblt_toggle(&a, common[i], 1);
                    iblt_toggle(&b, common[i], 1);
                }
                static uint32_t ta[400], tb[400];
                int nta = 0, ntb = 0;
                for (int i = 0; i < d; i++) {
                    uint32_t k = (uint32_t)rnd() | 0x80000000u; /* disjoint from common keys with high probability */
                    if (rnd() & 1) {
                        ta[nta++] = k;
                        iblt_toggle(&a, k, 1);
                    } else {
                        tb[ntb++] = k;
                        iblt_toggle(&b, k, 1);
                    }
                }
                iblt_subtract(&a, &b);
                static uint32_t ra[400], rb[400];
                int nra, nrb;
                int good = iblt_decode(&a, ra, &nra, rb, &nrb);
                if (good) { /* a full decode must equal the true difference exactly */
                    check(nra == nta && nrb == ntb, "recovered sizes");
                    qsort(ta, (size_t)nta, 4, cmp_u32);
                    qsort(ra, (size_t)nra, 4, cmp_u32);
                    qsort(tb, (size_t)ntb, 4, cmp_u32);
                    qsort(rb, (size_t)nrb, 4, cmp_u32);
                    check(memcmp(ta, ra, (size_t)nta * 4) == 0 && memcmp(tb, rb, (size_t)ntb * 4) == 0, "recovered keys");
                    ok++;
                }
            }
            printf(" %6d", ok);
        }
        printf("\n");
    }
    return 0;
}
