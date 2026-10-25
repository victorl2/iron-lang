/*
 * title: Singly linked list reversal variants
 * topic: data_structures
 * covers: linked list reversal, recursion, k-group reversal, sublist reversal, pointer rewiring
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 4101842887655102017ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 22); }

typedef struct Node { int v; struct Node *next; } Node;

static Node *build(int n, int base) {
    Node *h = NULL, **pp = &h;
    for (int i = 0; i < n; i++) { *pp = malloc(sizeof **pp); CHECK(*pp); (*pp)->v = base + i; (*pp)->next = NULL; pp = &(*pp)->next; }
    return h;
}
static void destroy(Node *h) { while (h) { Node *n = h->next; free(h); h = n; } }
static int to_arr(const Node *h, int *a) { int n = 0; for (; h; h = h->next) a[n++] = h->v; return n; }
static void print(const char *label, const Node *h) {
    printf("%-14s:", label);
    for (; h; h = h->next) printf(" %d", h->v);
    printf("\n");
}

static Node *rev_iter(Node *h) {
    Node *prev = NULL;
    while (h) { Node *nx = h->next; h->next = prev; prev = h; h = nx; }
    return prev;
}
static Node *rev_rec(Node *h) {
    if (!h || !h->next) return h;
    Node *r = rev_rec(h->next);
    h->next->next = h; h->next = NULL;
    return r;
}
/* reverse positions [m, n] (1-based, inclusive) */
static Node *rev_between(Node *h, int m, int n) {
    Node dummy = { 0, h };
    Node *pre = &dummy;
    for (int i = 1; i < m; i++) pre = pre->next;
    Node *cur = pre->next;
    for (int i = 0; i < n - m; i++) {
        Node *mv = cur->next;
        cur->next = mv->next;
        mv->next = pre->next;
        pre->next = mv;
    }
    return dummy.next;
}
/* reverse in groups of k; a short tail group is kept when keep_tail else reversed too */
static Node *rev_groups(Node *h, int k, int keep_tail) {
    if (!h) return NULL;
    Node *p = h; int cnt = 0;
    while (p && cnt < k) { p = p->next; cnt++; }
    if (cnt < k && keep_tail) return h;
    Node *prev = rev_groups(p, k, keep_tail), *cur = h;
    for (int i = 0; i < cnt; i++) { Node *nx = cur->next; cur->next = prev; prev = cur; cur = nx; }
    return prev;
}
/* reverse only the values, leaving node identities in place */
static void rev_values(Node *h) {
    int a[256]; int n = to_arr(h, a);
    for (int i = n - 1; i >= 0; i--, h = h->next) h->v = a[i];
}
/* alternate reversal: reverse every second group of k */
static Node *rev_alternate(Node *h, int k) {
    Node dummy = { 0, h };
    Node *tail = &dummy;
    int flip = 1;
    while (h) {
        Node *end = h; int c = 1;
        while (c < k && end->next) { end = end->next; c++; }
        Node *rest = end->next; end->next = NULL;
        if (flip) { tail->next = rev_iter(h); tail = h; }
        else { tail->next = h; tail = end; }
        h = rest; flip = !flip;
    }
    tail->next = NULL;
    return dummy.next;
}

int main(void) {
    Node *h = build(10, 1);
    print("original", h);
    h = rev_iter(h); print("iterative", h);
    h = rev_rec(h); print("recursive", h);
    h = rev_between(h, 3, 7); print("between 3..7", h);
    h = rev_groups(h, 3, 1); print("groups 3 keep", h);
    h = rev_groups(h, 4, 0); print("groups 4 all", h);
    rev_values(h); print("values only", h);
    h = rev_alternate(h, 3); print("alternate 3", h);
    destroy(h);

    /* randomized cross-checks against array reversal */
    for (int t = 0; t < 400; t++) {
        int n = (int)(rnd() % 40);
        int a[64], b[64];
        h = build(n, (int)(rnd() % 100));
        int m = to_arr(h, a);
        CHECK(m == n);
        int op = (int)(rnd() % 4);
        int k = 1 + (int)(rnd() % 6);
        for (int i = 0; i < n; i++) b[i] = a[i];
        if (op == 0) {
            h = rev_iter(h);
            for (int i = 0; i < n; i++) b[i] = a[n - 1 - i];
        } else if (op == 1) {
            h = rev_rec(h);
            for (int i = 0; i < n; i++) b[i] = a[n - 1 - i];
        } else if (op == 2 && n > 0) {
            int lo = 1 + (int)(rnd() % (unsigned)n), hi = lo + (int)(rnd() % (unsigned)(n - lo + 1));
            h = rev_between(h, lo, hi);
            for (int i = lo - 1, j = hi - 1; i < j; i++, j--) { int x = b[i]; b[i] = b[j]; b[j] = x; }
        } else {
            h = rev_groups(h, k, 1);
            for (int s = 0; s + k <= n; s += k) for (int i = s, j = s + k - 1; i < j; i++, j--) { int x = b[i]; b[i] = b[j]; b[j] = x; }
        }
        int c[64]; int cn = to_arr(h, c);
        CHECK(cn == n);
        for (int i = 0; i < n; i++) CHECK(c[i] == b[i]);
        destroy(h);
    }
    printf("randomized reversal checks passed: 400\n");
    return 0;
}
