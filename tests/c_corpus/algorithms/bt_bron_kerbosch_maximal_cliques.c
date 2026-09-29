/*
 * title: Maximal cliques with Bron-Kerbosch and pivoting
 * topic: algorithms
 * covers: clique enumeration, pivot selection, bitset sets, Moon-Moser graphs, maximum clique brute force, recursion counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 24

typedef struct {
    int n;
    unsigned adj[MAXN];
} Graph;

typedef struct {
    long cliques, calls;
    int max_size;
    int max_clique_count;
    unsigned best;
    unsigned long long size_hist[MAXN + 1];
    unsigned first_max;
} Stats;

static unsigned st = 1618033u;

static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int popcount(unsigned v) {
    int c = 0;
    for (; v; v &= v - 1)
        c++;
    return c;
}

static int lowbit_index(unsigned v) {
    int i = 0;
    while (!((v >> i) & 1u))
        i++;
    return i;
}

static void add_edge(Graph *g, int a, int b) {
    g->adj[a] |= 1u << b;
    g->adj[b] |= 1u << a;
}

/* R: current clique, P: candidates, X: excluded. With `pivot` the branching set is
 * P minus N(u) for the u in P|X with the most neighbours in P. */
static void bk(const Graph *g, unsigned R, unsigned P, unsigned X, int pivot, Stats *s) {
    s->calls++;
    if (P == 0 && X == 0) {
        int sz = popcount(R);
        s->cliques++;
        s->size_hist[sz]++;
        if (sz > s->max_size) {
            s->max_size = sz;
            s->max_clique_count = 0;
            s->first_max = R;
        }
        if (sz == s->max_size)
            s->max_clique_count++;
        return;
    }
    unsigned branch = P;
    if (pivot) {
        int best = -1, bu = 0;
        for (unsigned c = P | X; c; c &= c - 1) {
            int u = lowbit_index(c);
            int d = popcount(P & g->adj[u]);
            if (d > best) {
                best = d;
                bu = u;
            }
        }
        branch = P & ~g->adj[bu];
    }
    for (unsigned c = branch; c; c &= c - 1) {
        int v = lowbit_index(c);
        bk(g, R | (1u << v), P & g->adj[v], X & g->adj[v], pivot, s);
        P &= ~(1u << v);
        X |= 1u << v;
    }
}

static int is_clique(const Graph *g, unsigned m) {
    for (unsigned a = m; a; a &= a - 1) {
        int v = lowbit_index(a);
        if ((m & ~(1u << v)) & ~g->adj[v])
            return 0;
    }
    return 1;
}

static int brute_max_clique(const Graph *g) {
    int best = 0;
    for (unsigned m = 1; m < (1u << g->n); m++) {
        int sz = popcount(m);
        if (sz > best && is_clique(g, m))
            best = sz;
    }
    return best;
}

static void print_set(unsigned m) {
    printf("{");
    int first = 1;
    for (unsigned c = m; c; c &= c - 1) {
        printf("%s%d", first ? "" : ",", lowbit_index(c));
        first = 0;
    }
    printf("}");
}

int main(void) {
    /* Wikipedia example graph: 6 vertices, 5 maximal cliques */
    Graph w;
    memset(&w, 0, sizeof w);
    w.n = 6;
    int e[][2] = {{0, 1}, {0, 4}, {1, 2}, {1, 4}, {2, 3}, {3, 4}, {3, 5}};
    for (int i = 0; i < 7; i++)
        add_edge(&w, e[i][0], e[i][1]);
    Stats s;
    memset(&s, 0, sizeof s);
    bk(&w, 0, (1u << 6) - 1, 0, 1, &s);
    printf("small graph: %ld maximal cliques, largest size %d, ", s.cliques, s.max_size);
    print_set(s.first_max);
    printf("\n");
    check(s.cliques == 5 && s.max_size == 3, "small graph result");

    /* Moon-Moser: complement of disjoint triangles has 3^(n/3) maximal cliques */
    for (int parts = 2; parts <= 6; parts++) {
        Graph g;
        memset(&g, 0, sizeof g);
        g.n = parts * 3;
        for (int a = 0; a < g.n; a++)
            for (int b = a + 1; b < g.n; b++)
                if (a / 3 != b / 3)
                    add_edge(&g, a, b);
        Stats p, q;
        memset(&p, 0, sizeof p);
        memset(&q, 0, sizeof q);
        bk(&g, 0, (1u << g.n) - 1, 0, 1, &p);
        bk(&g, 0, (1u << g.n) - 1, 0, 0, &q);
        long expect = 1;
        for (int i = 0; i < parts; i++)
            expect *= 3;
        check(p.cliques == expect && q.cliques == expect, "Moon-Moser count");
        printf("Moon-Moser n=%2d: cliques=%4ld calls pivot=%5ld plain=%5ld\n", g.n, p.cliques, p.calls, q.calls);
    }
    long tot_cliques = 0, tot_calls_p = 0, tot_calls_n = 0;
    for (int t = 0; t < 40; t++) {
        Graph g;
        memset(&g, 0, sizeof g);
        g.n = 8 + (int)(rnd() % 13);
        unsigned density = 20 + (rnd() % 60);
        for (int a = 0; a < g.n; a++)
            for (int b = a + 1; b < g.n; b++)
                if (rnd() % 100 < density)
                    add_edge(&g, a, b);
        Stats p, q;
        memset(&p, 0, sizeof p);
        memset(&q, 0, sizeof q);
        unsigned all = (1u << g.n) - 1;
        bk(&g, 0, all, 0, 1, &p);
        bk(&g, 0, all, 0, 0, &q);
        check(p.cliques == q.cliques && p.max_size == q.max_size, "pivot and plain agree");
        check(p.max_size == brute_max_clique(&g), "maximum clique matches brute force");
        check(is_clique(&g, p.first_max), "reported clique is a clique");
        unsigned long long sum = 0;
        for (int k = 0; k <= g.n; k++)
            sum += p.size_hist[k];
        check((long)sum == p.cliques, "size histogram totals");
        tot_cliques += p.cliques;
        tot_calls_p += p.calls;
        tot_calls_n += q.calls;
    }
    printf("40 random graphs: maximal cliques=%ld, calls with pivot=%ld, without=%ld\n", tot_cliques, tot_calls_p,
           tot_calls_n);
    check(tot_calls_p <= tot_calls_n, "pivoting reduces recursion");
    return 0;
}
