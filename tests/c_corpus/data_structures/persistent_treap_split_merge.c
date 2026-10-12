/*
 * title: Persistent treap with split, merge and version grafting
 * topic: data_structures
 * covers: persistent treap, split/merge, hashed priorities, order statistics, structural sharing
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 424242u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct T { int key; unsigned pri; int size; struct T *l, *r; } T;

static T **pool;
static size_t pool_n, pool_cap;
static long allocated;

static unsigned prio_of(int key) {
    unsigned x = (unsigned)key * 2654435761u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
    return x;
}
static int sz(T *t) { return t ? t->size : 0; }
static T *mk(int key, unsigned pri, T *l, T *r) {
    T *t = malloc(sizeof *t);
    CHECK(t);
    t->key = key; t->pri = pri; t->l = l; t->r = r;
    t->size = 1 + sz(l) + sz(r);
    if (pool_n == pool_cap) {
        pool_cap = pool_cap ? pool_cap * 2 : 4096;
        pool = realloc(pool, pool_cap * sizeof *pool);
        CHECK(pool);
    }
    pool[pool_n++] = t;
    allocated++;
    return t;
}
/* split into keys < k and keys >= k */
static void split(T *t, int k, T **a, T **b) {
    if (!t) { *a = *b = NULL; return; }
    if (t->key < k) {
        T *x, *y;
        split(t->r, k, &x, &y);
        *a = mk(t->key, t->pri, t->l, x);
        *b = y;
    } else {
        T *x, *y;
        split(t->l, k, &x, &y);
        *a = x;
        *b = mk(t->key, t->pri, y, t->r);
    }
}
static T *merge(T *a, T *b) {
    if (!a) return b;
    if (!b) return a;
    if (a->pri > b->pri) return mk(a->key, a->pri, a->l, merge(a->r, b));
    return mk(b->key, b->pri, merge(a, b->l), b->r);
}
static int contains(T *t, int k) {
    while (t) {
        if (t->key == k) return 1;
        t = k < t->key ? t->l : t->r;
    }
    return 0;
}
static T *insert(T *t, int k) {
    if (contains(t, k)) return t;
    T *a, *b;
    split(t, k, &a, &b);
    return merge(merge(a, mk(k, prio_of(k), NULL, NULL)), b);
}
static T *erase(T *t, int k) {
    if (!contains(t, k)) return t;
    T *a, *b, *c, *d;
    split(t, k, &a, &b);
    split(b, k + 1, &c, &d);
    return merge(a, d);
}
static int kth(T *t, int i) { /* 0-based */
    while (t) {
        int ls = sz(t->l);
        if (i < ls) t = t->l;
        else if (i == ls) return t->key;
        else { i -= ls + 1; t = t->r; }
    }
    return -1;
}
static int count_less(T *t, int k) {
    int c = 0;
    while (t) {
        if (k <= t->key) t = t->l;
        else { c += sz(t->l) + 1; t = t->r; }
    }
    return c;
}
static int check_heap(T *t) {
    if (!t) return 1;
    if (t->l && t->l->pri > t->pri) return 0;
    if (t->r && t->r->pri > t->pri) return 0;
    if (t->size != 1 + sz(t->l) + sz(t->r)) return 0;
    return check_heap(t->l) && check_heap(t->r);
}
static int height(T *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}

#define UNIV 500
#define VERS 10

int main(void) {
    T *root[VERS];
    unsigned char model[VERS][UNIV];
    for (int i = 0; i < VERS; i++) {
        root[i] = NULL;
        for (int k = 0; k < UNIV; k++) model[i][k] = 0;
    }
    long n_ins = 0, n_del = 0, n_graft = 0, n_cut = 0;
    for (int step = 0; step < 3000; step++) {
        int src = (int)(rnd() % VERS);
        int dst = (int)(rnd() % VERS);
        int other = (int)(rnd() % VERS);
        int k = (int)(rnd() % UNIV);
        unsigned op = rnd() % 10;
        T *nr;
        unsigned char nm[UNIV];
        for (int i = 0; i < UNIV; i++) nm[i] = model[src][i];
        if (op < 5) {
            nr = insert(root[src], k);
            nm[k] = 1;
            n_ins++;
        } else if (op < 7) {
            nr = erase(root[src], k);
            nm[k] = 0;
            n_del++;
        } else if (op < 9) {
            /* graft: keys below k from src, keys from k up from other */
            T *a, *b, *c, *d;
            split(root[src], k, &a, &b);
            split(root[other], k, &c, &d);
            nr = merge(a, d);
            for (int i = 0; i < UNIV; i++) nm[i] = i < k ? model[src][i] : model[other][i];
            n_graft++;
        } else {
            /* cut out the range [k, k+40) */
            int hi = k + 40;
            T *a, *b, *c, *d;
            split(root[src], k, &a, &b);
            split(b, hi, &c, &d);
            nr = merge(a, d);
            for (int i = k; i < hi && i < UNIV; i++) nm[i] = 0;
            n_cut++;
        }
        root[dst] = nr;
        for (int i = 0; i < UNIV; i++) model[dst][i] = nm[i];
        CHECK(check_heap(nr));
        int cnt = 0;
        for (int i = 0; i < UNIV; i++) cnt += nm[i];
        CHECK(sz(nr) == cnt);
    }
    long total = 0;
    for (int v = 0; v < VERS; v++) {
        int cnt = 0;
        for (int x = 0; x < UNIV; x++) {
            CHECK(contains(root[v], x) == model[v][x]);
            CHECK(count_less(root[v], x) == cnt);
            if (model[v][x]) { CHECK(kth(root[v], cnt) == x); cnt++; }
        }
        CHECK(cnt == sz(root[v]));
        CHECK(kth(root[v], cnt) == -1);
        total += cnt;
        printf("version %d: size %3d, height %2d, median %d\n", v, cnt, height(root[v]), cnt ? kth(root[v], cnt / 2) : -1);
    }
    printf("insert=%ld erase=%ld graft=%ld cut=%ld\n", n_ins, n_del, n_graft, n_cut);
    printf("total keys over versions=%ld, nodes allocated=%ld\n", total, allocated);
    for (size_t i = 0; i < pool_n; i++) free(pool[i]);
    free(pool);
    return 0;
}
