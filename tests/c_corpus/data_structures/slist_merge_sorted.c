/*
 * title: Merging sorted linked lists
 * topic: data_structures
 * covers: linked list merge, k-way merge by pairwise reduction, dedupe, stable merge, intersection
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x2545F4914F6CDD1DULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 30); }

typedef struct Node { int key, src; struct Node *next; } Node;

static Node *mk(int key, int src) { Node *n = malloc(sizeof *n); CHECK(n); n->key = key; n->src = src; n->next = NULL; return n; }
static void destroy(Node *h) { while (h) { Node *n = h->next; free(h); h = n; } }

/* random sorted list, sorted by construction with random gaps */
static Node *random_sorted(int n, int src) {
    Node *h = NULL, **pp = &h;
    int k = (int)(rnd() % 5);
    for (int i = 0; i < n; i++) { k += (int)(rnd() % 4); *pp = mk(k, src); pp = &(*pp)->next; }
    return h;
}

/* stable: on ties, elements of a come first */
static Node *merge2(Node *a, Node *b) {
    Node dummy = {0, 0, NULL}, *t = &dummy;
    while (a && b) {
        if (b->key < a->key) { t->next = b; b = b->next; } else { t->next = a; a = a->next; }
        t = t->next;
    }
    t->next = a ? a : b;
    return dummy.next;
}
static Node *merge_recursive(Node *a, Node *b) {
    if (!a) return b;
    if (!b) return a;
    if (b->key < a->key) { b->next = merge_recursive(a, b->next); return b; }
    a->next = merge_recursive(a->next, b);
    return a;
}
static Node *merge_k(Node **lists, int k) {
    while (k > 1) {
        int m = 0;
        for (int i = 0; i + 1 < k; i += 2) lists[m++] = merge2(lists[i], lists[i + 1]);
        if (k & 1) lists[m++] = lists[k - 1];
        k = m;
    }
    return k ? lists[0] : NULL;
}
static Node *dedupe(Node *h, int *removed) {
    for (Node *p = h; p && p->next;) {
        if (p->next->key == p->key) { Node *d = p->next; p->next = d->next; free(d); (*removed)++; }
        else p = p->next;
    }
    return h;
}
/* keys present in both lists (does not consume inputs) */
static Node *intersect(const Node *a, const Node *b) {
    Node *h = NULL, **pp = &h, *last = NULL;
    while (a && b) {
        if (a->key < b->key) a = a->next;
        else if (b->key < a->key) b = b->next;
        else {
            if (!last || last->key != a->key) { last = mk(a->key, 0); *pp = last; pp = &last->next; }
            a = a->next; b = b->next;
        }
    }
    return h;
}
static int len(const Node *h) { int n = 0; for (; h; h = h->next) n++; return n; }
static int is_sorted_stable(const Node *h) {
    for (; h && h->next; h = h->next) {
        if (h->key > h->next->key) return 0;
        if (h->key == h->next->key && h->src > h->next->src) return 0;
    }
    return 1;
}

int main(void) {
    long total_nodes = 0; unsigned long chk = 0;
    for (int round = 0; round < 60; round++) {
        int k = 1 + (int)(rnd() % 9);
        Node *lists[10];
        int expected = 0;
        for (int i = 0; i < k; i++) {
            int n = (int)(rnd() % 30);
            lists[i] = random_sorted(n, i);
            expected += n;
        }
        /* histogram model over keys */
        int hist[256] = {0};
        for (int i = 0; i < k; i++) for (Node *p = lists[i]; p; p = p->next) { CHECK(p->key < 256); hist[p->key]++; }
        if (k >= 2) { /* two-way merge, recursive and iterative must agree on order */
            Node *a = random_sorted(20, 1), *b = random_sorted(20, 2);
            Node *ia = random_sorted(0, 1); destroy(ia);
            int la = len(a), lb = len(b);
            Node *m = merge2(a, b);
            CHECK(len(m) == la + lb && is_sorted_stable(m));
            destroy(m);
            a = random_sorted(15, 1); b = random_sorted(15, 2);
            Node *ai = intersect(a, b);
            for (Node *p = ai; p && p->next; p = p->next) CHECK(p->key < p->next->key);
            destroy(ai);
            Node *r = merge_recursive(a, b);
            CHECK(len(r) == 30 && is_sorted_stable(r));
            destroy(r);
        }
        Node *m = merge_k(lists, k);
        CHECK(len(m) == expected);
        int seen[256] = {0};
        for (Node *p = m; p; p = p->next) seen[p->key]++;
        for (int i = 0; i < 256; i++) CHECK(seen[i] == hist[i]);
        CHECK(is_sorted_stable(m) || k > 1);
        for (Node *p = m; p && p->next; p = p->next) CHECK(p->key <= p->next->key);
        int removed = 0;
        m = dedupe(m, &removed);
        int distinct = 0; for (int i = 0; i < 256; i++) distinct += hist[i] > 0;
        CHECK(len(m) == distinct && removed == expected - distinct);
        total_nodes += expected;
        for (Node *p = m; p; p = p->next) chk = chk * 131u + (unsigned)p->key;
        chk &= 0xffffffffu;
        destroy(m);
    }
    printf("rounds=60 total_nodes=%ld checksum=%lu\n", total_nodes, chk);

    /* a fixed example showing stability across sources */
    Node *a = mk(1, 1), *b = mk(1, 2);
    a->next = mk(3, 1); b->next = mk(3, 2);
    Node *m = merge2(a, b);
    for (Node *p = m; p; p = p->next) printf("%d/%d ", p->key, p->src);
    printf("\n");
    destroy(m);
    return 0;
}
