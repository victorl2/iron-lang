/*
 * title: Graph representations and conversions between them
 * topic: data_structures
 * covers: adjacency matrix, adjacency list, edge list, round-trip conversion, transpose, symmetrize, degree tables, checksum
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 48

static unsigned long long rs = 0x9E3779B97F4A7C15ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int n; unsigned char a[MAXN][MAXN]; } Matrix;
typedef struct { int n; int *len, *cap; int **adj; } AdjList;
typedef struct { int u, v; } Edge;
typedef struct { int n, m, cap; Edge *e; } EdgeList;

static void al_init(AdjList *g, int n) {
    if (n < 0 || n > MAXN) exit(2);
    g->n = n; g->len = calloc((size_t)n, sizeof(int)); g->cap = calloc((size_t)n, sizeof(int));
    g->adj = calloc((size_t)n, sizeof(int *));
}
static void al_free(AdjList *g) {
    for (int i = 0; i < g->n; i++) free(g->adj[i]);
    free(g->adj); free(g->len); free(g->cap);
}
static void al_add(AdjList *g, int u, int v) {
    if (g->len[u] == g->cap[u]) {
        g->cap[u] = g->cap[u] ? g->cap[u] * 2 : 2;
        g->adj[u] = realloc(g->adj[u], (size_t)g->cap[u] * sizeof(int));
    }
    g->adj[u][g->len[u]++] = v;
}
static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static int cmp_edge(const void *a, const void *b) {
    const Edge *x = a, *y = b;
    if (x->u != y->u) return x->u < y->u ? -1 : 1;
    return (x->v > y->v) - (x->v < y->v);
}
static void al_sort(AdjList *g) { for (int i = 0; i < g->n; i++) if (g->len[i] > 0) qsort(g->adj[i], (size_t)g->len[i], sizeof(int), cmp_int); }
static void el_add(EdgeList *l, int u, int v) {
    if (l->m == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->e = realloc(l->e, (size_t)l->cap * sizeof(Edge)); }
    l->e[l->m].u = u; l->e[l->m].v = v; l->m++;
}

static void m_to_al(const Matrix *m, AdjList *g) {
    al_init(g, m->n);
    for (int u = 0; u < m->n; u++) for (int v = 0; v < m->n; v++) if (m->a[u][v]) al_add(g, u, v);
}
static void al_to_el(const AdjList *g, EdgeList *l) {
    l->n = g->n; l->m = l->cap = 0; l->e = NULL;
    for (int u = 0; u < g->n; u++) for (int i = 0; i < g->len[u]; i++) el_add(l, u, g->adj[u][i]);
}
static void el_to_m(const EdgeList *l, Matrix *m) {
    memset(m, 0, sizeof *m); m->n = l->n;
    for (int i = 0; i < l->m; i++) m->a[l->e[i].u][l->e[i].v] = 1;
}
static void el_to_al(const EdgeList *l, AdjList *g) {
    al_init(g, l->n);
    for (int i = 0; i < l->m; i++) al_add(g, l->e[i].u, l->e[i].v);
    al_sort(g);
}
static void al_to_m(const AdjList *g, Matrix *m) {
    memset(m, 0, sizeof *m); m->n = g->n;
    for (int u = 0; u < g->n; u++) for (int i = 0; i < g->len[u]; i++) m->a[u][g->adj[u][i]] = 1;
}
static int m_equal(const Matrix *a, const Matrix *b) { return a->n == b->n && memcmp(a->a, b->a, sizeof a->a) == 0; }
static unsigned m_hash(const Matrix *m) {
    unsigned h = 2166136261u;
    for (int u = 0; u < m->n; u++) for (int v = 0; v < m->n; v++) { h ^= m->a[u][v] + 1u; h *= 16777619u; }
    return h;
}
static void m_transpose(const Matrix *m, Matrix *t) {
    memset(t, 0, sizeof *t); t->n = m->n;
    for (int u = 0; u < m->n; u++) for (int v = 0; v < m->n; v++) t->a[v][u] = m->a[u][v];
}
static int m_edges(const Matrix *m) { int c = 0; for (int u = 0; u < m->n; u++) for (int v = 0; v < m->n; v++) c += m->a[u][v]; return c; }

int main(void) {
    int ns[4] = {5, 17, 33, 48};
    int pcts[4] = {10, 30, 5, 50};
    for (int t = 0; t < 4; t++) {
        Matrix m; memset(&m, 0, sizeof m); m.n = ns[t];
        for (int u = 0; u < m.n; u++) for (int v = 0; v < m.n; v++) {
            unsigned r = rnd();
            m.a[u][v] = (int)(r % 100) < pcts[t];
        }
        AdjList g; EdgeList el; m_to_al(&m, &g); al_to_el(&g, &el);
        Matrix m2, m3; el_to_m(&el, &m2); check(m_equal(&m, &m2), "matrix->list->edges->matrix");
        /* shuffle edge list, rebuild through the other paths */
        for (int i = el.m - 1; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); Edge tmp = el.e[i]; el.e[i] = el.e[j]; el.e[j] = tmp; }
        AdjList g2; el_to_al(&el, &g2); al_to_m(&g2, &m3);
        check(m_equal(&m, &m3), "shuffled edges->list->matrix");
        EdgeList el2; al_to_el(&g2, &el2);
        qsort(el.e, (size_t)el.m, sizeof(Edge), cmp_edge);
        check(el.m == el2.m && memcmp(el.e, el2.e, (size_t)el.m * sizeof(Edge)) == 0, "sorted edge lists agree");
        /* degree tables from each representation */
        int outd = 0, ind = 0, maxout = 0, maxin = 0;
        int indeg[MAXN] = {0};
        for (int u = 0; u < m.n; u++) {
            int d = 0;
            for (int v = 0; v < m.n; v++) { d += m.a[u][v]; indeg[v] += m.a[u][v]; }
            check(d == g.len[u], "out degree matrix vs list");
            outd += d; if (d > maxout) maxout = d;
        }
        for (int v = 0; v < m.n; v++) { ind += indeg[v]; if (indeg[v] > maxin) maxin = indeg[v]; }
        check(outd == ind && outd == el.m && outd == m_edges(&m), "handshake");
        /* transpose via edge swap vs matrix transpose */
        Matrix tm, te; m_transpose(&m, &tm);
        for (int i = 0; i < el.m; i++) { int x = el.e[i].u; el.e[i].u = el.e[i].v; el.e[i].v = x; }
        el_to_m(&el, &te); check(m_equal(&tm, &te), "transpose agree");
        /* symmetrize */
        Matrix sy = m; int loops = 0;
        for (int u = 0; u < m.n; u++) for (int v = 0; v < m.n; v++) if (m.a[u][v] || m.a[v][u]) sy.a[u][v] = sy.a[v][u] = 1;
        for (int u = 0; u < m.n; u++) loops += sy.a[u][u];
        int und = 0;
        for (int u = 0; u < m.n; u++) for (int v = u; v < m.n; v++) { check(sy.a[u][v] == sy.a[v][u], "symmetric"); und += sy.a[u][v]; }
        printf("n=%d edges=%d maxout=%d maxin=%d undirected=%d loops=%d hash=%08x thash=%08x\n",
               m.n, outd, maxout, maxin, und, loops, m_hash(&m), m_hash(&tm));
        al_free(&g); al_free(&g2); free(el.e); free(el2.e);
    }
    return 0;
}
