/*
 * title: Graph coloring with DSATUR and Welsh-Powell
 * topic: algorithms
 * covers: greedy graph coloring, DSATUR, Welsh-Powell, saturation degree, chromatic number by backtracking, bitmasks
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 18 };

static unsigned st = 3033u;
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

static unsigned adjm[N];
static int degree(int v) {
    int c = 0;
    for (unsigned x = adjm[v]; x; x &= x - 1) c++;
    return c;
}
static void verify(const int *col, int k) {
    for (int u = 0; u < N; u++) {
        check(col[u] >= 0 && col[u] < k, "color in range");
        for (int v = 0; v < N; v++)
            if (adjm[u] >> v & 1u) check(col[u] != col[v], "proper coloring");
    }
}

static int dsatur(int *col) {
    unsigned seen[N] = {0}; /* colors present among neighbours */
    int used = 0;
    for (int v = 0; v < N; v++) col[v] = -1;
    for (int step = 0; step < N; step++) {
        int best = -1, bsat = -1, bdeg = -1;
        for (int v = 0; v < N; v++) {
            if (col[v] >= 0) continue;
            int sat = 0;
            for (unsigned x = seen[v]; x; x &= x - 1) sat++;
            int d = degree(v);
            if (sat > bsat || (sat == bsat && d > bdeg)) best = v, bsat = sat, bdeg = d;
        }
        int c = 0;
        while (seen[best] >> c & 1u) c++;
        col[best] = c;
        if (c + 1 > used) used = c + 1;
        for (int u = 0; u < N; u++)
            if (adjm[best] >> u & 1u) seen[u] |= 1u << c;
    }
    return used;
}

static int welsh_powell(int *col) {
    int order[N];
    for (int i = 0; i < N; i++) order[i] = i;
    for (int i = 1; i < N; i++) { /* insertion sort by degree desc then id: total order */
        int v = order[i], j = i - 1;
        while (j >= 0 && (degree(order[j]) < degree(v))) order[j + 1] = order[j], j--;
        order[j + 1] = v;
    }
    for (int v = 0; v < N; v++) col[v] = -1;
    int used = 0, left = N;
    while (left) {
        unsigned taken = 0;
        for (int i = 0; i < N; i++) {
            int v = order[i];
            if (col[v] >= 0 || (adjm[v] & taken)) continue;
            col[v] = used;
            taken |= 1u << v;
            left--;
        }
        used++;
    }
    return used;
}

static int cur[N];
static int try_k(int v, int k, int maxc) {
    if (v == N) return 1;
    for (int c = 0; c < k && c <= maxc + 1; c++) {
        int ok = 1;
        for (int u = 0; u < v && ok; u++)
            if ((adjm[v] >> u & 1u) && cur[u] == c) ok = 0;
        if (!ok) continue;
        cur[v] = c;
        if (try_k(v + 1, k, c > maxc ? c : maxc)) return 1;
    }
    return 0;
}

int main(void) {
    for (int trial = 0; trial < 7; trial++) {
        unsigned density = 10 + 8u * (unsigned)trial;
        memset(adjm, 0, sizeof adjm);
        int edges = 0;
        for (int u = 0; u < N; u++)
            for (int v = u + 1; v < N; v++)
                if (rnd() % 100 < density) adjm[u] |= 1u << v, adjm[v] |= 1u << u, edges++;
        int c1[N], c2[N];
        int k1 = dsatur(c1), k2 = welsh_powell(c2);
        verify(c1, k1);
        verify(c2, k2);
        int chi = 1;
        while (!try_k(0, chi, -1)) chi++;
        verify(cur, chi);
        check(k1 >= chi && k2 >= chi, "heuristics no better than chromatic number");
        int maxdeg = 0;
        for (int v = 0; v < N; v++)
            if (degree(v) > maxdeg) maxdeg = degree(v);
        check(k1 <= maxdeg + 1 && k2 <= maxdeg + 1, "Brooks-style greedy bound");
        printf("density %2u%%: edges %3d, max degree %2d, dsatur %d, welsh-powell %d, chromatic %d\n", density, edges, maxdeg, k1, k2, chi);
    }
    /* odd cycle needs 3, even cycle needs 2, complete graph needs N */
    memset(adjm, 0, sizeof adjm);
    for (int i = 0; i < 9; i++) adjm[i] |= 1u << ((i + 1) % 9), adjm[(i + 1) % 9] |= 1u << i;
    int chi = 1;
    while (!try_k(0, chi, -1)) chi++;
    printf("cycle C9 plus 9 isolated vertices: chromatic %d\n", chi);
    check(chi == 3, "odd cycle chromatic number");
    return 0;
}
