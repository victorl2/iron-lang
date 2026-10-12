/*
 * title: Hierholzer Euler circuits and paths
 * topic: algorithms
 * covers: Eulerian circuit, Eulerian path, Hierholzer, degree conditions, directed and undirected graphs, edge usage validation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 12, MAXE = 80 };

static unsigned st = 8128u;
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

static int eu[MAXE], ev[MAXE], ne;
static int inc[N][MAXE * 2], ninc[N], ptr[N], used[MAXE];
static int path[MAXE + 2], plen;
static int directed;

static void add(int u, int v) {
    eu[ne] = u, ev[ne] = v;
    inc[u][ninc[u]++] = ne;
    if (!directed) inc[v][ninc[v]++] = ne;
    ne++;
}
static void reset(int dir) {
    directed = dir;
    ne = plen = 0;
    memset(ninc, 0, sizeof ninc);
    memset(ptr, 0, sizeof ptr);
    memset(used, 0, sizeof used);
}

static void hier(int u) {
    while (ptr[u] < ninc[u]) {
        int e = inc[u][ptr[u]++];
        if (used[e]) continue;
        used[e] = 1;
        int v = directed ? ev[e] : (eu[e] == u ? ev[e] : eu[e]);
        hier(v);
    }
    path[plen++] = u;
}

/* returns start vertex if Eulerian path/circuit exists, else -1; *circuit says which */
static int find_start(int *circuit) {
    int indeg[N] = {0}, outdeg[N] = {0};
    for (int e = 0; e < ne; e++) outdeg[eu[e]]++, indeg[ev[e]]++;
    int start = -1, odd = 0, plus = 0, minus = 0;
    for (int v = 0; v < N; v++) {
        if (directed) {
            if (outdeg[v] - indeg[v] == 1) plus++, start = v;
            else if (indeg[v] - outdeg[v] == 1) minus++;
            else if (indeg[v] != outdeg[v]) return -1;
        } else if ((outdeg[v] + indeg[v]) & 1) odd++, start = start < 0 ? v : start;
    }
    if (directed) {
        if (plus == 0 && minus == 0) *circuit = 1;
        else if (plus == 1 && minus == 1) *circuit = 0;
        else return -1;
    } else {
        if (odd == 0) *circuit = 1;
        else if (odd == 2) *circuit = 0;
        else return -1;
    }
    if (start < 0)
        for (int v = 0; v < N; v++)
            if (ninc[v]) {
                start = v;
                break;
            }
    return start;
}

static void run(const char *name, int dir) {
    int circuit = 0;
    int s = find_start(&circuit);
    if (s < 0) {
        printf("%s: no Eulerian path\n", name);
        return;
    }
    hier(s);
    if (plen != ne + 1) {
        printf("%s: degrees fine but graph disconnected (used %d of %d edges)\n", name, plen - 1, ne);
        return;
    }
    /* path[] is reversed for directed graphs; reverse to travel order */
    for (int i = 0; i < plen / 2; i++) {
        int t = path[i];
        path[i] = path[plen - 1 - i];
        path[plen - 1 - i] = t;
    }
    int multi[N][N];
    memset(multi, 0, sizeof multi);
    for (int e = 0; e < ne; e++) {
        multi[eu[e]][ev[e]]++;
        if (!dir) multi[ev[e]][eu[e]]++;
    }
    for (int i = 0; i + 1 < plen; i++) {
        int a = path[i], b = path[i + 1];
        check(multi[a][b] > 0, "step uses an unused edge");
        multi[a][b]--;
        if (!dir) multi[b][a]--;
    }
    if (circuit) check(path[0] == path[plen - 1], "circuit closes");
    printf("%s: %s with %d edges from %d:", name, circuit ? "circuit" : "path", ne, path[0]);
    for (int i = 0; i < plen; i++) printf(" %d", path[i]);
    printf("\n");
}

int main(void) {
    /* undirected circuit: union of cycles */
    reset(0);
    for (int c = 0; c < 4; c++) {
        int a = 0, b = 1 + (int)(rnd() % (N - 1)), d = 1 + (int)(rnd() % (N - 1));
        if (a != b) add(a, b);
        if (b != d) add(b, d);
        if (d != a) add(d, a);
    }
    run("undirected union of triangles", 0);
    /* undirected path: add a walk between two vertices to a cycle */
    reset(0);
    for (int i = 0; i < 7; i++) add(i, (i + 1) % 7);
    add(0, 8);
    add(8, 9);
    add(9, 3);
    add(3, 10);
    run("undirected with two odd vertices", 0);
    /* directed circuit from a de Bruijn-like graph on 8 nodes */
    reset(1);
    for (int v = 0; v < 8; v++) add(v, (2 * v) % 8), add(v, (2 * v + 1) % 8);
    run("directed de Bruijn B(2,3)", 1);
    /* directed path */
    reset(1);
    for (int c = 0; c < 5; c++) { /* closed walks through vertex 0 */
        int prev = 0, len = 2 + (int)(rnd() % 4);
        for (int i = 0; i < len; i++) {
            int nx = 1 + (int)(rnd() % 6);
            add(prev, nx);
            prev = nx;
        }
        add(prev, 0);
    }
    run("directed closed walks", 1);
    reset(1);
    add(0, 1), add(1, 2), add(2, 0), add(2, 3), add(3, 4), add(4, 2), add(2, 5);
    run("directed with path", 1);
    reset(0);
    for (int i = 0; i < 4; i++) add(0, i + 1);
    run("undirected star (K1,4)", 0);
    return 0;
}
