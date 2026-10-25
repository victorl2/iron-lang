/*
 * title: Kahn topological sort with cycle detection
 * topic: algorithms
 * covers: topological sort, in-degree queue, priority variant, cycle detection, order validation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 24 };

static unsigned st = 99991u;
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

typedef struct {
    int n;
    unsigned char e[N][N];
} G;

/* Returns number of vertices emitted. lex=1 picks smallest available vertex. */
static int kahn(const G *g, int *order, int lex) {
    int indeg[N] = {0}, avail[N], na = 0, cnt = 0;
    for (int u = 0; u < g->n; u++)
        for (int v = 0; v < g->n; v++) indeg[v] += g->e[u][v];
    for (int v = 0; v < g->n; v++)
        if (!indeg[v]) avail[na++] = v;
    while (na > 0) {
        int pick = 0;
        if (lex) {
            for (int i = 1; i < na; i++)
                if (avail[i] < avail[pick]) pick = i;
        } else {
            pick = 0; /* FIFO */
        }
        int u = avail[pick];
        if (lex)
            avail[pick] = avail[--na];
        else {
            memmove(avail, avail + 1, sizeof(int) * (size_t)(na - 1));
            na--;
        }
        order[cnt++] = u;
        for (int v = 0; v < g->n; v++)
            if (g->e[u][v] && --indeg[v] == 0) avail[na++] = v;
    }
    return cnt;
}

static void validate(const G *g, const int *order) {
    int pos[N];
    for (int i = 0; i < g->n; i++) pos[order[i]] = i;
    for (int u = 0; u < g->n; u++)
        for (int v = 0; v < g->n; v++)
            if (g->e[u][v]) check(pos[u] < pos[v], "edge respects order");
}

int main(void) {
    G g;
    memset(&g, 0, sizeof g);
    g.n = N;
    /* random DAG through hidden permutation */
    int perm[N];
    for (int i = 0; i < N; i++) perm[i] = i;
    for (int i = N - 1; i > 0; i--) {
        int j = (int)(rnd() % (unsigned)(i + 1)), t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    int edges = 0;
    for (int i = 0; i < N; i++)
        for (int j = i + 1; j < N; j++)
            if (rnd() % 100 < 12) g.e[perm[i]][perm[j]] = 1, edges++;
    int order[N];
    int c1 = kahn(&g, order, 0);
    check(c1 == N, "dag fully sorted");
    validate(&g, order);
    printf("edges: %d\nfifo order:", edges);
    for (int i = 0; i < N; i++) printf(" %d", order[i]);
    int c2 = kahn(&g, order, 1);
    check(c2 == N, "lex sorted");
    validate(&g, order);
    printf("\nlex order: ");
    for (int i = 0; i < N; i++) printf(" %d", order[i]);
    printf("\n");
    /* add back edges until a cycle appears */
    int added = 0;
    for (;;) {
        int i = (int)(rnd() % N), j = (int)(rnd() % N);
        if (i == j || g.e[perm[j]][perm[i]] || g.e[perm[i]][perm[j]]) continue;
        int lo = i < j ? i : j, hi = i < j ? j : i;
        g.e[perm[hi]][perm[lo]] = 1; /* against topological direction */
        added++;
        int got = kahn(&g, order, 1);
        if (got < N) {
            printf("cycle after %d back edges, emitted %d of %d\n", added, got, N);
            /* everything not emitted lies on or after a cycle */
            int emitted[N] = {0};
            for (int k = 0; k < got; k++) emitted[order[k]] = 1;
            int stuck = 0;
            for (int v = 0; v < N; v++) stuck += !emitted[v];
            check(stuck == N - got, "stuck count");
            printf("stuck vertices:");
            for (int v = 0; v < N; v++)
                if (!emitted[v]) printf(" %d", v);
            printf("\n");
            break;
        }
        validate(&g, order);
    }
    return 0;
}
