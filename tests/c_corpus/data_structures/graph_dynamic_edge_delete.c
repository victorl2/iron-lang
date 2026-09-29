/*
 * title: Dynamic graph with edge ids, swap-remove and vertex deletion
 * topic: data_structures
 * covers: dynamic graph, edge ids, back-pointers, O(1) edge deletion, free list, incidence lists, vertex removal, matrix oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 24
#define MAXE 400

static unsigned long long rs = 0x1234ABCD5678EFULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* Edge slot e joins a[e]-b[e]. Each vertex keeps an incidence array of edge ids;
 * pos_a[e]/pos_b[e] are the slot's indices in those arrays for O(1) removal. */
typedef struct {
    int a, b, pa, pb, live, next_free;
} Edge;
typedef struct {
    Edge e[MAXE];
    int free_head, used;
    int deg[N], inc[N][MAXE];
    int alive[N];
    int nedges;
} Graph;

static void g_init(Graph *g) {
    memset(g, 0, sizeof *g); g->free_head = -1;
    for (int i = 0; i < N; i++) g->alive[i] = 1;
}
static int g_add(Graph *g, int u, int v) {
    int id;
    if (g->free_head >= 0) { id = g->free_head; g->free_head = g->e[id].next_free; }
    else { check(g->used < MAXE, "capacity"); id = g->used++; }
    Edge *e = &g->e[id];
    e->a = u; e->b = v; e->live = 1;
    e->pa = g->deg[u]; g->inc[u][g->deg[u]++] = id;
    if (u != v) { e->pb = g->deg[v]; g->inc[v][g->deg[v]++] = id; } else e->pb = -1;
    g->nedges++;
    return id;
}
static void detach(Graph *g, int v, int pos) {
    int last = g->inc[v][--g->deg[v]];
    if (pos != g->deg[v]) {
        g->inc[v][pos] = last;
        Edge *le = &g->e[last];
        if (le->a == v && le->pa == g->deg[v]) le->pa = pos; else le->pb = pos;
    }
}
static void g_del(Graph *g, int id) {
    Edge *e = &g->e[id];
    check(e->live, "delete live");
    detach(g, e->a, e->pa);
    if (e->a != e->b) detach(g, e->b, e->pb);
    e->live = 0; e->next_free = g->free_head; g->free_head = id; g->nedges--;
}
static void g_del_vertex(Graph *g, int v) {
    while (g->deg[v] > 0) g_del(g, g->inc[v][g->deg[v] - 1]);
    g->alive[v] = 0;
}

/* oracle: multiplicity matrix */
static int mult[N][N];

static int comps(const Graph *g) {
    int seen[N] = {0}, st[N], c = 0;
    for (int s = 0; s < N; s++) {
        if (!g->alive[s] || seen[s]) continue;
        c++; int sp = 0; st[sp++] = s; seen[s] = 1;
        while (sp) {
            int u = st[--sp];
            for (int i = 0; i < g->deg[u]; i++) {
                const Edge *e = &g->e[g->inc[u][i]];
                int v = e->a == u ? e->b : e->a;
                if (!seen[v]) { seen[v] = 1; st[sp++] = v; }
            }
        }
    }
    return c;
}
static void verify(const Graph *g) {
    int total = 0;
    for (int u = 0; u < N; u++) {
        int expect = 0;
        for (int v = 0; v < N; v++) expect += (u == v) ? 2 * mult[u][u] : mult[u][v];
        int got = 0;
        for (int i = 0; i < g->deg[u]; i++) { const Edge *e = &g->e[g->inc[u][i]]; got += (e->a == e->b) ? 2 : 1; check(e->live, "inc live"); }
        check(got == expect, "degree vs matrix");
        for (int i = 0; i < g->deg[u]; i++) {
            int id = g->inc[u][i]; const Edge *e = &g->e[id];
            if (e->a == u) check(e->pa == i, "pa backpointer"); else check(e->pb == i, "pb backpointer");
        }
    }
    for (int u = 0; u < N; u++) for (int v = u; v < N; v++) total += mult[u][v];
    check(total == g->nedges, "edge count");
}
/* components by matrix flood fill */
static int comps_oracle(const Graph *g) {
    int seen[N] = {0}, st[N], c = 0;
    for (int s = 0; s < N; s++) {
        if (!g->alive[s] || seen[s]) continue;
        c++; int sp = 0; st[sp++] = s; seen[s] = 1;
        while (sp) {
            int u = st[--sp];
            for (int v = 0; v < N; v++) if (mult[u][v] && !seen[v]) { seen[v] = 1; st[sp++] = v; }
        }
    }
    return c;
}

int main(void) {
    Graph *g = malloc(sizeof *g); g_init(g);
    int ids[MAXE]; int nid = 0;
    int adds = 0, dels = 0, vdel = 0, reuse = 0, revived = 0;
    for (int step = 0; step < 3000; step++) {
        unsigned r = rnd() % 100;
        if (r < 55 && g->nedges < MAXE - 2) {
            int u = (int)(rnd() % N), v = (int)(rnd() % N);
            if (!g->alive[u] || !g->alive[v]) continue;
            int wasfree = g->free_head >= 0;
            int id = g_add(g, u, v); reuse += wasfree;
            mult[u][v]++; if (u != v) mult[v][u]++;
            ids[nid++] = id; adds++;
        } else if (r < 97 && nid > 0) {
            int k = (int)(rnd() % (unsigned)nid);
            int id = ids[k]; ids[k] = ids[--nid];
            if (!g->e[id].live) continue;
            int u = g->e[id].a, v = g->e[id].b;
            g_del(g, id); mult[u][v]--; if (u != v) mult[v][u]--; dels++;
        } else {
            int v = (int)(rnd() % N);
            if (!g->alive[v]) { g->alive[v] = 1; revived++; continue; }
            if (rnd() % 4 != 0) continue;
            g_del_vertex(g, v); vdel++;
            for (int x = 0; x < N; x++) { mult[v][x] = 0; mult[x][v] = 0; }
            int k = 0; for (int i = 0; i < nid; i++) if (g->e[ids[i]].live) ids[k++] = ids[i];
            nid = k;
        }
        if (step % 50 == 0) { verify(g); check(comps(g) == comps_oracle(g), "components"); }
        if (step % 500 == 0)
            printf("step %4d: edges=%3d comps=%2d freehead_set=%d\n", step, g->nedges, comps(g), g->free_head >= 0);
    }
    verify(g);
    int alive = 0; for (int i = 0; i < N; i++) alive += g->alive[i];
    printf("adds=%d dels=%d vertex_deletes=%d revived=%d slot_reuse=%d alive=%d edges=%d comps=%d\n",
           adds, dels, vdel, revived, reuse, alive, g->nedges, comps(g));
    free(g);
    return 0;
}
