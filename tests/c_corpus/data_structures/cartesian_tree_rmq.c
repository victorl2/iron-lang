/*
 * title: Cartesian tree by stack construction with range-minimum queries
 * topic: data_structures
 * covers: Cartesian tree, linear-time stack construction, heap order and inorder invariants, tie handling, RMQ by descent, index-based nodes, brute-force cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 2048

static unsigned long long rs = 88172645463325252ULL;
unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}
void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) exit(2);
    return p;
}

#define NMAX 400

typedef struct {
    int n, root;
    int l[NMAX], r[NMAX], p[NMAX];   /* -1 for none */
} CT;

static int a[NMAX];

/* linear-time construction: min-heap on values, ties keep the leftmost as ancestor */
static void build_stack(CT *t, const int *v, int n) {
    int st[NMAX], sp = 0;
    t->n = n;
    for (int i = 0; i < n; i++) t->l[i] = t->r[i] = t->p[i] = -1;
    for (int i = 0; i < n; i++) {
        int last = -1;
        while (sp && v[st[sp - 1]] > v[i]) last = st[--sp];
        t->l[i] = last;
        if (last >= 0) t->p[last] = i;
        if (sp) { t->r[st[sp - 1]] = i; t->p[i] = st[sp - 1]; }
        st[sp++] = i;
    }
    t->root = st[0];
}
/* quadratic reference: recursive leftmost minimum */
static int build_rec(CT *t, const int *v, int lo, int hi, int parent) {
    if (lo >= hi) return -1;
    int m = lo;
    for (int i = lo + 1; i < hi; i++) if (v[i] < v[m]) m = i;
    t->p[m] = parent;
    t->l[m] = build_rec(t, v, lo, m, m);
    t->r[m] = build_rec(t, v, m + 1, hi, m);
    return m;
}
static int same_tree(const CT *x, const CT *y) {
    if (x->n != y->n || x->root != y->root) return 0;
    for (int i = 0; i < x->n; i++)
        if (x->l[i] != y->l[i] || x->r[i] != y->r[i] || x->p[i] != y->p[i]) return 0;
    return 1;
}
static int verify(const CT *t, const int *v, int node, int lo, int hi, int depth, int *maxd, long *sumd) {
    if (node < 0) return 0;
    check(node >= lo && node < hi, "inorder range (index order)");
    if (depth > *maxd) *maxd = depth;
    *sumd += depth;
    if (t->l[node] >= 0) check(v[t->l[node]] >= v[node], "heap order left");
    if (t->r[node] >= 0) check(v[t->r[node]] >= v[node], "heap order right");
    if (t->l[node] >= 0) check(t->p[t->l[node]] == node, "parent of left");
    if (t->r[node] >= 0) check(t->p[t->r[node]] == node, "parent of right");
    return 1 + verify(t, v, t->l[node], lo, node, depth + 1, maxd, sumd)
             + verify(t, v, t->r[node], node + 1, hi, depth + 1, maxd, sumd);
}
/* range minimum by descent: the first node whose index falls in [i, j] is the LCA */
static int rmq(const CT *t, int i, int j, int *steps) {
    int x = t->root;
    while (x < i || x > j) {
        x = x < i ? t->r[x] : t->l[x];
        (*steps)++;
    }
    return x;
}
static int brute_argmin(const int *v, int i, int j) {
    int m = i;
    for (int k = i + 1; k <= j; k++) if (v[k] < v[m]) m = k;
    return m;
}
static void fill(int kind, int n) {
    for (int i = 0; i < n; i++) {
        switch (kind) {
        case 0: a[i] = (int)(rnd() % 1000); break;
        case 1: a[i] = i; break;
        case 2: a[i] = n - i; break;
        case 3: a[i] = 5; break;
        case 4: a[i] = (i % 10) * 3; break;
        default: a[i] = (int)(rnd() % 6); break;
        }
    }
}

int main(void) {
    static CT fast, slow;
    static const char *names[6] = {"random", "ascending", "descending", "constant", "sawtooth", "few values"};
    long total_steps = 0, queries = 0;
    for (int kind = 0; kind < 6; kind++) {
        int n = kind == 0 ? 300 : 200;
        fill(kind, n);
        build_stack(&fast, a, n);
        slow.n = n;
        slow.root = build_rec(&slow, a, 0, n, -1);
        check(same_tree(&fast, &slow), "stack build equals recursive build");
        int maxd = 0;
        long sumd = 0;
        check(verify(&fast, a, fast.root, 0, n, 0, &maxd, &sumd) == n, "tree covers all indices");
        long steps = 0;
        int q = 0;
        for (int i = 0; i < n; i += 7)
            for (int j = i; j < n; j += 11) {
                int s = 0;
                int got = rmq(&fast, i, j, &s);
                check(got == brute_argmin(a, i, j), "rmq equals brute force");
                steps += s; q++;
            }
        total_steps += steps; queries += q;
        printf("%-10s n=%d root=%d value=%d height=%d depth sum=%ld rmq queries=%d steps=%ld\n",
               names[kind], n, fast.root, a[fast.root], maxd + 1, sumd, q, steps);
    }
    printf("total rmq queries=%ld steps=%ld\n", queries, total_steps);
    /* treap view: keys sorted, priorities as values (heap order), inorder gives keys */
    int n = 64;
    for (int i = 0; i < n; i++) a[i] = (int)(rnd() % 100000);
    build_stack(&fast, a, n);
    int maxd = 0; long sumd = 0;
    check(verify(&fast, a, fast.root, 0, n, 0, &maxd, &sumd) == n, "treap");
    /* parent relation: parent is the nearer-valued of previous-smaller and next-smaller */
    for (int i = 0; i < n; i++) {
        int ps = -1, ns = -1;
        for (int j = i - 1; j >= 0; j--) if (a[j] < a[i]) { ps = j; break; }
        for (int j = i + 1; j < n; j++) if (a[j] <= a[i]) { ns = j; break; }
        (void)ns;
        int want;
        int ns2 = -1;
        for (int j = i + 1; j < n; j++) if (a[j] < a[i]) { ns2 = j; break; }
        if (ps < 0) want = ns2;
        else if (ns2 < 0) want = ps;
        else want = a[ps] > a[ns2] ? ps : ns2;
        check(fast.p[i] == want, "parent is larger of the two nearest smaller values");
    }
    printf("treap-style: n=%d root index=%d height=%d\n", n, fast.root, maxd + 1);
    return 0;
}
