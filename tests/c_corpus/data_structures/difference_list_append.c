/*
 * title: Difference lists with constant-time append
 * topic: data_structures
 * covers: difference list, lazy concatenation tree, iterative flatten, quadratic vs linear append cost
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 161803u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

/* A difference list is a description of a sequence that is materialized once, at the end. */
typedef struct DL {
    enum { D_EMPTY, D_ONE, D_CAT } kind;
    int val;
    const struct DL *a, *b;
    long len;
} DL;

static DL **pool;
static size_t pool_n, pool_cap;
static long dl_nodes;
static const DL *node(int kind, int val, const DL *a, const DL *b, long len) {
    DL *d = malloc(sizeof *d);
    CHECK(d);
    d->kind = (int)kind; d->val = val; d->a = a; d->b = b; d->len = len;
    if (pool_n == pool_cap) {
        pool_cap = pool_cap ? pool_cap * 2 : 1024;
        pool = realloc(pool, pool_cap * sizeof *pool);
        CHECK(pool);
    }
    pool[pool_n++] = d;
    dl_nodes++;
    return d;
}
static const DL *dl_empty(void) { return node(D_EMPTY, 0, NULL, NULL, 0); }
static const DL *dl_one(int v) { return node(D_ONE, v, NULL, NULL, 1); }
static const DL *dl_append(const DL *a, const DL *b) {
    if (a->len == 0) return b;
    if (b->len == 0) return a;
    return node(D_CAT, 0, a, b, a->len + b->len);
}
static const DL *dl_cons(int v, const DL *d) { return dl_append(dl_one(v), d); }
static const DL *dl_snoc(const DL *d, int v) { return dl_append(d, dl_one(v)); }

/* iterative in-order flatten with an explicit stack: no recursion depth problems on left-nested chains */
static long dl_to_array(const DL *d, int *out) {
    const DL **stack = malloc((size_t)(d->len + 2) * sizeof *stack);
    CHECK(stack);
    size_t sp = 0;
    long n = 0;
    stack[sp++] = d;
    while (sp) {
        const DL *t = stack[--sp];
        if (t->kind == D_ONE) out[n++] = t->val;
        else if (t->kind == D_CAT) {
            stack[sp++] = t->b;
            stack[sp++] = t->a;
        }
    }
    free(stack);
    return n;
}

/* the strict alternative: plain arrays where append copies both sides */
typedef struct { int *v; long n; } Arr;
static long copy_cost;
static Arr arr_append(Arr a, Arr b) {
    Arr r;
    r.n = a.n + b.n;
    r.v = malloc((size_t)(r.n + 1) * sizeof(int));
    CHECK(r.v);
    if (a.n) memcpy(r.v, a.v, (size_t)a.n * sizeof(int));
    if (b.n) memcpy(r.v + a.n, b.v, (size_t)b.n * sizeof(int));
    copy_cost += r.n;
    return r;
}

/* flattening a random rose tree of numbers: naive appends versus difference lists */
typedef struct Rose { int val; int nk; struct Rose *kids[4]; } Rose;
static Rose *rose_make(int depth) {
    Rose *r = malloc(sizeof *r);
    CHECK(r);
    r->val = (int)(rnd() % 1000);
    r->nk = 0;
    if (depth > 0) {
        int k = (int)(rnd() % 5);
        if (depth > 6 && k > 2) k = 2;
        for (int i = 0; i < k; i++) r->kids[r->nk++] = rose_make(depth - 1);
    }
    return r;
}
static void rose_free(Rose *r) {
    for (int i = 0; i < r->nk; i++) rose_free(r->kids[i]);
    free(r);
}
static Arr flatten_naive(const Rose *r) {
    int *one = malloc(sizeof(int));
    CHECK(one);
    one[0] = r->val;
    Arr acc = { one, 1 };
    for (int i = 0; i < r->nk; i++) {
        Arr k = flatten_naive(r->kids[i]);
        Arr n = arr_append(acc, k);
        free(acc.v);
        free(k.v);
        acc = n;
    }
    return acc;
}
static const DL *flatten_dl(const Rose *r) {
    const DL *acc = dl_one(r->val);
    for (int i = 0; i < r->nk; i++) acc = dl_append(acc, flatten_dl(r->kids[i]));
    return acc;
}

int main(void) {
    /* left-nested snoc chain: ((((a)++b)++c)++d) */
    enum { N = 20000 };
    const DL *d = dl_empty();
    for (int i = 0; i < N; i++) d = dl_snoc(d, i);
    int *flat = malloc((size_t)N * sizeof(int));
    CHECK(flat);
    long n = dl_to_array(d, flat);
    CHECK(n == N);
    for (int i = 0; i < N; i++) CHECK(flat[i] == i);
    printf("left-nested snoc chain: %d appends, %ld nodes, flattened in order\n", N, dl_nodes);
    free(flat);

    /* mixed cons, snoc and general append against a model array */
    dl_nodes = 0;
    enum { CAPM = 4000 };
    static int model[CAPM];
    int mn = 0;
    const DL *acc = dl_empty();
    for (int step = 0; step < 600; step++) {
        unsigned op = rnd() % 3;
        int v = (int)(rnd() % 100);
        if (op == 0 && mn + 1 < CAPM) {
            acc = dl_cons(v, acc);
            memmove(model + 1, model, (size_t)mn * sizeof(int));
            model[0] = v; mn++;
        } else if (op == 1 && mn + 1 < CAPM) {
            acc = dl_snoc(acc, v);
            model[mn++] = v;
        } else if (mn + 20 < CAPM) {
            const DL *chunk = dl_empty();
            int k = (int)(rnd() % 8) + 1;
            for (int i = 0; i < k; i++) {
                int w = (int)(rnd() % 100);
                chunk = dl_snoc(chunk, w);
                model[mn + i] = w;
            }
            acc = dl_append(acc, chunk);
            mn += k;
        }
        CHECK(acc->len == mn);
    }
    int *out = malloc((size_t)(mn + 1) * sizeof(int));
    CHECK(out);
    CHECK(dl_to_array(acc, out) == mn);
    CHECK(memcmp(out, model, (size_t)mn * sizeof(int)) == 0);
    free(out);
    printf("mixed build: %d elements, %ld nodes\n", mn, dl_nodes);

    /* flatten random rose trees both ways and compare cost */
    long naive_total = 0, dl_total = 0, elems = 0;
    for (int t = 0; t < 6; t++) {
        Rose *r = rose_make(9);
        copy_cost = 0;
        Arr a = flatten_naive(r);
        long before = dl_nodes;
        const DL *df = flatten_dl(r);
        int *o = malloc((size_t)(df->len + 1) * sizeof(int));
        CHECK(o);
        CHECK(dl_to_array(df, o) == a.n && df->len == a.n);
        CHECK(memcmp(o, a.v, (size_t)a.n * sizeof(int)) == 0);
        naive_total += copy_cost;
        dl_total += dl_nodes - before;
        elems += a.n;
        printf("tree %d: %ld values\n", t, a.n);
        free(o);
        free(a.v);
        rose_free(r);
    }
    printf("flatten totals: %ld values, naive copies=%ld, difference-list nodes=%ld\n", elems, naive_total, dl_total);
    CHECK(dl_total < naive_total);
    for (size_t i = 0; i < pool_n; i++) free(pool[i]);
    free(pool);
    return 0;
}
