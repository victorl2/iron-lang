/*
 * title: Tournament tree (winner tree) sort
 * topic: algorithms
 * covers: selection tree, winner tree, replay along one path, sentinel infinity, power-of-two padding
 * deps: libc
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 1234567u;
static unsigned rng(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    int *val;    /* leaves: values, size m */
    int *win;    /* internal nodes hold index of winning leaf; size m */
    int m;       /* leaf count, power of two */
    long cmps;
} Tree;

static int better(Tree *t, int i, int j) {
    /* smaller wins; ties go to lower index (stable) */
    t->cmps++;
    if (t->val[i] != t->val[j])
        return t->val[i] < t->val[j] ? i : j;
    return i < j ? i : j;
}

static int winner_at(Tree *t, int node) {
    /* node in [1,2m): leaves are m..2m-1 */
    return node >= t->m ? node - t->m : t->win[node];
}

static void build(Tree *t, const int *a, int n) {
    int m = 1;
    while (m < n)
        m *= 2;
    t->m = m;
    t->cmps = 0;
    t->val = malloc(sizeof(int) * (size_t)m);
    t->win = malloc(sizeof(int) * (size_t)m);
    check(t->val && t->win, "alloc");
    for (int i = 0; i < m; i++)
        t->val[i] = i < n ? a[i] : INT_MAX;
    for (int node = m - 1; node >= 1; node--)
        t->win[node] = better(t, winner_at(t, 2 * node), winner_at(t, 2 * node + 1));
}

static void replay(Tree *t, int leaf) {
    for (int node = (leaf + t->m) / 2; node >= 1; node /= 2)
        t->win[node] = better(t, winner_at(t, 2 * node), winner_at(t, 2 * node + 1));
}

int main(void) {
    static const int sizes[] = {1, 2, 3, 10, 64, 100, 1000};
    for (int s = 0; s < 7; s++) {
        int n = sizes[s];
        int *a = malloc(sizeof(int) * (size_t)n), *out = malloc(sizeof(int) * (size_t)n);
        check(a && out, "alloc");
        for (int i = 0; i < n; i++)
            a[i] = (int)(rng() % 500);
        long total = 0;
        for (int i = 0; i < n; i++)
            total += a[i];
        Tree t;
        build(&t, a, n);
        long build_cmps = t.cmps;
        int prev_leaf_ties_ok = 1;
        for (int k = 0; k < n; k++) {
            int w = t.m == 1 ? 0 : t.win[1];
            out[k] = t.val[w];
            t.val[w] = INT_MAX;
            replay(&t, w);
        }
        long sum = 0;
        for (int i = 0; i < n; i++) {
            sum += out[i];
            if (i)
                check(out[i - 1] <= out[i], "sorted");
        }
        check(sum == total && prev_leaf_ties_ok, "sum preserved");
        printf("n=%-5d leaves=%-5d build_cmps=%-5ld total_cmps=%-7ld min=%d max=%d\n", n, t.m, build_cmps, t.cmps,
               out[0], out[n - 1]);
        free(t.val);
        free(t.win);
        free(a);
        free(out);
    }
    return 0;
}
