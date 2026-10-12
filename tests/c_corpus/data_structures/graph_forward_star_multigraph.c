/*
 * title: Forward-star multigraph with paired reverse edges
 * topic: data_structures
 * covers: forward star, head/next arrays, paired edges xor 1, parallel edges, self loops, edge tombstones, Euler circuit
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 16
#define M 200

static unsigned long long rs = 0xFEED5EED77ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* Edge i and i^1 are the two directions of one undirected edge. */
static int head[N], nxt[2 * M], to[2 * M], dead[2 * M], cnt;

static void reset(void) { memset(head, -1, sizeof head); cnt = 0; memset(dead, 0, sizeof dead); }
static void add_edge(int u, int v) {
    to[cnt] = v; nxt[cnt] = head[u]; head[u] = cnt++;
    to[cnt] = u; nxt[cnt] = head[v]; head[v] = cnt++;
}
static void kill_edge(int e) { dead[e] = dead[e ^ 1] = 1; }

static int degree(int u) {
    int d = 0;
    for (int e = head[u]; e >= 0; e = nxt[e]) if (!dead[e]) d += (to[e] == u && (e & 1) == 0) ? 2 : (to[e] == u ? 0 : 1);
    return d;
}
static int components(void) {
    int seen[N] = {0}, st[2 * M], c = 0;
    for (int s = 0; s < N; s++) {
        if (seen[s]) continue;
        c++; int sp = 0; st[sp++] = s; seen[s] = 1;
        while (sp) {
            int u = st[--sp];
            for (int e = head[u]; e >= 0; e = nxt[e]) if (!dead[e] && !seen[to[e]]) { seen[to[e]] = 1; st[sp++] = to[e]; }
        }
    }
    return c;
}

/* Hierholzer over live edges; returns number of edges in the circuit. */
static int euler(int start, int *path) {
    static int used_e[2 * M];
    int stack[2 * M + 2], sp = 0, cur[N], on = 0;
    memcpy(cur, head, sizeof cur);
    memset(used_e, 0, sizeof used_e);
    stack[sp++] = start;
    while (sp) {
        int u = stack[sp - 1];
        while (cur[u] >= 0 && (dead[cur[u]] || used_e[cur[u]])) cur[u] = nxt[cur[u]];
        if (cur[u] < 0) { path[on++] = u; sp--; }
        else { int e = cur[u]; used_e[e] = used_e[e ^ 1] = 1; stack[sp++] = to[e]; }
    }
    return on - 1;
}

int main(void) {
    for (int round = 0; round < 6; round++) {
        reset();
        int m = 20 + (int)(rnd() % 60);
        int mat[N][N]; memset(mat, 0, sizeof mat);
        for (int i = 0; i < m; i++) {
            int u = (int)(rnd() % N), v = (int)(rnd() % N);
            add_edge(u, v); mat[u][v]++; if (u != v) mat[v][u]++;
        }
        int kills = 0;
        for (int i = 0; i < m / 4; i++) {
            int e = 2 * (int)(rnd() % (unsigned)m);
            if (dead[e]) continue;
            kill_edge(e); kills++;
            int u = to[e ^ 1], v = to[e];
            mat[u][v]--; if (u != v) mat[v][u]--;
        }
        int odd = 0, loops = 0, par = 0, live = 0;
        for (int u = 0; u < N; u++) {
            int d = degree(u), md = 0;
            for (int v = 0; v < N; v++) md += (u == v) ? 2 * mat[u][u] : mat[u][v];
            check(d == md, "degree matches multiplicity matrix");
            if (d & 1) odd++;
            loops += mat[u][u];
            for (int v = u + 1; v < N; v++) par += mat[u][v] > 1 ? mat[u][v] - 1 : 0;
            live += d;
        }
        check(live % 2 == 0 && odd % 2 == 0, "handshake lemma");
        int comps_before = components();
        /* pair up odd vertices with new edges so every degree is even, then walk a circuit */
        int oddv[N], no = 0;
        for (int u = 0; u < N; u++) if (degree(u) & 1) oddv[no++] = u;
        for (int i = 0; i + 1 < no; i += 2) add_edge(oddv[i], oddv[i + 1]);
        int start = -1;
        for (int u = 0; u < N; u++) if (degree(u) > 0) { start = u; break; }
        int path[2 * M + 2], elen = 0, live_e = 0;
        for (int e = 0; e < cnt; e += 2) if (!dead[e]) live_e++;
        if (start >= 0) {
            int comp_e2 = 0, seen[N] = {0}, st[N], sp = 0;
            st[sp++] = start; seen[start] = 1;
            while (sp) {
                int u = st[--sp];
                for (int e = head[u]; e >= 0; e = nxt[e]) if (!dead[e]) { comp_e2++; if (!seen[to[e]]) { seen[to[e]] = 1; st[sp++] = to[e]; } }
            }
            elen = euler(start, path);
            check(elen == comp_e2 / 2, "euler circuit covers component");
            check(path[0] == path[elen], "circuit closes");
        }
        printf("round %d: edges=%d killed=%d odd=%d loops=%d extra_parallel=%d comps=%d fixed_edges=%d circuit=%d\n",
               round, m, kills, odd, loops, par, comps_before, live_e, elen);
    }
    return 0;
}
