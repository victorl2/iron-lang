/*
 * title: Deep clone of a DAG preserving sharing
 * topic: memory
 * covers: deep copy with memo table, shared substructure stays shared, cycle-safe clone, ownership of cloned graph
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Node Node;
struct Node {
    int label;
    Node *kids[3];
    int nkids;
};

typedef struct {
    Node *from[64];
    Node *to[64];
    int n;
} Memo;

static int live_nodes;
static Node *pool[128];
static int npool;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Node *nn(int label) {
    Node *n = calloc(1, sizeof *n);
    check(n != NULL, "alloc");
    n->label = label;
    live_nodes++;
    pool[npool++] = n;
    return n;
}

static void edge(Node *a, Node *b) { a->kids[a->nkids++] = b; }

static Node *clone_rec(const Node *src, Memo *m) {
    for (int i = 0; i < m->n; i++)
        if (m->from[i] == src)
            return m->to[i];
    Node *c = nn(src->label);
    m->from[m->n] = (Node *)src;
    m->to[m->n++] = c;
    for (int i = 0; i < src->nkids; i++)
        edge(c, clone_rec(src->kids[i], m));
    return c;
}

static Node *dag_clone(const Node *root, int *count) {
    Memo m;
    m.n = 0;
    Node *r = clone_rec(root, &m);
    *count = m.n;
    return r;
}

/* naive tree-style clone (ignores sharing), for comparison; depth-limited so cycles terminate */
static Node *naive_clone(const Node *src, int depth) {
    Node *c = nn(src->label);
    if (depth > 0)
        for (int i = 0; i < src->nkids; i++)
            edge(c, naive_clone(src->kids[i], depth - 1));
    return c;
}

static int reachable(const Node *n, const Node **seen, int *ns) {
    for (int i = 0; i < *ns; i++)
        if (seen[i] == n)
            return 0;
    seen[(*ns)++] = n;
    int c = 1;
    for (int i = 0; i < n->nkids; i++)
        c += reachable(n->kids[i], seen, ns);
    return c;
}

static int distinct_nodes(const Node *root) {
    const Node *seen[64];
    int ns = 0;
    return reachable(root, seen, &ns);
}

static void free_from(Node **arr, int lo, int hi) {
    for (int i = lo; i < hi; i++) {
        free(arr[i]);
        live_nodes--;
        arr[i] = NULL;
    }
}

int main(void) {
    /* diamond with a shared tail and a back edge:  1 -> 2,3 ; 2 -> 4 ; 3 -> 4 ; 4 -> 5 ; 5 -> 2 */
    Node *n1 = nn(1), *n2 = nn(2), *n3 = nn(3), *n4 = nn(4), *n5 = nn(5);
    edge(n1, n2); edge(n1, n3);
    edge(n2, n4); edge(n3, n4);
    edge(n4, n5);
    edge(n5, n2);
    int orig_end = npool;
    printf("original distinct nodes: %d\n", distinct_nodes(n1));

    int cnt;
    Node *c1 = dag_clone(n1, &cnt);
    printf("clone copied %d nodes, distinct in clone: %d\n", cnt, distinct_nodes(c1));
    check(cnt == 5 && distinct_nodes(c1) == 5, "clone size");

    /* sharing preserved: c1->kids[0]->kids[0] and c1->kids[1]->kids[0] are the same node */
    printf("shared tail preserved: %d\n", c1->kids[0]->kids[0] == c1->kids[1]->kids[0]);
    printf("back edge preserved: %d\n", c1->kids[0]->kids[0]->kids[0]->kids[0] == c1->kids[0]);
    check(c1->kids[0]->kids[0] == c1->kids[1]->kids[0], "sharing");
    for (int i = orig_end; i < npool; i++)
        for (int j = 0; j < orig_end; j++)
            check(pool[i] != pool[j], "clone is disjoint from original");

    /* mutate the clone: original untouched */
    c1->label = 100;
    c1->kids[0]->kids[0]->label = 400;
    printf("original labels: %d %d; clone labels: %d %d\n", n1->label, n4->label, c1->label,
           c1->kids[1]->kids[0]->label);
    check(n1->label == 1 && n4->label == 4, "original intact");

    int before = npool;
    Node *naive = naive_clone(n1, 4);
    printf("naive clone (depth 4) made %d nodes, distinct reachable: %d\n", npool - before,
           distinct_nodes(naive));

    printf("live nodes: %d\n", live_nodes);
    free_from(pool, 0, npool);
    check(live_nodes == 0, "leak");
    return 0;
}
