/*
 * title: Heavy-light decomposition path sums
 * topic: algorithms
 * covers: heavy-light decomposition, chain heads, Fenwick tree, path queries, point updates, naive parent-walk verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 400 };

static unsigned st = 1029384u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void check(int c, const char *m) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", m);
        exit(1);
    }
}

static int par[N], depth[N], sz[N], heavy[N], head[N], pos[N], val[N];
static int kids[N][N], nk[N];
static long bit[N + 1];
static int cur_pos;

static void bit_add(int i, long d) {
    for (i++; i <= N; i += i & -i) bit[i] += d;
}
static long bit_sum(int i) { /* prefix [0, i) */
    long s = 0;
    for (; i > 0; i -= i & -i) s += bit[i];
    return s;
}
static long range_sum(int l, int r) { return bit_sum(r + 1) - bit_sum(l); }

/* parents always have smaller ids, so reverse id order is a valid post-order */
static void build(void) {
    for (int v = N - 1; v >= 0; v--) {
        sz[v] += 1;
        if (v) sz[par[v]] += sz[v];
    }
    for (int v = 0; v < N; v++) heavy[v] = -1;
    for (int v = 1; v < N; v++) {
        int p = par[v];
        if (heavy[p] < 0 || sz[v] > sz[heavy[p]]) heavy[p] = v;
    }
    /* assign positions so every heavy chain is contiguous */
    int stack[N], sp = 0;
    stack[sp++] = 0;
    head[0] = 0;
    while (sp) {
        int h = stack[--sp];
        for (int v = h; v >= 0; v = heavy[v]) {
            head[v] = h;
            pos[v] = cur_pos++;
            for (int i = 0; i < nk[v]; i++)
                if (kids[v][i] != heavy[v]) stack[sp++] = kids[v][i];
        }
    }
}

static long path_sum(int a, int b) {
    long s = 0;
    while (head[a] != head[b]) {
        if (depth[head[a]] < depth[head[b]]) {
            int t = a;
            a = b;
            b = t;
        }
        s += range_sum(pos[head[a]], pos[a]);
        a = par[head[a]];
    }
    if (depth[a] > depth[b]) {
        int t = a;
        a = b;
        b = t;
    }
    return s + range_sum(pos[a], pos[b]);
}

static long naive_sum(int a, int b) {
    long s = 0;
    while (a != b) {
        if (depth[a] < depth[b]) {
            int t = a;
            a = b;
            b = t;
        }
        s += val[a];
        a = par[a];
    }
    return s + val[a];
}

int main(void) {
    par[0] = -1;
    for (int v = 1; v < N; v++) {
        par[v] = (int)(rnd() % (unsigned)v);
        if (rnd() % 3 == 0 && v > 3) par[v] = v - 1 - (int)(rnd() % 3u); /* some long paths */
        depth[v] = depth[par[v]] + 1;
        kids[par[v]][nk[par[v]]++] = v;
    }
    build();
    for (int v = 0; v < N; v++) val[v] = (int)(rnd() % 100) - 20, bit_add(pos[v], val[v]);
    int chains = 0, maxchain = 0, chainlen[N] = {0};
    for (int v = 0; v < N; v++) {
        if (head[v] == v) chains++;
        chainlen[head[v]]++;
    }
    for (int v = 0; v < N; v++)
        if (chainlen[v] > maxchain) maxchain = chainlen[v];
    /* light-edge count on any root path is at most log2 N */
    int worst_light = 0;
    for (int v = 0; v < N; v++) {
        int light = 0;
        for (int x = v; head[x] != 0; x = par[head[x]]) light++;
        if (light > worst_light) worst_light = light;
    }
    check(worst_light <= 9, "at most log2(N) light edges");
    long total = 0;
    int updates = 0;
    for (int q = 0; q < 6000; q++) {
        if (q % 4 == 3) {
            int v = (int)(rnd() % N), nv = (int)(rnd() % 200) - 100;
            bit_add(pos[v], nv - val[v]);
            val[v] = nv;
            updates++;
        } else {
            int a = (int)(rnd() % N), b = (int)(rnd() % N);
            long got = path_sum(a, b), want = naive_sum(a, b);
            check(got == want, "hld path sum equals naive walk");
            total += got;
        }
    }
    printf("nodes %d, chains %d, longest chain %d, max light edges to root %d\n", N, chains, maxchain, worst_light);
    printf("updates %d, sum of query answers %ld\n", updates, total);
    printf("path(399, 0)=%ld path(200, 300)=%ld\n", path_sum(399, 0), path_sum(200, 300));
    return 0;
}
