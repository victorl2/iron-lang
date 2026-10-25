/*
 * title: Immutable rope with concat, split, insert and delete sharing structure
 * topic: data_structures
 * covers: persistent rope, weight-annotated concat tree, split by index, rebalancing by rebuild, version sharing
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 1111u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct Rope {
    const struct Rope *l, *r;    /* both NULL for a leaf */
    const char *s;               /* leaf text, not NUL terminated */
    int len, depth;
} Rope;

static void **arena;
static size_t arena_n, arena_cap;
static long nodes_made, rebuilds;
static void *keep(void *p) {
    if (arena_n == arena_cap) {
        arena_cap = arena_cap ? arena_cap * 2 : 1024;
        arena = realloc(arena, arena_cap * sizeof *arena);
        CHECK(arena);
    }
    arena[arena_n++] = p;
    return p;
}
static const Rope *leaf(const char *s, int n) {
    if (n == 0) return NULL;
    Rope *r = keep(calloc(1, sizeof *r));
    CHECK(r);
    char *copy = keep(malloc((size_t)n));
    CHECK(copy);
    memcpy(copy, s, (size_t)n);
    r->s = copy; r->len = n; r->depth = 1;
    nodes_made++;
    return r;
}
static int rlen(const Rope *r) { return r ? r->len : 0; }
static int rdepth(const Rope *r) { return r ? r->depth : 0; }
static const Rope *raw_cat(const Rope *a, const Rope *b) {
    if (!a) return b;
    if (!b) return a;
    Rope *r = keep(calloc(1, sizeof *r));
    CHECK(r);
    r->l = a; r->r = b;
    r->len = a->len + b->len;
    r->depth = 1 + (a->depth > b->depth ? a->depth : b->depth);
    nodes_made++;
    return r;
}
static void collect_leaves(const Rope *r, const Rope **out, int *n) {
    if (!r) return;
    if (!r->l) { out[(*n)++] = r; return; }
    collect_leaves(r->l, out, n);
    collect_leaves(r->r, out, n);
}
static const Rope *build(const Rope **lv, int lo, int hi) {
    if (hi - lo == 1) return lv[lo];
    int mid = (lo + hi) / 2;
    return raw_cat(build(lv, lo, mid), build(lv, mid, hi));
}
static int ilog2(int x) { int k = 0; while ((1 << k) < x) k++; return k; }
/* concat that rebuilds from the leaves when the tree gets lopsided; the inputs stay untouched */
static const Rope *cat(const Rope *a, const Rope *b) {
    const Rope *r = raw_cat(a, b);
    if (!r || !r->l) return r;
    int nleaves = 0;
    static const Rope *lv[4096];
    if (r->depth > 2 * ilog2(r->len + 1) + 6) {
        collect_leaves(r, lv, &nleaves);
        CHECK(nleaves < 4096);
        rebuilds++;
        return build(lv, 0, nleaves);
    }
    return r;
}
static void split(const Rope *r, int i, const Rope **a, const Rope **b) {
    if (!r) { *a = *b = NULL; return; }
    if (i <= 0) { *a = NULL; *b = r; return; }
    if (i >= r->len) { *a = r; *b = NULL; return; }
    if (!r->l) { *a = leaf(r->s, i); *b = leaf(r->s + i, r->len - i); return; }
    int ll = r->l->len;
    if (i == ll) { *a = r->l; *b = r->r; return; }
    if (i < ll) {
        const Rope *x, *y;
        split(r->l, i, &x, &y);
        *a = x;
        *b = cat(y, r->r);
    } else {
        const Rope *x, *y;
        split(r->r, i - ll, &x, &y);
        *a = cat(r->l, x);
        *b = y;
    }
}
static char at(const Rope *r, int i) {
    while (r->l) {
        if (i < r->l->len) r = r->l; else { i -= r->l->len; r = r->r; }
    }
    return r->s[i];
}
static int flatten(const Rope *r, char *out, int n) {
    if (!r) return n;
    if (!r->l) { memcpy(out + n, r->s, (size_t)r->len); return n + r->len; }
    return flatten(r->r, out, flatten(r->l, out, n));
}
static const Rope *insert(const Rope *r, int pos, const char *s, int n) {
    const Rope *a, *b;
    split(r, pos, &a, &b);
    return cat(cat(a, leaf(s, n)), b);
}
static const Rope *erase(const Rope *r, int from, int to) {
    const Rope *a, *b, *c, *d;
    split(r, from, &a, &b);
    split(b, to - from, &c, &d);
    return cat(a, d);
}
static const Rope *sub(const Rope *r, int from, int to) {
    const Rope *a, *b, *c, *d;
    split(r, from, &a, &b);
    split(b, to - from, &c, &d);
    return c;
}

#define NV 8
#define MAXT 2500
int main(void) {
    const Rope *v[NV];
    char model[NV][MAXT + 64];
    int mlen[NV];
    for (int i = 0; i < NV; i++) { v[i] = NULL; mlen[i] = 0; }
    long ins = 0, del = 0, cats = 0, subs = 0;
    int maxdepth = 0;
    for (int step = 0; step < 3500; step++) {
        int i = (int)(rnd() % NV), j = (int)(rnd() % NV);
        unsigned op = rnd() % 10;
        char chunk[24];
        if (op < 4 && mlen[i] < MAXT - 30) {
            int k = 1 + (int)(rnd() % 20);
            for (int x = 0; x < k; x++) chunk[x] = (char)('a' + rnd() % 26);
            int pos = (int)(rnd() % (unsigned)(mlen[i] + 1));
            const Rope *nr = insert(v[i], pos, chunk, k);
            char tmp[MAXT + 64];
            memcpy(tmp, model[i], (size_t)pos);
            memcpy(tmp + pos, chunk, (size_t)k);
            memcpy(tmp + pos + k, model[i] + pos, (size_t)(mlen[i] - pos));
            v[j] = nr;
            memcpy(model[j], tmp, (size_t)(mlen[i] + k));
            mlen[j] = mlen[i] + k;
            ins++;
        } else if (op < 6 && mlen[i] > 0) {
            int a = (int)(rnd() % (unsigned)mlen[i]);
            int b = a + 1 + (int)(rnd() % 25);
            if (b > mlen[i]) b = mlen[i];
            const Rope *nr = erase(v[i], a, b);
            char tmp[MAXT + 64];
            memcpy(tmp, model[i], (size_t)a);
            memcpy(tmp + a, model[i] + b, (size_t)(mlen[i] - b));
            int nl = mlen[i] - (b - a);
            v[j] = nr;
            memcpy(model[j], tmp, (size_t)nl);
            mlen[j] = nl;
            del++;
        } else if (op < 8 && mlen[i] + mlen[j] < MAXT) {
            /* concatenating two versions: result replaces slot i, both inputs stay valid as they were */
            const Rope *nr = cat(v[i], v[j]);
            char tmp[MAXT + 64];
            memcpy(tmp, model[i], (size_t)mlen[i]);
            memcpy(tmp + mlen[i], model[j], (size_t)mlen[j]);
            int nl = mlen[i] + mlen[j];
            v[i] = nr;
            memcpy(model[i], tmp, (size_t)nl);
            mlen[i] = nl;
            j = i;
            cats++;
        } else if (mlen[i] > 0) {
            int a = (int)(rnd() % (unsigned)mlen[i]);
            int b = a + (int)(rnd() % (unsigned)(mlen[i] - a + 1));
            const Rope *s = sub(v[i], a, b);
            CHECK(rlen(s) == b - a);
            char tmp[MAXT + 64];
            int n = flatten(s, tmp, 0);
            CHECK(n == b - a && memcmp(tmp, model[i] + a, (size_t)n) == 0);
            subs++;
        }
        if (rdepth(v[j]) > maxdepth) maxdepth = rdepth(v[j]);
        CHECK(rlen(v[j]) == mlen[j]);
        if (step % 100 == 0)
            for (int q = 0; q < NV; q++) {
                CHECK(rlen(v[q]) == mlen[q]);
                for (int p = 0; p < mlen[q]; p += 31) CHECK(at(v[q], p) == model[q][p]);
            }
    }
    long total = 0;
    for (int q = 0; q < NV; q++) {
        char buf[MAXT + 64];
        int n = flatten(v[q], buf, 0);
        CHECK(n == mlen[q] && memcmp(buf, model[q], (size_t)n) == 0);
        total += n;
    }
    printf("inserts=%ld erases=%ld concats=%ld substrings=%ld\n", ins, del, cats, subs);
    printf("versions hold %ld characters, max depth %d, rebuilds %ld, nodes allocated %ld\n", total, maxdepth, rebuilds, nodes_made);
    for (size_t i = 0; i < arena_n; i++) free(arena[i]);
    free(arena);
    return 0;
}
