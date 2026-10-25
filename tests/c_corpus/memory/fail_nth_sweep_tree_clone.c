/*
 * title: Failure sweep proving a tree deep-copy rolls back cleanly
 * topic: memory
 * covers: allocation failure injection, recursive rollback, error propagation, leak check per failure point
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long calls, fail_at, live;
static void *xalloc(size_t n) {
    calls++;
    if (fail_at && calls == fail_at) return NULL;
    void *p = malloc(n);
    if (p) live++;
    return p;
}
static void xfree(void *p) { if (p) { live--; free(p); } }

typedef struct Node {
    int key;
    char *label;
    struct Node *l, *r;
} Node;

static unsigned s = 424242u;
static unsigned rnd(void) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

static void tree_free(Node *n) {
    if (!n) return;
    tree_free(n->l); tree_free(n->r);
    xfree(n->label); xfree(n);
}

static Node *node_new(int key, const char *label) {
    Node *n = xalloc(sizeof *n);
    if (!n) return NULL;
    n->label = xalloc(strlen(label) + 1);
    if (!n->label) { xfree(n); return NULL; }
    strcpy(n->label, label);
    n->key = key; n->l = n->r = NULL;
    return n;
}

/* error code by out-param: 0 ok, else the depth at which allocation failed (>0) */
static Node *clone(const Node *src, int depth, int *fail_depth) {
    if (!src) return NULL;
    Node *n = node_new(src->key, src->label);
    if (!n) { *fail_depth = depth; return NULL; }
    n->l = clone(src->l, depth + 1, fail_depth);
    if (src->l && !n->l) { tree_free(n); return NULL; }
    n->r = clone(src->r, depth + 1, fail_depth);
    if (src->r && !n->r) { tree_free(n); return NULL; }
    return n;
}

static Node *insert(Node *root, int key) {
    if (!root) {
        char lab[16];
        snprintf(lab, sizeof lab, "n%d", key);
        return node_new(key, lab);
    }
    if (key < root->key) root->l = insert(root->l, key);
    else if (key > root->key) root->r = insert(root->r, key);
    return root;
}

static int equal(const Node *a, const Node *b) {
    if (!a || !b) return a == b;
    return a->key == b->key && strcmp(a->label, b->label) == 0 && equal(a->l, b->l) && equal(a->r, b->r);
}

static int height(const Node *n) {
    if (!n) return 0;
    int a = height(n->l), b = height(n->r);
    return 1 + (a > b ? a : b);
}

int main(void) {
    Node *root = NULL;
    for (int i = 0; i < 40; i++) root = insert(root, (int)(rnd() % 200));
    long src_live = live;
    int h = height(root);
    printf("source tree: %ld allocations live, height %d\n", src_live, h);

    fail_at = 0; calls = 0;
    int fd = 0;
    Node *c = clone(root, 1, &fd);
    long total = calls;
    if (!c || !equal(root, c) || live != 2 * src_live) { fprintf(stderr, "clone baseline failed\n"); return 1; }
    tree_free(c);
    printf("clone needs %ld allocations\n", total);

    int depth_hist[32] = {0};
    for (long n = 1; n <= total; n++) {
        fail_at = n; calls = 0; fd = 0;
        Node *bad = clone(root, 1, &fd);
        if (bad) { fprintf(stderr, "clone should have failed at %ld\n", n); return 1; }
        if (live != src_live) { fprintf(stderr, "leak at %ld: %ld extra\n", n, live - src_live); return 1; }
        if (fd < 1 || fd > 31) { fprintf(stderr, "bad depth %d\n", fd); return 1; }
        depth_hist[fd]++;
    }
    fail_at = 0;
    printf("failure depth histogram (depth: count of injection points):\n");
    for (int d = 1; d < 32; d++)
        if (depth_hist[d]) printf("  depth %2d: %d\n", d, depth_hist[d]);
    tree_free(root);
    if (live != 0) return 1;
    printf("final live=%ld\n", live);
    return 0;
}
