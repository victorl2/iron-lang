/*
 * title: Intrusive refcount on a shared DAG
 * topic: memory
 * covers: intrusive reference counting, retain/release, destructor ordering, shared children
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct Node Node;
struct Node {
    int rc;
    int id;
    Node *kids[3];
    int nkids;
};

static int live_nodes;
static int destroy_log[64];
static int destroy_n;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Node *node_new(int id) {
    Node *n = calloc(1, sizeof *n);
    check(n != NULL, "alloc");
    n->rc = 1;
    n->id = id;
    live_nodes++;
    return n;
}

static Node *retain(Node *n) {
    if (n)
        n->rc++;
    return n;
}

static void release(Node *n) {
    if (!n)
        return;
    check(n->rc > 0, "release of dead node");
    if (--n->rc == 0) {
        destroy_log[destroy_n++] = n->id;
        for (int i = 0; i < n->nkids; i++)
            release(n->kids[i]);
        n->id = -1;
        free(n);
        live_nodes--;
    }
}

/* Attaching retains the child: the parent owns one reference. */
static void attach(Node *parent, Node *child) {
    check(parent->nkids < 3, "fanout");
    parent->kids[parent->nkids++] = retain(child);
}

static void print_log(const char *label) {
    printf("%s:", label);
    for (int i = 0; i < destroy_n; i++)
        printf(" %d", destroy_log[i]);
    printf("\n");
    destroy_n = 0;
}

int main(void) {
    /* diamond: root -> a,b ; a -> shared ; b -> shared ; shared -> leaf */
    Node *leaf = node_new(1);
    Node *shared = node_new(2);
    attach(shared, leaf);
    release(leaf); /* drop our local ref, shared owns it now */
    check(leaf->rc == 1, "leaf rc 1");

    Node *a = node_new(3), *b = node_new(4), *root = node_new(5);
    attach(a, shared);
    attach(b, shared);
    release(shared);
    check(shared->rc == 2, "shared rc 2");
    attach(root, a);
    attach(root, b);
    release(a);
    release(b);
    printf("live after build: %d\n", live_nodes);
    printf("rc: root=%d a=%d b=%d shared=%d leaf=%d\n", root->rc, a->rc, b->rc, shared->rc,
           leaf->rc);

    /* keep an extra reference to b across the release of the root */
    Node *keep = retain(b);
    release(root);
    print_log("after root release");
    printf("live: %d, b rc=%d, shared rc=%d\n", live_nodes, keep->rc, shared->rc);
    check(live_nodes == 3, "3 live after root release");
    release(keep);
    print_log("after keep release");
    check(live_nodes == 0, "all freed");

    /* chain of 40 nodes, released from head: destruction order is head first */
    Node *head = node_new(100);
    Node *cur = head;
    for (int i = 1; i < 40; i++) {
        Node *nx = node_new(100 + i);
        attach(cur, nx);
        release(nx);
        cur = nx;
    }
    printf("chain live: %d\n", live_nodes);
    release(head);
    printf("chain destroyed: %d first=%d last=%d\n", destroy_n, destroy_log[0],
           destroy_log[destroy_n - 1]);
    check(live_nodes == 0, "chain freed");
    return 0;
}
