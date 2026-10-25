/*
 * title: Fenwick tree viewed as an explicit tree
 * topic: data_structures
 * covers: fenwick tree, lowbit parent links, responsibility ranges, update and query paths, descent search, range updates with two trees
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 64

static long bit[N + 1];
static unsigned long long rs = 0xFE17A11CULL * 4391;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int lowbit(int i) { return i & -i; }
static void update(long *t, int n, int i, long d) { for (; i <= n; i += lowbit(i)) t[i] += d; }
static long query(const long *t, int i) { long s = 0; for (; i > 0; i -= lowbit(i)) s += t[i]; return s; }
/* smallest index whose prefix sum >= target (all values non-negative) */
static int lower_bound_prefix(const long *t, int n, long target) {
    int pos = 0, step = 1;
    while (step * 2 <= n) step *= 2;
    for (; step; step >>= 1) if (pos + step <= n && t[pos + step] < target) { pos += step; target -= t[pos]; }
    return pos + 1;
}

/* explicit tree: the parent of i is i + lowbit(i); node i covers (i - lowbit(i), i] */
static int par(int i, int n) { int p = i + lowbit(i); return p <= n ? p : 0; }
static void print_tree(int node, int n, int depth) {
    printf("%*s%d covers [%d..%d]\n", depth * 2, "", node, node - lowbit(node) + 1, node);
    /* children of node: j with par(j) == node */
    for (int j = 1; j <= n; j++) if (par(j, n) == node) print_tree(j, n, depth + 1);
}
static int tree_depth(int i, int n) { int d = 0; while (par(i, n)) { i = par(i, n); d++; } return d; }

/* range update + range query with two trees B1, B2 (prefix sum of a = sum(B1)*i - sum(B2)) */
static long b1[N + 2], b2[N + 2];
static void range_add(int l, int r, long v) {
    update(b1, N, l, v); update(b1, N, r + 1, -v);
    update(b2, N, l, v * (l - 1)); update(b2, N, r + 1, -v * r);
}
static long prefix(int i) { return query(b1, i) * i - query(b2, i); }

int main(void) {
    /* the tree for n = 16 */
    printf("fenwick tree for n=16 (root is 16):\n");
    print_tree(16, 16, 0);
    /* structural facts checked for n = 64 */
    int maxdepth = 0, leaves = 0;
    int children[N + 1] = {0};
    for (int i = 1; i <= N; i++) { int p = par(i, N); if (p) children[p]++; }
    for (int i = 1; i <= N; i++) {
        /* number of children of i is log2(lowbit(i)): its range splits into lowbit(i) = 1 + sum of children's ranges */
        int lb = lowbit(i), lg = 0; while ((1 << lg) < lb) lg++;
        check(children[i] == lg, "children count equals log2 of lowbit");
        int covered = 1;
        for (int j = 1; j <= N; j++) if (par(j, N) == i) covered += lowbit(j);
        check(covered == lb, "children ranges tile the parent range");
        if (children[i] == 0) leaves++;
        if (tree_depth(i, N) > maxdepth) maxdepth = tree_depth(i, N);
    }
    check(par(N, N) == 0, "N is the root");
    printf("n=%d: leaves %d, max depth %d, root children %d\n", N, leaves, maxdepth, children[N]);
    /* update path length = depth+1; query path length = popcount(i) */
    long upd_total = 0, qry_total = 0;
    for (int i = 1; i <= N; i++) {
        int steps = 0; for (int j = i; j <= N; j += lowbit(j)) steps++;
        check(steps == tree_depth(i, N) + 1, "update path is the chain to the root");
        upd_total += steps;
        int q = 0; for (int j = i; j > 0; j -= lowbit(j)) q++;
        int pc = 0; for (int x = i; x; x &= x - 1) pc++;
        check(q == pc, "query path length equals popcount");
        qry_total += q;
    }
    printf("total update steps %ld, total query steps %ld over all %d indices\n", upd_total, qry_total, N);
    /* random updates and queries against a plain array */
    long a[N + 1] = {0};
    long chk = 0;
    for (int step = 0; step < 4000; step++) {
        int i = 1 + (int)(rnd() % N);
        if (rnd() % 2) { long d = (long)(rnd() % 50); a[i] += d; update(bit, N, i, d); }
        else {
            int l = 1 + (int)(rnd() % N), r = l + (int)(rnd() % (unsigned)(N - l + 1));
            long want = 0; for (int j = l; j <= r; j++) want += a[j];
            long got = query(bit, r) - query(bit, l - 1);
            check(got == want, "range sum");
            chk += got % 1000;
        }
        if (step % 500 == 0) { /* the value at node i equals the sum over its responsibility range */
            for (int j = 1; j <= N; j++) { long s = 0; for (int x = j - lowbit(j) + 1; x <= j; x++) s += a[x]; check(bit[j] == s, "node holds its range sum"); }
        }
    }
    /* descent search */
    long total = query(bit, N);
    for (int t = 0; t < 300; t++) {
        long target = 1 + (long)(rnd() % (unsigned)(total));
        int got = lower_bound_prefix(bit, N, target), want = 0; long s = 0;
        while (s < target) s += a[++want];
        check(got == want, "descent lower bound");
    }
    printf("random phase checksum %ld, total %ld, descent searches verified\n", chk, total);
    /* range update, range query */
    long ref[N + 2] = {0};
    long chk2 = 0;
    for (int step = 0; step < 3000; step++) {
        int l = 1 + (int)(rnd() % N), r = l + (int)(rnd() % (unsigned)(N - l + 1));
        if (step % 3 == 0) { long v = (long)(rnd() % 21) - 10; range_add(l, r, v); for (int j = l; j <= r; j++) ref[j] += v; }
        else {
            long want = 0; for (int j = l; j <= r; j++) want += ref[j];
            long got = prefix(r) - prefix(l - 1);
            check(got == want, "range update range query");
            chk2 += got;
        }
    }
    printf("range-update phase checksum %ld\n", chk2);
    return 0;
}
