/*
 * title: Persistent red-black tree with Okasaki insert and Kahrs delete
 * topic: data_structures
 * covers: persistent tree, red-black invariants, path copying, functional deletion, version sharing
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 1234567u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

enum { R, B };
typedef struct T { int col; struct T *l, *r; int key; } T;

static T **pool;
static size_t pool_n, pool_cap;
static long allocated;

static T *mk(int col, T *l, int key, T *r) {
    T *t = malloc(sizeof *t);
    CHECK(t);
    t->col = col; t->l = l; t->r = r; t->key = key;
    if (pool_n == pool_cap) {
        pool_cap = pool_cap ? pool_cap * 2 : 4096;
        pool = realloc(pool, pool_cap * sizeof *pool);
        CHECK(pool);
    }
    pool[pool_n++] = t;
    allocated++;
    return t;
}
static int is_red(T *t) { return t && t->col == R; }
static int is_black(T *t) { return t && t->col == B; }

/* Okasaki balance: builds a black node, fixing a red-red violation below it. */
static T *balance(T *a, int x, T *b) {
    if (is_red(a) && is_red(a->l))
        return mk(R, mk(B, a->l->l, a->l->key, a->l->r), a->key, mk(B, a->r, x, b));
    if (is_red(a) && is_red(a->r))
        return mk(R, mk(B, a->l, a->key, a->r->l), a->r->key, mk(B, a->r->r, x, b));
    if (is_red(b) && is_red(b->l))
        return mk(R, mk(B, a, x, b->l->l), b->l->key, mk(B, b->l->r, b->key, b->r));
    if (is_red(b) && is_red(b->r))
        return mk(R, mk(B, a, x, b->l), b->key, mk(B, b->r->l, b->r->key, b->r->r));
    return mk(B, a, x, b);
}

static T *ins(T *t, int k) {
    if (!t) return mk(R, NULL, k, NULL);
    if (k < t->key) return t->col == B ? balance(ins(t->l, k), t->key, t->r) : mk(R, ins(t->l, k), t->key, t->r);
    if (k > t->key) return t->col == B ? balance(t->l, t->key, ins(t->r, k)) : mk(R, t->l, t->key, ins(t->r, k));
    return t;
}
static T *insert(T *t, int k) {
    T *n = ins(t, k);
    return n->col == B ? n : mk(B, n->l, n->key, n->r);
}

static T *sub1(T *t) {
    CHECK(is_black(t));
    return mk(R, t->l, t->key, t->r);
}
static T *balleft(T *l, int x, T *r) {
    if (is_red(l)) return mk(R, mk(B, l->l, l->key, l->r), x, r);
    if (is_black(r)) return balance(l, x, mk(R, r->l, r->key, r->r));
    CHECK(is_red(r) && is_black(r->l));
    return mk(R, mk(B, l, x, r->l->l), r->l->key, balance(r->l->r, r->key, sub1(r->r)));
}
static T *balright(T *l, int x, T *r) {
    if (is_red(r)) return mk(R, l, x, mk(B, r->l, r->key, r->r));
    if (is_black(l)) return balance(mk(R, l->l, l->key, l->r), x, r);
    CHECK(is_red(l) && is_black(l->r));
    return mk(R, balance(sub1(l->l), l->key, l->r->l), l->r->key, mk(B, l->r->r, x, r));
}
static T *app(T *a, T *b) {
    if (!a) return b;
    if (!b) return a;
    if (a->col == R && b->col == R) {
        T *m = app(a->r, b->l);
        if (is_red(m))
            return mk(R, mk(R, a->l, a->key, m->l), m->key, mk(R, m->r, b->key, b->r));
        return mk(R, a->l, a->key, mk(R, m, b->key, b->r));
    }
    if (a->col == B && b->col == B) {
        T *m = app(a->r, b->l);
        if (is_red(m))
            return mk(R, mk(B, a->l, a->key, m->l), m->key, mk(B, m->r, b->key, b->r));
        return balleft(a->l, a->key, mk(B, m, b->key, b->r));
    }
    if (b->col == R) return mk(R, app(a, b->l), b->key, b->r);
    return mk(R, a->l, a->key, app(a->r, b));
}
static T *del(T *t, int k) {
    if (!t) return NULL;
    if (k < t->key) {
        if (is_black(t->l)) return balleft(del(t->l, k), t->key, t->r);
        return mk(R, del(t->l, k), t->key, t->r);
    }
    if (k > t->key) {
        if (is_black(t->r)) return balright(t->l, t->key, del(t->r, k));
        return mk(R, t->l, t->key, del(t->r, k));
    }
    return app(t->l, t->r);
}
static T *erase(T *t, int k) {
    T *n = del(t, k);
    if (!n) return NULL;
    return n->col == B ? n : mk(B, n->l, n->key, n->r);
}

static int contains(T *t, int k) {
    while (t) {
        if (k == t->key) return 1;
        t = k < t->key ? t->l : t->r;
    }
    return 0;
}

/* returns black height, or -1 on violation; also checks order via bounds */
static int validate(T *t, int lo, int hi, int parent_red) {
    if (!t) return 1;
    if (t->key <= lo || t->key >= hi) return -1;
    if (t->col == R && parent_red) return -1;
    int a = validate(t->l, lo, t->key, t->col == R);
    int b = validate(t->r, t->key, hi, t->col == R);
    if (a < 0 || b < 0 || a != b) return -1;
    return a + (t->col == B);
}
static int height(T *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static int collect(T *t, int *out, int n) {
    if (!t) return n;
    n = collect(t->l, out, n);
    out[n++] = t->key;
    return collect(t->r, out, n);
}

#define UNIV 400
#define VERS 12

int main(void) {
    T *root[VERS];
    unsigned char model[VERS][UNIV];
    for (int i = 0; i < VERS; i++) {
        root[i] = NULL;
        for (int k = 0; k < UNIV; k++) model[i][k] = 0;
    }
    long ins_ops = 0, del_ops = 0;
    int maxh = 0;
    for (int step = 0; step < 5000; step++) {
        int src = (int)(rnd() % VERS);
        int dst = (int)(rnd() % VERS);
        int k = (int)(rnd() % UNIV);
        int del_it = (rnd() % 100) < 40;
        T *nr = del_it ? erase(root[src], k) : insert(root[src], k);
        for (int i = 0; i < UNIV; i++) model[dst][i] = model[src][i];
        model[dst][k] = del_it ? 0 : 1;
        root[dst] = nr;
        if (del_it) del_ops++; else ins_ops++;
        CHECK(!nr || nr->col == B);
        CHECK(validate(nr, -1, UNIV + 1, 0) > 0);
        CHECK(contains(nr, k) == (del_it ? 0 : 1));
        int h = height(nr);
        if (h > maxh) maxh = h;
        if (step % 250 == 0) {
            for (int v = 0; v < VERS; v++)
                for (int x = 0; x < UNIV; x += 7) CHECK(contains(root[v], x) == model[v][x]);
        }
    }
    long total = 0;
    for (int v = 0; v < VERS; v++) {
        int buf[UNIV];
        int n = collect(root[v], buf, 0);
        int cnt = 0;
        for (int x = 0; x < UNIV; x++) {
            if (model[v][x]) { CHECK(cnt < n && buf[cnt] == x); cnt++; }
        }
        CHECK(cnt == n);
        total += n;
        printf("version %2d: %3d keys, height %d, black height %d\n", v, n, height(root[v]), validate(root[v], -1, UNIV + 1, 0) - 1);
    }
    printf("inserts=%ld deletes=%ld total keys=%ld max height=%d nodes allocated=%ld\n", ins_ops, del_ops, total, maxh, allocated);
    /* sequential insert stays logarithmic */
    T *seq = NULL;
    for (int i = 0; i < 300; i++) seq = insert(seq, i);
    printf("sequential 300: height %d black height %d\n", height(seq), validate(seq, -1, 1000, 0) - 1);
    for (int i = 0; i < 300; i += 2) seq = erase(seq, i);
    CHECK(validate(seq, -1, 1000, 0) > 0);
    int buf[300];
    int n = collect(seq, buf, 0);
    CHECK(n == 150 && buf[0] == 1 && buf[149] == 299);
    printf("after erasing evens: %d keys, height %d\n", n, height(seq));
    for (size_t i = 0; i < pool_n; i++) free(pool[i]);
    free(pool);
    return 0;
}
