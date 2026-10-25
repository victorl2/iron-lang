/*
 * title: Optimal BST construction with failed-search weights and heuristic comparison
 * topic: data_structures
 * covers: optimal binary search tree, cubic interval DP, root table, dummy keys for unsuccessful searches, tree reconstruction, actual search-cost simulation, brute force over insertion orders, heuristics
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

#define NK 32

static int n;
static int p[NK + 2], q[NK + 2];          /* p[1..n] hits, q[0..n] misses (integer weights) */
static long w[NK + 2][NK + 2], e[NK + 2][NK + 2];
static int root[NK + 2][NK + 2];
static int lc[NK + 2], rc[NK + 2];        /* built tree, 0 = none */

static void optimal(void) {
    for (int i = 1; i <= n + 1; i++) { e[i][i - 1] = q[i - 1]; w[i][i - 1] = q[i - 1]; }
    for (int len = 1; len <= n; len++)
        for (int i = 1; i + len - 1 <= n; i++) {
            int j = i + len - 1;
            w[i][j] = w[i][j - 1] + p[j] + q[j];
            e[i][j] = -1;
            for (int r = i; r <= j; r++) {
                long t = e[i][r - 1] + e[r + 1][j] + w[i][j];
                if (e[i][j] < 0 || t < e[i][j]) { e[i][j] = t; root[i][j] = r; }
            }
        }
}
static int build_from_table(int i, int j) {
    if (i > j) return 0;
    int r = root[i][j];
    lc[r] = build_from_table(i, r - 1);
    rc[r] = build_from_table(r + 1, j);
    return r;
}
/* actual expected search cost: hits cost the nodes visited, misses cost nodes visited + 1 */
static long tree_cost(int rt) {
    long total = 0;
    for (int key = 1; key <= n; key++) {
        int x = rt, visited = 0;
        while (x != key) { visited++; x = key < x ? lc[x] : rc[x]; check(x != 0, "key reachable"); }
        total += (long)p[key] * (visited + 1);
    }
    for (int gap = 0; gap <= n; gap++) {
        /* a miss between key gap and key gap+1 */
        int x = rt, visited = 0;
        while (x) {
            visited++;
            if (x <= gap) x = rc[x]; else x = lc[x];
        }
        total += (long)q[gap] * (visited + 1);
    }
    return total;
}
/* heuristic builders over key range [i, j] */
static int build_mid(int i, int j) {
    if (i > j) return 0;
    int r = (i + j) / 2;
    lc[r] = build_mid(i, r - 1); rc[r] = build_mid(r + 1, j);
    return r;
}
static int build_greedy(int i, int j) {
    if (i > j) return 0;
    int r = i;
    for (int k = i + 1; k <= j; k++) if (p[k] > p[r]) r = k;
    lc[r] = build_greedy(i, r - 1); rc[r] = build_greedy(r + 1, j);
    return r;
}
static int build_wmedian(int i, int j) {
    if (i > j) return 0;
    long total = w[i][j], acc = q[i - 1];
    int r = i;
    for (int k = i; k <= j; k++) {
        acc += p[k] + q[k];
        r = k;
        if (2 * acc >= total) break;
    }
    lc[r] = build_wmedian(i, r - 1); rc[r] = build_wmedian(r + 1, j);
    return r;
}
static void shape(int x, char *b, int *pos) {
    if (!x) { b[(*pos)++] = '.'; return; }
    b[(*pos)++] = '(';
    shape(lc[x], b, pos);
    *pos += snprintf(b + *pos, 8, "%d", x);
    shape(rc[x], b, pos);
    b[(*pos)++] = ')';
}
static int height(int x) {
    if (!x) return 0;
    int a = height(lc[x]), b = height(rc[x]);
    return 1 + (a > b ? a : b);
}
/* independent check: every BST arises from some insertion order */
static long best_by_permutation(void) {
    int perm[NK], used[NK + 2];
    long best = -1;
    int idx[NK + 1];
    int depth = 0;
    (void)perm; (void)used;
    /* iterative permutation enumeration via Heap-free lexicographic next_permutation */
    for (int i = 0; i < n; i++) idx[i] = i + 1;
    for (;;) {
        for (int i = 1; i <= n; i++) lc[i] = rc[i] = 0;
        int rt = idx[0];
        for (int t = 1; t < n; t++) {
            int x = rt, key = idx[t];
            for (;;) {
                if (key < x) { if (!lc[x]) { lc[x] = key; break; } x = lc[x]; }
                else { if (!rc[x]) { rc[x] = key; break; } x = rc[x]; }
            }
        }
        long c = tree_cost(rt);
        if (best < 0 || c < best) best = c;
        int i = n - 2;
        while (i >= 0 && idx[i] > idx[i + 1]) i--;
        if (i < 0) break;
        int j = n - 1;
        while (idx[j] < idx[i]) j--;
        int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
        for (int a = i + 1, b = n - 1; a < b; a++, b--) { t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
    }
    (void)depth;
    return best;
}

int main(void) {
    char buf[512];
    /* CLRS example, weights scaled by 100: optimal expected cost 2.75 */
    n = 5;
    static const int p0[6] = {0, 15, 10, 5, 10, 20};
    static const int q0[6] = {5, 10, 5, 5, 5, 10};
    for (int i = 0; i <= 5; i++) { p[i] = p0[i]; q[i] = q0[i]; }
    optimal();
    int rt = build_from_table(1, n);
    int pos = 0;
    shape(rt, buf, &pos); buf[pos] = 0;
    check(e[1][n] == 275, "CLRS example cost");
    check(tree_cost(rt) == e[1][n], "built tree realizes DP cost");
    printf("CLRS example: cost=%ld root=%d height=%d shape=%s\n", e[1][n], rt, height(rt), buf);
    /* small random instances checked against exhaustive insertion orders */
    for (int trial = 0; trial < 12; trial++) {
        n = 2 + trial % 6;
        for (int i = 0; i <= n; i++) { p[i] = (int)(rnd() % 30); q[i] = (int)(rnd() % 12); }
        p[0] = 0;
        optimal();
        rt = build_from_table(1, n);
        long dp = e[1][n];
        check(tree_cost(rt) == dp, "tree cost equals DP");
        long bf = best_by_permutation();
        check(bf == dp, "brute force over insertion orders equals DP");
        if (trial % 3 == 0) printf("n=%d dp=%ld brute=%ld\n", n, dp, bf);
    }
    /* larger instances against heuristics */
    long sum_opt = 0, sum_mid = 0, sum_greedy = 0, sum_wm = 0;
    for (int trial = 0; trial < 20; trial++) {
        n = 10 + (int)(rnd() % 20);
        for (int i = 0; i <= n; i++) {
            /* skewed weights: a few hot keys */
            p[i] = (rnd() % 5 == 0) ? (int)(rnd() % 200) : (int)(rnd() % 10);
            q[i] = (int)(rnd() % 8);
        }
        p[0] = 0;
        optimal();
        rt = build_from_table(1, n);
        long opt = e[1][n];
        check(tree_cost(rt) == opt, "optimal tree cost");
        long cm = tree_cost(build_mid(1, n));
        long cg = tree_cost(build_greedy(1, n));
        long cw = tree_cost(build_wmedian(1, n));
        check(opt <= cm && opt <= cg && opt <= cw, "optimum beats heuristics");
        sum_opt += opt; sum_mid += cm; sum_greedy += cg; sum_wm += cw;
        if (trial < 4) printf("n=%d optimal=%ld balanced=%ld greedy=%ld weight-median=%ld\n", n, opt, cm, cg, cw);
    }
    printf("20 instances: optimal=%ld balanced=%ld greedy=%ld weight-median=%ld\n", sum_opt, sum_mid, sum_greedy, sum_wm);
    /* uniform weights: optimal tree is perfectly balanced */
    n = 15;
    for (int i = 0; i <= n; i++) { p[i] = 10; q[i] = 0; }
    p[0] = 0;
    optimal();
    rt = build_from_table(1, n);
    printf("uniform 15 keys: cost=%ld height=%d root=%d\n", e[1][n], height(rt), rt);
    check(height(rt) == 4, "balanced height");
    return 0;
}
