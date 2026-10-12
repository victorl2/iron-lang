/*
 * title: Huet zipper over a binary tree with local edits
 * topic: data_structures
 * covers: tree zipper, crumbs, path reversal, edit at focus, reconstruction to root
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 97531u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct T { int val; const struct T *l, *r; } T;
/* crumb: which side we descended into, the parent's value, and the sibling we did not enter */
typedef struct Crumb { int went_left; int val; const T *sibling; const struct Crumb *up; } Crumb;
typedef struct { const T *focus; const Crumb *path; int depth; } Zip;

static void **pool;
static size_t pool_n, pool_cap;
static void *xa(size_t n) {
    void *p = malloc(n);
    CHECK(p);
    if (pool_n == pool_cap) {
        pool_cap = pool_cap ? pool_cap * 2 : 1024;
        pool = realloc(pool, pool_cap * sizeof *pool);
        CHECK(pool);
    }
    pool[pool_n++] = p;
    return p;
}
static const T *mk(int v, const T *l, const T *r) {
    T *t = xa(sizeof *t);
    t->val = v; t->l = l; t->r = r;
    return t;
}
static const Crumb *crumb(int went_left, int val, const T *sib, const Crumb *up) {
    Crumb *c = xa(sizeof *c);
    c->went_left = went_left; c->val = val; c->sibling = sib; c->up = up;
    return c;
}

static Zip z_of(const T *t) { Zip z = { t, NULL, 0 }; return z; }
static int z_can_down(Zip z, int left) { return z.focus && (left ? z.focus->l : z.focus->r) != NULL; }
static Zip z_down(Zip z, int left) {
    CHECK(z.focus);
    const T *child = left ? z.focus->l : z.focus->r;
    const T *sib = left ? z.focus->r : z.focus->l;
    Zip n = { child, crumb(left, z.focus->val, sib, z.path), z.depth + 1 };
    return n;
}
static Zip z_up(Zip z) {
    CHECK(z.path);
    const Crumb *c = z.path;
    const T *p = c->went_left ? mk(c->val, z.focus, c->sibling) : mk(c->val, c->sibling, z.focus);
    Zip n = { p, c->up, z.depth - 1 };
    return n;
}
static Zip z_root(Zip z) { while (z.path) z = z_up(z); return z; }
static Zip z_set(Zip z, int v) { CHECK(z.focus); Zip n = { mk(v, z.focus->l, z.focus->r), z.path, z.depth }; return n; }
static Zip z_graft(Zip z, const T *sub) { Zip n = { sub, z.path, z.depth }; return n; }

/* model: heap-indexed array, 0 means empty, node i has children 2i and 2i+1 */
#define MAXD 6
#define MSIZE (1 << (MAXD + 1))
static void m_clear(int *m, int i) {
    if (i >= MSIZE || !m[i]) return;
    m_clear(m, 2 * i);
    m_clear(m, 2 * i + 1);
    m[i] = 0;
}
static int cmp_model(const T *t, const int *m, int i) {
    if (!t) return i >= MSIZE || m[i] == 0;
    if (i >= MSIZE || m[i] != t->val) return 0;
    return cmp_model(t->l, m, 2 * i) && cmp_model(t->r, m, 2 * i + 1);
}
static int count(const T *t) { return t ? 1 + count(t->l) + count(t->r) : 0; }
static long preorder_sum(const T *t, int *idx) {
    if (!t) return 0;
    long s = (long)t->val * (++*idx);
    s += preorder_sum(t->l, idx);
    s += preorder_sum(t->r, idx);
    return s;
}
/* build a subtree of the model (a single node, or a small random tree) under index i */
static const T *build(int *m, int i, int depth) {
    if (depth > MAXD || i >= MSIZE) return NULL;
    int v = (int)(rnd() % 900) + 1;
    m[i] = v;
    const T *l = rnd() % 3 ? build(m, 2 * i, depth + 1) : NULL;
    const T *r = rnd() % 3 ? build(m, 2 * i + 1, depth + 1) : NULL;
    return mk(v, l, r);
}

int main(void) {
    int model[MSIZE];
    memset(model, 0, sizeof model);
    const T *tree = build(model, 1, 1);
    CHECK(cmp_model(tree, model, 1));
    Zip z = z_of(tree);
    int idx = 1; /* heap index of the focus */
    long moves = 0, sets = 0, grafts = 0, prunes = 0;
    Zip saved = z;
    int saved_idx = 1;
    const T *saved_tree = tree;
    int saved_model[MSIZE];
    memcpy(saved_model, model, sizeof model);
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 12;
        if (op < 3) {
            if (z_can_down(z, 1)) { z = z_down(z, 1); idx = 2 * idx; moves++; }
        } else if (op < 6) {
            if (z_can_down(z, 0)) { z = z_down(z, 0); idx = 2 * idx + 1; moves++; }
        } else if (op < 8) {
            if (z.path) { z = z_up(z); idx /= 2; moves++; }
        } else if (op < 9) {
            int v = (int)(rnd() % 900) + 1;
            z = z_set(z, v);
            model[idx] = v;
            sets++;
        } else if (op < 10) {
            /* graft a fresh small subtree in place of the focus (or at an empty child) */
            if (z.focus) {
                m_clear(model, idx);
                const T *sub = build(model, idx, z.depth + 1);
                z = z_graft(z, sub);
                grafts++;
            }
        } else if (op < 11) {
            /* prune: replace the focus subtree by nothing, then step up */
            if (z.path) {
                m_clear(model, idx);
                z = z_graft(z, NULL);
                z = z_up(z);
                idx /= 2;
                prunes++;
            }
        } else {
            /* jump to root, verify the reconstruction against the model, then come back down the same way */
            Zip r = z_root(z);
            CHECK(cmp_model(r.focus, model, 1));
            z = r;
            idx = 1;
        }
        { int dd = 0; for (int q = idx; q > 1; q /= 2) dd++; CHECK(z.depth == dd); }
        CHECK((z.focus != NULL) == (model[idx] != 0));
        if (z.focus) CHECK(z.focus->val == model[idx]);
    }
    Zip r = z_root(z);
    CHECK(cmp_model(r.focus, model, 1));
    int ord = 0;
    printf("moves=%ld sets=%ld grafts=%ld prunes=%ld\n", moves, sets, grafts, prunes);
    printf("final tree: %d nodes, weighted preorder sum %ld\n", count(r.focus), preorder_sum(r.focus, &ord));
    /* the original tree and the saved zipper were never disturbed */
    CHECK(cmp_model(saved_tree, saved_model, 1));
    Zip sr = z_root(saved);
    CHECK(sr.focus == saved_tree && saved_idx == 1);
    ord = 0;
    printf("original tree: %d nodes, weighted preorder sum %ld\n", count(saved_tree), preorder_sum(saved_tree, &ord));
    for (size_t i = 0; i < pool_n; i++) free(pool[i]);
    free(pool);
    return 0;
}
