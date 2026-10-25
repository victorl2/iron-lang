/*
 * title: Compressed sparse row graph built by counting sort
 * topic: data_structures
 * covers: CSR, counting sort, prefix sums, transpose, degree arrays, neighbor slices, edge weights, BFS cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 0xC5A0BEEF12345ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int n, m; int *off; int *dst; int *w; } Csr;

/* two-pass build: count degrees, prefix sum, scatter. Stable in input order. */
static Csr csr_build(int n, int m, const int *src, const int *dst, const int *w) {
    Csr g; g.n = n; g.m = m;
    g.off = calloc((size_t)n + 1, sizeof(int));
    g.dst = malloc((size_t)(m ? m : 1) * sizeof(int));
    g.w = malloc((size_t)(m ? m : 1) * sizeof(int));
    for (int i = 0; i < m; i++) g.off[src[i] + 1]++;
    for (int u = 0; u < n; u++) g.off[u + 1] += g.off[u];
    int *pos = malloc((size_t)n * sizeof(int));
    memcpy(pos, g.off, (size_t)n * sizeof(int));
    for (int i = 0; i < m; i++) { int p = pos[src[i]]++; g.dst[p] = dst[i]; g.w[p] = w[i]; }
    free(pos);
    return g;
}
static void csr_free(Csr *g) { free(g->off); free(g->dst); free(g->w); }

static Csr csr_transpose(const Csr *g) {
    int *s = malloc((size_t)(g->m ? g->m : 1) * sizeof(int)), *d = malloc((size_t)(g->m ? g->m : 1) * sizeof(int));
    int *w = malloc((size_t)(g->m ? g->m : 1) * sizeof(int));
    int k = 0;
    for (int u = 0; u < g->n; u++)
        for (int p = g->off[u]; p < g->off[u + 1]; p++) { s[k] = g->dst[p]; d[k] = u; w[k] = g->w[p]; k++; }
    Csr t = csr_build(g->n, g->m, s, d, w);
    free(s); free(d); free(w);
    return t;
}

static int csr_has(const Csr *g, int u, int v) {
    for (int p = g->off[u]; p < g->off[u + 1]; p++) if (g->dst[p] == v) return 1;
    return 0;
}

static int csr_bfs(const Csr *g, int s, int *dist) {
    int *q = malloc((size_t)g->n * sizeof(int)); int h = 0, t = 0, reach = 0;
    for (int i = 0; i < g->n; i++) dist[i] = -1;
    dist[s] = 0; q[t++] = s;
    while (h < t) {
        int u = q[h++]; reach++;
        for (int p = g->off[u]; p < g->off[u + 1]; p++) {
            int v = g->dst[p];
            if (dist[v] < 0) { dist[v] = dist[u] + 1; q[t++] = v; }
        }
    }
    free(q);
    return reach;
}

/* reference BFS on a dense matrix */
static int mat_bfs(int n, const unsigned char *a, int s, int *dist) {
    for (int i = 0; i < n; i++) dist[i] = -1;
    dist[s] = 0; int reach = 0;
    for (int d = 0; ; d++) {
        int any = 0;
        for (int u = 0; u < n; u++) if (dist[u] == d) {
            any = 1; reach++;
            for (int v = 0; v < n; v++) if (a[u * n + v] && dist[v] < 0) dist[v] = d + 1;
        }
        if (!any) break;
    }
    return reach;
}

int main(void) {
    int cases[3][2] = {{12, 20}, {60, 150}, {200, 600}};
    for (int c = 0; c < 3; c++) {
        int n = cases[c][0], m = cases[c][1];
        int *s = malloc((size_t)m * sizeof(int)), *d = malloc((size_t)m * sizeof(int)), *w = malloc((size_t)m * sizeof(int));
        unsigned char *mat = calloc((size_t)n * n, 1);
        long wsum = 0;
        for (int i = 0; i < m; i++) {
            s[i] = (int)(rnd() % (unsigned)n); d[i] = (int)(rnd() % (unsigned)n); w[i] = (int)(rnd() % 90) + 1;
            mat[s[i] * n + d[i]] = 1; wsum += w[i];
        }
        Csr g = csr_build(n, m, s, d, w);
        check(g.off[0] == 0 && g.off[n] == m, "offsets bounds");
        for (int u = 0; u < n; u++) check(g.off[u] <= g.off[u + 1], "offsets monotone");
        /* stability: the slice of u lists edges in input order */
        for (int u = 0; u < n; u++) {
            int p = g.off[u];
            for (int i = 0; i < m; i++) if (s[i] == u) { check(g.dst[p] == d[i] && g.w[p] == w[i], "stable scatter"); p++; }
            check(p == g.off[u + 1], "slice length");
        }
        Csr t = csr_transpose(&g), tt = csr_transpose(&t);
        long tw = 0; for (int i = 0; i < t.m; i++) tw += t.w[i];
        check(tw == wsum, "weight preserved");
        for (int u = 0; u < n; u++) for (int v = 0; v < n; v++) {
            check(csr_has(&g, u, v) == mat[u * n + v], "has edge");
            check(csr_has(&t, v, u) == mat[u * n + v], "transpose has edge");
        }
        check(tt.m == g.m && memcmp(tt.off, g.off, (size_t)(n + 1) * sizeof(int)) == 0, "double transpose offsets");
        int *dist = malloc((size_t)n * sizeof(int)), *dref = malloc((size_t)n * sizeof(int));
        int r1 = csr_bfs(&g, 0, dist), r2 = mat_bfs(n, mat, 0, dref);
        check(r1 == r2 && memcmp(dist, dref, (size_t)n * sizeof(int)) == 0, "bfs vs matrix");
        int far = 0; for (int i = 0; i < n; i++) if (dist[i] > far) far = dist[i];
        int maxdeg = 0, isolated = 0;
        for (int u = 0; u < n; u++) {
            int dg = g.off[u + 1] - g.off[u];
            if (dg > maxdeg) maxdeg = dg;
            if (dg == 0 && t.off[u + 1] == t.off[u]) isolated++;
        }
        printf("n=%d m=%d weight=%ld maxdeg=%d isolated=%d reach0=%d ecc0=%d\n", n, m, wsum, maxdeg, isolated, r1, far);
        csr_free(&g); csr_free(&t); csr_free(&tt);
        free(s); free(d); free(w); free(mat); free(dist); free(dref);
    }
    return 0;
}
