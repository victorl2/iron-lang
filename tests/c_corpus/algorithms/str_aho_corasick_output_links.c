/*
 * title: Aho-Corasick with output links
 * topic: algorithms
 * covers: Aho-Corasick automaton, failure links, dictionary suffix links, goto completion, multi-pattern matching
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 0x9e3779b97f4a7c15ULL;

static inline unsigned rnd(void) {
    unsigned long long z = (rng_s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (unsigned)((z ^ (z >> 31)) >> 16);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline void rand_str(char *s, int n, int alpha) {
    for (int i = 0; i < n; i++)
        s[i] = (char)('a' + rnd() % (unsigned)alpha);
    s[n] = 0;
}

enum { ALPHA = 26, MAXNODE = 512 };

typedef struct {
    int next[ALPHA];
    int fail;
    int out_link; /* nearest proper suffix node that ends a pattern */
    int pat;      /* pattern index ending here or -1 */
} Node;

static Node nodes[MAXNODE];
static int node_count;

static int new_node(void) {
    Node *n = &nodes[node_count];
    for (int i = 0; i < ALPHA; i++)
        n->next[i] = -1;
    n->fail = 0;
    n->out_link = -1;
    n->pat = -1;
    return node_count++;
}

static void insert(const char *w, int id) {
    int cur = 0;
    for (; *w; w++) {
        int c = *w - 'a';
        if (nodes[cur].next[c] < 0) {
            int nn = new_node();
            nodes[cur].next[c] = nn;
        }
        cur = nodes[cur].next[c];
    }
    nodes[cur].pat = id;
}

static void build(void) {
    int queue[MAXNODE], qh = 0, qt = 0;
    for (int c = 0; c < ALPHA; c++) {
        int v = nodes[0].next[c];
        if (v < 0)
            nodes[0].next[c] = 0;
        else {
            nodes[v].fail = 0;
            queue[qt++] = v;
        }
    }
    while (qh < qt) {
        int u = queue[qh++];
        int f = nodes[u].fail;
        nodes[u].out_link = nodes[f].pat >= 0 ? f : nodes[f].out_link;
        for (int c = 0; c < ALPHA; c++) {
            int v = nodes[u].next[c];
            if (v < 0)
                nodes[u].next[c] = nodes[f].next[c];
            else {
                nodes[v].fail = nodes[f].next[c];
                queue[qt++] = v;
            }
        }
    }
}

int main(void) {
    const char *dict[] = {"he", "she", "his", "hers", "is", "a", "ash", "shes"};
    enum { NP = 8 };
    int len[NP];
    new_node();
    for (int i = 0; i < NP; i++) {
        insert(dict[i], i);
        len[i] = (int)strlen(dict[i]);
    }
    build();
    printf("trie nodes: %d\n", node_count);

    const char *text = "ushershishashes";
    int n = (int)strlen(text);
    int counts[NP] = {0};
    int cur = 0, total = 0;
    for (int i = 0; i < n; i++) {
        cur = nodes[cur].next[text[i] - 'a'];
        int v = nodes[cur].pat >= 0 ? cur : nodes[cur].out_link;
        while (v >= 0) {
            int p = nodes[v].pat;
            counts[p]++;
            total++;
            printf("  end %2d: %s (start %d)\n", i, dict[p], i - len[p] + 1);
            v = nodes[v].out_link;
        }
    }
    /* brute force cross-check on demo */
    int bc[NP] = {0};
    for (int p = 0; p < NP; p++)
        for (int i = 0; i + len[p] <= n; i++)
            if (strncmp(text + i, dict[p], (size_t)len[p]) == 0)
                bc[p]++;
    for (int p = 0; p < NP; p++)
        check(bc[p] == counts[p], "demo counts");
    printf("total matches: %d\n", total);

    /* random large text */
    static char big[8001];
    rand_str(big, 8000, 5);
    for (int i = 0; i < 8000; i++)
        big[i] = "hesia"[big[i] - 'a'];
    int c2[NP] = {0};
    cur = 0;
    for (int i = 0; i < 8000; i++) {
        cur = nodes[cur].next[big[i] - 'a'];
        int v = nodes[cur].pat >= 0 ? cur : nodes[cur].out_link;
        for (; v >= 0; v = nodes[v].out_link)
            c2[nodes[v].pat]++;
    }
    for (int p = 0; p < NP; p++) {
        int b = 0;
        for (int i = 0; i + len[p] <= 8000; i++)
            if (strncmp(big + i, dict[p], (size_t)len[p]) == 0)
                b++;
        check(b == c2[p], "random counts");
        printf("%-5s %d\n", dict[p], c2[p]);
    }
    return 0;
}
