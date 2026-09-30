/*
 * title: Word ladder BFS with wildcard buckets
 * topic: algorithms
 * covers: breadth-first search, implicit graph, wildcard bucketing, qsort total order, path reconstruction
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NW = 180, L = 4 };

static unsigned st = 88172645u;
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
    char pat[L + 1];
    int word;
} Entry;

static char words[NW][L + 1];
static Entry ents[NW * L];

static int cmp_entry(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    int c = strcmp(x->pat, y->pat);
    if (c) return c;
    return x->word - y->word;
}
static int cmp_pat_key(const void *k, const void *e) {
    return strcmp((const char *)k, ((const Entry *)e)->pat);
}

static int one_apart(const char *a, const char *b) {
    int diff = 0;
    for (int i = 0; i < L; i++) diff += a[i] != b[i];
    return diff == 1;
}

int main(void) {
    int n = 0;
    while (n < NW) {
        char w[L + 1];
        for (int i = 0; i < L; i++) w[i] = (char)('a' + rnd() % 5);
        w[L] = 0;
        int dup = 0;
        for (int i = 0; i < n; i++)
            if (!strcmp(words[i], w)) dup = 1;
        if (!dup) strcpy(words[n++], w);
    }
    int ne = 0;
    for (int i = 0; i < NW; i++)
        for (int p = 0; p < L; p++) {
            strcpy(ents[ne].pat, words[i]);
            ents[ne].pat[p] = '*';
            ents[ne].word = i;
            ne++;
        }
    qsort(ents, (size_t)ne, sizeof(Entry), cmp_entry);

    int dist[NW], par[NW], q[NW];
    for (int i = 0; i < NW; i++) dist[i] = -1, par[i] = -1;
    int head = 0, tail = 0, probes = 0;
    dist[0] = 0;
    q[tail++] = 0;
    while (head < tail) {
        int u = q[head++];
        for (int p = 0; p < L; p++) {
            char pat[L + 1];
            strcpy(pat, words[u]);
            pat[p] = '*';
            Entry *e = bsearch(pat, ents, (size_t)ne, sizeof(Entry), cmp_pat_key);
            check(e != NULL, "pattern present");
            while (e > ents && strcmp((e - 1)->pat, pat) == 0) e--;
            for (; e < ents + ne && strcmp(e->pat, pat) == 0; e++) {
                probes++;
                int v = e->word;
                if (dist[v] < 0) {
                    dist[v] = dist[u] + 1;
                    par[v] = u;
                    q[tail++] = v;
                }
            }
        }
    }
    /* Reference: pairwise adjacency BFS. */
    int d2[NW];
    for (int i = 0; i < NW; i++) d2[i] = -1;
    head = tail = 0;
    d2[0] = 0;
    q[tail++] = 0;
    while (head < tail) {
        int u = q[head++];
        for (int v = 0; v < NW; v++)
            if (d2[v] < 0 && one_apart(words[u], words[v])) {
                d2[v] = d2[u] + 1;
                q[tail++] = v;
            }
    }
    int reach = 0, far = 0, far_w = 0;
    for (int i = 0; i < NW; i++) {
        check(dist[i] == d2[i], "bucket bfs equals pairwise bfs");
        if (dist[i] >= 0) {
            reach++;
            if (dist[i] > far) far = dist[i], far_w = i;
        }
    }
    printf("words: %d, reachable from %s: %d\n", NW, words[0], reach);
    printf("bucket probes: %d\n", probes);
    printf("eccentricity of %s: %d (%s)\n", words[0], far, words[far_w]);
    int chain[64], cl = 0;
    for (int v = far_w; v != -1; v = par[v]) chain[cl++] = v;
    check(cl == far + 1, "chain length");
    printf("ladder:");
    for (int i = cl - 1; i >= 0; i--) printf(" %s", words[chain[i]]);
    printf("\n");
    for (int i = cl - 1; i > 0; i--) check(one_apart(words[chain[i]], words[chain[i - 1]]), "step");
    for (int d = 0; d <= far; d++) {
        int c = 0;
        for (int i = 0; i < NW; i++) c += dist[i] == d;
        printf("level %d: %d\n", d, c);
    }
    return 0;
}
