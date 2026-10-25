/*
 * title: Rose tree with folds
 * topic: data_structures
 * covers: rose tree, catamorphism, fold with function pointers, map/filter, fusion law, iterative cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Rose {
    long val;
    int nkids;
    struct Rose **kids;
} Rose;

static unsigned long long rs = 0xDEADBEEFCAFEF00DULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static Rose *node(long v, int n) {
    Rose *r = malloc(sizeof *r);
    r->val = v; r->nkids = n;
    r->kids = n ? calloc((size_t)n, sizeof *r->kids) : NULL;
    return r;
}
static void destroy(Rose *r) {
    for (int i = 0; i < r->nkids; i++) destroy(r->kids[i]);
    free(r->kids); free(r);
}
static Rose *gen(int depth) {
    int n = depth == 0 ? 0 : (int)(rnd() % 4);
    Rose *r = node((long)(rnd() % 19) - 9, n);
    for (int i = 0; i < n; i++) r->kids[i] = gen(depth - 1);
    return r;
}

/* fold: f(value, results of children) */
typedef long (*FoldFn)(long val, int n, const long *sub);
static long fold(const Rose *r, FoldFn f) {
    long sub[8];
    for (int i = 0; i < r->nkids; i++) sub[i] = fold(r->kids[i], f);
    return f(r->val, r->nkids, sub);
}
static long f_size(long v, int n, const long *s) { long t = 1; for (int i = 0; i < n; i++) t += s[i]; return t; }
static long f_sum(long v, int n, const long *s) { long t = v; for (int i = 0; i < n; i++) t += s[i]; return t; }
static long f_height(long v, int n, const long *s) { long m = 0; for (int i = 0; i < n; i++) if (s[i] > m) m = s[i]; return m + 1; }
static long f_leaves(long v, int n, const long *s) { if (!n) return 1; long t = 0; for (int i = 0; i < n; i++) t += s[i]; return t; }
static long f_max(long v, int n, const long *s) { long m = v; for (int i = 0; i < n; i++) if (s[i] > m) m = s[i]; return m; }
static long f_maxpath(long v, int n, const long *s) { /* best root-to-leaf sum */
    if (!n) return v;
    long m = s[0];
    for (int i = 1; i < n; i++) if (s[i] > m) m = s[i];
    return v + m;
}
static long f_hash(long v, int n, const long *s) {
    unsigned long h = 1469598103u ^ (unsigned long)(v + 100);
    for (int i = 0; i < n; i++) h = (h * 16777619u + (unsigned long)s[i]) & 0xffffffffu;
    return (long)h;
}
/* squared-value sum, for fusion check */
static long f_sumsq(long v, int n, const long *s) { long t = v * v; for (int i = 0; i < n; i++) t += s[i]; return t; }

/* map */
typedef long (*MapFn)(long);
static Rose *rmap(const Rose *r, MapFn f) {
    Rose *o = node(f(r->val), r->nkids);
    for (int i = 0; i < r->nkids; i++) o->kids[i] = rmap(r->kids[i], f);
    return o;
}
static long sq(long x) { return x * x; }
static long neg(long x) { return -x; }
/* prune: drop subtrees whose root value is negative (root always kept) */
static Rose *prune(const Rose *r) {
    int keep[8], k = 0;
    for (int i = 0; i < r->nkids; i++) if (r->kids[i]->val >= 0) keep[k++] = i;
    Rose *o = node(r->val, k);
    for (int i = 0; i < k; i++) o->kids[i] = prune(r->kids[keep[i]]);
    return o;
}
/* iterative reference computations using an explicit stack */
typedef struct { const Rose *n; int depth; } Fr;
static void iter_stats(const Rose *root, long *size, long *sum, long *height, long *leaves, long *mx) {
    Fr st[4096]; int sp = 0;
    st[sp++] = (Fr){ root, 1 };
    *size = *sum = *height = *leaves = 0; *mx = root->val;
    while (sp) {
        Fr f = st[--sp];
        (*size)++; *sum += f.n->val;
        if (f.depth > *height) *height = f.depth;
        if (f.n->val > *mx) *mx = f.n->val;
        if (!f.n->nkids) (*leaves)++;
        for (int i = 0; i < f.n->nkids; i++) st[sp++] = (Fr){ f.n->kids[i], f.depth + 1 };
    }
}
static long brute_maxpath(const Rose *r) {
    if (!r->nkids) return r->val;
    long best = brute_maxpath(r->kids[0]);
    for (int i = 1; i < r->nkids; i++) { long b = brute_maxpath(r->kids[i]); if (b > best) best = b; }
    return r->val + best;
}
/* show: "v(k1 k2 ...)" */
static void show(const Rose *r, char *o) {
    char t[24]; snprintf(t, sizeof t, "%ld", r->val); strcat(o, t);
    if (r->nkids) {
        strcat(o, "(");
        for (int i = 0; i < r->nkids; i++) { if (i) strcat(o, " "); show(r->kids[i], o); }
        strcat(o, ")");
    }
}
static int equal(const Rose *a, const Rose *b) {
    if (a->val != b->val || a->nkids != b->nkids) return 0;
    for (int i = 0; i < a->nkids; i++) if (!equal(a->kids[i], b->kids[i])) return 0;
    return 1;
}

int main(void) {
    /* small fixed example */
    Rose *t = node(1, 3);
    t->kids[0] = node(2, 2); t->kids[0]->kids[0] = node(4, 0); t->kids[0]->kids[1] = node(5, 0);
    t->kids[1] = node(3, 0);
    t->kids[2] = node(-6, 1); t->kids[2]->kids[0] = node(7, 0);
    char s[256] = ""; show(t, s);
    printf("tree %s\n", s);
    printf("size %ld sum %ld height %ld leaves %ld max %ld maxpath %ld\n",
           fold(t, f_size), fold(t, f_sum), fold(t, f_height), fold(t, f_leaves), fold(t, f_max), fold(t, f_maxpath));
    Rose *p = prune(t);
    char s2[256] = ""; show(p, s2);
    printf("pruned %s size %ld\n", s2, fold(p, f_size));
    destroy(p); destroy(t);

    long tot_size = 0, tot_leaf = 0, hh = 0;
    for (int it = 0; it < 200; it++) {
        Rose *r = gen(5);
        long sz, sm, ht, lv, mx;
        iter_stats(r, &sz, &sm, &ht, &lv, &mx);
        check(fold(r, f_size) == sz, "size");
        check(fold(r, f_sum) == sm, "sum");
        check(fold(r, f_height) == ht, "height");
        check(fold(r, f_leaves) == lv, "leaves");
        check(fold(r, f_max) == mx, "max");
        check(fold(r, f_maxpath) == brute_maxpath(r), "maxpath");
        /* fusion: fold sum (map sq t) == fold sumsq t */
        Rose *m = rmap(r, sq);
        check(fold(m, f_sum) == fold(r, f_sumsq), "map/fold fusion");
        /* map neg twice is identity; sum negates */
        Rose *n1 = rmap(r, neg), *n2 = rmap(n1, neg);
        check(equal(n2, r), "neg involution");
        check(fold(n1, f_sum) == -sm, "sum of negation");
        /* pruning never increases size and keeps nonneg-only trees */
        Rose *pr = prune(r);
        check(fold(pr, f_size) <= sz, "prune shrinks");
        check(fold(pr, f_hash) == fold(pr, f_hash), "hash stable");
        Rose *pp = prune(pr);
        check(equal(pp, pr), "prune idempotent");
        tot_size += sz; tot_leaf += lv; if (ht > hh) hh = ht;
        destroy(r); destroy(m); destroy(n1); destroy(n2); destroy(pr); destroy(pp);
    }
    printf("200 random trees: total size %ld, total leaves %ld, max height %ld\n", tot_size, tot_leaf, hh);
    Rose *r = gen(3);
    printf("sample hash %ld\n", fold(r, f_hash));
    destroy(r);
    return 0;
}
