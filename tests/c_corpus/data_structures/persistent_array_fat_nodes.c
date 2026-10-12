/*
 * title: Partially persistent array with fat nodes
 * topic: data_structures
 * covers: partial persistence, fat nodes, version stamps, binary search over modifications
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 99991u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct { int ver; int val; } Mod;
typedef struct { Mod *mods; int n, cap; } Fat;
typedef struct { Fat *cells; int len; int version; } PArray;

static void pa_init(PArray *a, int len, int init) {
    a->cells = calloc((size_t)len, sizeof(Fat));
    CHECK(a->cells);
    a->len = len;
    a->version = 0;
    for (int i = 0; i < len; i++) {
        Fat *f = &a->cells[i];
        f->cap = 2;
        f->mods = malloc((size_t)f->cap * sizeof(Mod));
        CHECK(f->mods);
        f->mods[0].ver = 0;
        f->mods[0].val = init;
        f->n = 1;
    }
}
static void pa_free(PArray *a) {
    for (int i = 0; i < a->len; i++) free(a->cells[i].mods);
    free(a->cells);
}
/* the only writable version is the newest; each write creates a new version */
static int pa_set(PArray *a, int i, int val) {
    Fat *f = &a->cells[i];
    if (f->n == f->cap) {
        f->cap *= 2;
        f->mods = realloc(f->mods, (size_t)f->cap * sizeof(Mod));
        CHECK(f->mods);
    }
    a->version++;
    f->mods[f->n].ver = a->version;
    f->mods[f->n].val = val;
    f->n++;
    return a->version;
}
/* value of cell i as of version v: last modification with stamp <= v */
static int pa_get(const PArray *a, int v, int i) {
    const Fat *f = &a->cells[i];
    int lo = 0, hi = f->n - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (f->mods[mid].ver <= v) lo = mid; else hi = mid - 1;
    }
    return f->mods[lo].val;
}
static long pa_range_sum(const PArray *a, int v, int l, int r) {
    long s = 0;
    for (int i = l; i < r; i++) s += pa_get(a, v, i);
    return s;
}

#define LEN 40
#define NVER 600

int main(void) {
    PArray a;
    pa_init(&a, LEN, 0);
    static int snap[NVER + 1][LEN];
    for (int i = 0; i < LEN; i++) snap[0][i] = 0;
    int cur[LEN] = {0};
    for (int v = 1; v <= NVER; v++) {
        /* skewed choice of cell so some fat nodes grow much larger */
        int i = (int)(rnd() % LEN);
        if (rnd() % 3 == 0) i = (int)(rnd() % 4);
        int val = (int)(rnd() % 2001) - 1000;
        int got = pa_set(&a, i, val);
        CHECK(got == v);
        cur[i] = val;
        for (int k = 0; k < LEN; k++) snap[v][k] = cur[k];
    }
    long checked = 0;
    for (int v = 0; v <= NVER; v++)
        for (int i = 0; i < LEN; i++) {
            CHECK(pa_get(&a, v, i) == snap[v][i]);
            checked++;
        }
    for (int t = 0; t < 300; t++) {
        int v = (int)(rnd() % (NVER + 1));
        int l = (int)(rnd() % LEN);
        int r = l + (int)(rnd() % (unsigned)(LEN - l + 1));
        long expect = 0;
        for (int i = l; i < r; i++) expect += snap[v][i];
        CHECK(pa_range_sum(&a, v, l, r) == expect);
    }
    int max_mods = 0, max_cell = 0;
    long total_mods = 0;
    for (int i = 0; i < LEN; i++) {
        total_mods += a.cells[i].n;
        if (a.cells[i].n > max_mods) { max_mods = a.cells[i].n; max_cell = i; }
    }
    CHECK(total_mods == LEN + NVER);
    printf("versions=%d cells=%d point queries verified=%ld\n", NVER, LEN, checked);
    printf("total fat-node entries=%ld, fattest cell=%d with %d entries\n", total_mods, max_cell, max_mods);
    for (int v = 0; v <= NVER; v += 150)
        printf("version %3d: sum=%ld first=%d last=%d\n", v, pa_range_sum(&a, v, 0, LEN), pa_get(&a, v, 0), pa_get(&a, v, LEN - 1));
    pa_free(&a);
    return 0;
}
