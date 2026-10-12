/*
 * title: Avoiding stack overflow on deep structures with explicit heap stacks
 * topic: memory
 * covers: recursion to iteration, explicit stack, degenerate tree depth 200000, chunked buffer processing, heap workspace
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Node {
    int val;
    struct Node *left, *right;
} Node;

/* Degenerate chain: every node has only a left child (depth = n). A recursive walk would need n frames. */
static Node *build_chain(int n) {
    Node *root = NULL;
    for (int i = n; i >= 1; i--) {
        Node *x = malloc(sizeof *x);
        if (!x) exit(1);
        x->val = i; x->left = root; x->right = NULL;
        root = x;
    }
    return root;
}

/* branchy tree for variety */
static Node *build_full(int depth, int *ctr) {
    if (depth == 0) return NULL;
    Node *x = malloc(sizeof *x);
    if (!x) exit(1);
    x->val = ++*ctr;
    x->left = build_full(depth - 1, ctr);
    x->right = build_full(depth - 1, ctr);
    return x;
}

typedef struct { Node **a; size_t n, cap; size_t max; } Stack;
static void push(Stack *s, Node *x) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        Node **na = realloc(s->a, s->cap * sizeof *na);
        if (!na) exit(1);
        s->a = na;
    }
    s->a[s->n++] = x;
    if (s->n > s->max) s->max = s->n;
}

/* iterative height: track depth alongside each node */
typedef struct { Node *n; int d; } Item;

static int height_iter(Node *root, size_t *max_stack) {
    Item *st = NULL; size_t n = 0, cap = 0, mx = 0;
    int best = 0;
    if (root) { cap = 16; st = malloc(cap * sizeof *st); if (!st) exit(1); st[n].n = root; st[n].d = 1; n++; }
    while (n) {
        Item it = st[--n];
        if (it.d > best) best = it.d;
        for (int k = 0; k < 2; k++) {
            Node *c = k ? it.n->right : it.n->left;
            if (!c) continue;
            if (n == cap) { cap *= 2; Item *ns = realloc(st, cap * sizeof *ns); if (!ns) exit(1); st = ns; }
            st[n].n = c; st[n].d = it.d + 1; n++;
            if (n > mx) mx = n;
        }
    }
    free(st);
    *max_stack = mx;
    return best;
}

static long sum_iter(Node *root) {
    Stack s = {0};
    long total = 0;
    if (root) push(&s, root);
    while (s.n) {
        Node *x = s.a[--s.n];
        total += x->val;
        if (x->right) push(&s, x->right);
        if (x->left) push(&s, x->left);
    }
    free(s.a);
    return total;
}

static long free_iter(Node *root) {
    Stack s = {0};
    long freed = 0;
    if (root) push(&s, root);
    while (s.n) {
        Node *x = s.a[--s.n];
        if (x->left) push(&s, x->left);
        if (x->right) push(&s, x->right);
        free(x);
        freed++;
    }
    free(s.a);
    return freed;
}

/* Process a 1.5 MB stream through a 4 KB stack chunk instead of a big local array. */
static uint32_t fnv_chunked(uint32_t (*gen)(uint32_t), size_t total, size_t *chunks) {
    unsigned char chunk[4096];
    uint32_t h = 2166136261u;
    size_t done = 0;
    *chunks = 0;
    while (done < total) {
        size_t take = total - done < sizeof chunk ? total - done : sizeof chunk;
        for (size_t i = 0; i < take; i++) chunk[i] = (unsigned char)gen((uint32_t)(done + i));
        for (size_t i = 0; i < take; i++) h = (h ^ chunk[i]) * 16777619u;
        done += take;
        (*chunks)++;
    }
    return h;
}
static uint32_t gen_bytes(uint32_t i) { return (i * 2654435761u) >> 24; }

int main(void) {
    Node *chain = build_chain(200000);
    size_t mx;
    int h = height_iter(chain, &mx);
    long sm = sum_iter(chain);
    printf("chain: height=%d max_stack=%zu sum=%ld\n", h, mx, sm);
    if (h != 200000 || sm != 200000L * 200001L / 2) return 1;
    printf("freed %ld nodes\n", free_iter(chain));

    int ctr = 0;
    Node *full = build_full(14, &ctr);
    h = height_iter(full, &mx);
    sm = sum_iter(full);
    printf("full tree: nodes=%d height=%d max_stack=%zu sum=%ld\n", ctr, h, mx, sm);
    if (sm != (long)ctr * (ctr + 1) / 2) return 1;
    long fr = free_iter(full);
    printf("freed %ld nodes\n", fr);
    if (fr != ctr) return 1;

    size_t total = 1500000, chunks;
    uint32_t hc = fnv_chunked(gen_bytes, total, &chunks);
    /* direct computation without chunking */
    uint32_t hd = 2166136261u;
    for (size_t i = 0; i < total; i++) hd = (hd ^ (unsigned char)gen_bytes((uint32_t)i)) * 16777619u;
    printf("chunked hash=%08x chunks=%zu direct=%08x\n", (unsigned)hc, chunks, (unsigned)hd);
    return hc == hd ? 0 : 1;
}
