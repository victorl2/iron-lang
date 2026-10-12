/*
 * title: Shared ownership graph with weak back-edges
 * topic: memory
 * covers: reference cycles, strong vs weak edges, leak of pure-strong cycles, parent pointers as weak refs
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Node Node;
struct Node {
    int strong;
    int weak;
    int alive; /* payload alive (strong > 0) */
    char name[8];
    Node *parent;      /* weak edge */
    Node *kids[4];     /* strong edges */
    int nkids;
    Node *sibling_ref; /* optional strong edge to create cycles */
};

static int live_nodes;
static Node *registry[32];
static int nreg;
static char freed[128];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Node *nn(const char *name) {
    Node *n = calloc(1, sizeof *n);
    check(n != NULL, "alloc");
    n->strong = 1;
    n->alive = 1;
    snprintf(n->name, sizeof n->name, "%s", name);
    live_nodes++;
    registry[nreg++] = n;
    return n;
}

static void weak_drop(Node *n);
static void strong_drop(Node *n);

static void maybe_free(Node *n) {
    if (!n->alive && n->weak == 0) {
        for (int i = 0; i < nreg; i++)
            if (registry[i] == n)
                registry[i] = NULL;
        free(n);
        live_nodes--;
    }
}

static void weak_drop(Node *n) {
    n->weak--;
    maybe_free(n);
}

static void strong_drop(Node *n) {
    if (!n)
        return;
    if (--n->strong > 0)
        return;
    n->alive = 0;
    n->weak++; /* keep the block until teardown finishes */
    strcat(freed, n->name);
    strcat(freed, " ");
    for (int i = 0; i < n->nkids; i++)
        strong_drop(n->kids[i]);
    if (n->sibling_ref)
        strong_drop(n->sibling_ref);
    if (n->parent)
        weak_drop(n->parent);
    n->nkids = 0;
    n->parent = NULL;
    n->sibling_ref = NULL;
    weak_drop(n);
}

static void add_child(Node *p, Node *c, int use_weak_parent) {
    p->kids[p->nkids++] = c;
    c->strong++;
    if (use_weak_parent) {
        c->parent = p;
        p->weak++;
    } else {
        c->parent = p;
        p->strong++; /* naive strong parent pointer: creates a cycle */
    }
}

static void build_tree(int weak_parent) {
    Node *root = nn("root"), *a = nn("a"), *b = nn("b"), *c = nn("c");
    add_child(root, a, weak_parent);
    add_child(root, b, weak_parent);
    add_child(a, c, weak_parent);
    strong_drop(a); /* drop local refs; tree holds them now */
    strong_drop(b);
    strong_drop(c);
    printf("  built: strong root=%d a=%d c=%d weak root=%d\n", root->strong, root->kids[0]->strong,
           root->kids[0]->kids[0]->strong, root->weak);
    freed[0] = 0;
    strong_drop(root);
    printf("  after dropping root: live=%d freed=[%s]\n", live_nodes, freed);
}

int main(void) {
    printf("weak parent pointers:\n");
    build_tree(1);
    check(live_nodes == 0, "weak tree fully freed");

    printf("strong parent pointers (cycle):\n");
    build_tree(0);
    int leaked = live_nodes;
    check(leaked == 4, "cycle leaks 4 nodes");
    /* a "collector": free whatever the registry still holds (unreachable garbage) */
    for (int i = 0; i < nreg; i++)
        if (registry[i]) {
            Node *g = registry[i];
            registry[i] = NULL;
            free(g);
            live_nodes--;
        }
    check(live_nodes == 0, "swept");
    printf("  registry sweep reclaimed them\n");
    nreg = 0;

    printf("sibling strong cycle broken through weak side:\n");
    Node *x = nn("x"), *y = nn("y");
    x->sibling_ref = y;
    y->strong++;
    y->sibling_ref = x;
    x->strong++;
    strong_drop(x);
    strong_drop(y);
    printf("  live=%d (mutual strong refs leak)\n", live_nodes);
    check(live_nodes == 2, "pair leaked");
    /* explicit cycle break: zero one edge and release its target */
    Node *yy = x->sibling_ref;
    x->sibling_ref = NULL;
    freed[0] = 0;
    strong_drop(yy);
    printf("  after breaking: live=%d freed=[%s]\n", live_nodes, freed);
    check(live_nodes == 0, "broken cycle freed");
    return 0;
}
