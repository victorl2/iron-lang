/*
 * title: 2-SAT via implication graph SCCs
 * topic: algorithms
 * covers: 2-SAT, implication graph, Tarjan components, assignment extraction, brute-force verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { V = 12, MAXC = 60, NN = 2 * V };

static unsigned st = 1010101u;
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

/* literal x: 2*v for true, 2*v+1 for false */
static int adj[NN][MAXC * 2], deg[NN];
static int idx[NN], low[NN], onst[NN], comp[NN], stk[NN], sp, cnt, ncomp;
static int ca[MAXC], cb[MAXC];

static void tarjan(int u) {
    idx[u] = low[u] = ++cnt;
    stk[sp++] = u;
    onst[u] = 1;
    for (int i = 0; i < deg[u]; i++) {
        int v = adj[u][i];
        if (!idx[v]) {
            tarjan(v);
            if (low[v] < low[u]) low[u] = low[v];
        } else if (onst[v] && idx[v] < low[u])
            low[u] = idx[v];
    }
    if (low[u] == idx[u]) {
        int v;
        do {
            v = stk[--sp];
            onst[v] = 0;
            comp[v] = ncomp;
        } while (v != u);
        ncomp++;
    }
}

static int solve(int nclauses, int *assign) {
    memset(deg, 0, sizeof deg);
    memset(idx, 0, sizeof idx);
    sp = cnt = ncomp = 0;
    for (int i = 0; i < nclauses; i++) {
        int a = ca[i], b = cb[i];
        adj[a ^ 1][deg[a ^ 1]++] = b;
        adj[b ^ 1][deg[b ^ 1]++] = a;
    }
    for (int u = 0; u < NN; u++)
        if (!idx[u]) tarjan(u);
    for (int v = 0; v < V; v++) {
        if (comp[2 * v] == comp[2 * v + 1]) return 0;
        /* Tarjan ids are reverse topological: smaller id means later; choose literal with smaller id */
        assign[v] = comp[2 * v] < comp[2 * v + 1];
    }
    return 1;
}

static int lit_true(int lit, const int *assign) {
    return (lit & 1) ? !assign[lit >> 1] : assign[lit >> 1];
}

int main(void) {
    int sat_count = 0, unsat_count = 0;
    for (int round = 0; round < 12; round++) {
        int nclauses = 6 + round * 2;
        for (int i = 0; i < nclauses; i++) {
            ca[i] = (int)(rnd() % NN);
            cb[i] = (int)(rnd() % NN);
        }
        int assign[V];
        int ok = solve(nclauses, assign);
        /* brute force */
        int brute = 0;
        for (unsigned m = 0; m < (1u << V) && !brute; m++) {
            int a2[V];
            for (int v = 0; v < V; v++) a2[v] = (int)(m >> v & 1u);
            int good = 1;
            for (int i = 0; i < nclauses && good; i++) good = lit_true(ca[i], a2) || lit_true(cb[i], a2);
            brute = good;
        }
        check(ok == brute, "2-SAT verdict matches brute force");
        if (ok)
            for (int i = 0; i < nclauses; i++)
                check(lit_true(ca[i], assign) || lit_true(cb[i], assign), "assignment satisfies");
        printf("round %2d: %2d clauses -> %s", round, nclauses, ok ? "SAT  " : "UNSAT");
        if (ok) {
            printf(" ");
            for (int v = 0; v < V; v++) putchar('0' + assign[v]);
            sat_count++;
        } else
            unsat_count++;
        printf("\n");
    }
    printf("sat %d, unsat %d\n", sat_count, unsat_count);
    return 0;
}
