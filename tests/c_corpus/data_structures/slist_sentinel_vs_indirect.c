/*
 * title: Three ways to write singly list removal and insertion
 * topic: data_structures
 * covers: head special case, dummy sentinel node, pointer-to-pointer, sorted insert, remove, lockstep verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x5BE0CD19137E2179ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 29); }

typedef struct Node { int v; struct Node *next; } Node;
static Node *mk(int v, Node *n) { Node *x = malloc(sizeof *x); CHECK(x); x->v = v; x->next = n; return x; }

static long branches_a, branches_b, branches_c; /* count of special-case branches taken */

/* A: explicit head special case */
static Node *a_insert_sorted(Node *h, int v) {
    if (!h || v < h->v) { branches_a++; return mk(v, h); }
    Node *p = h;
    while (p->next && p->next->v <= v) p = p->next;
    p->next = mk(v, p->next);
    return h;
}
static Node *a_remove(Node *h, int v, int *found) {
    *found = 0;
    if (!h) return NULL;
    if (h->v == v) { branches_a++; Node *n = h->next; free(h); *found = 1; return n; }
    Node *p = h;
    while (p->next && p->next->v != v) p = p->next;
    if (p->next) { Node *d = p->next; p->next = d->next; free(d); *found = 1; }
    return h;
}
/* B: dummy sentinel head */
static Node *b_insert_sorted(Node *h, int v) {
    Node dummy = { 0, h };
    Node *p = &dummy;
    while (p->next && p->next->v <= v) p = p->next;
    p->next = mk(v, p->next);
    branches_b += (p == &dummy);
    return dummy.next;
}
static Node *b_remove(Node *h, int v, int *found) {
    Node dummy = { 0, h };
    Node *p = &dummy; *found = 0;
    while (p->next && p->next->v != v) p = p->next;
    if (p->next) { Node *d = p->next; p->next = d->next; free(d); *found = 1; branches_b += (p == &dummy); }
    return dummy.next;
}
/* C: pointer to the link that points at the current node */
static void c_insert_sorted(Node **head, int v) {
    Node **pp = head;
    while (*pp && (*pp)->v <= v) pp = &(*pp)->next;
    *pp = mk(v, *pp);
    branches_c += (pp == head);
}
static int c_remove(Node **head, int v) {
    for (Node **pp = head; *pp; pp = &(*pp)->next)
        if ((*pp)->v == v) { Node *d = *pp; *pp = d->next; free(d); branches_c += (pp == head); return 1; }
    return 0;
}
static int c_remove_all_if_even(Node **head) {
    int n = 0;
    for (Node **pp = head; *pp;) {
        if ((*pp)->v % 2 == 0) { Node *d = *pp; *pp = d->next; free(d); n++; }
        else pp = &(*pp)->next;
    }
    return n;
}
static int same(const Node *a, const Node *b, const Node *c) {
    while (a && b && c) { if (a->v != b->v || b->v != c->v) return 0; a = a->next; b = b->next; c = c->next; }
    return !a && !b && !c;
}
static void destroy(Node *h) { while (h) { Node *n = h->next; free(h); h = n; } }

int main(void) {
    Node *A = NULL, *B = NULL, *C = NULL;
    int model[600], mn = 0;
    long ins = 0, rem = 0, miss = 0, purged = 0;
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 100;
        int v = (int)(rnd() % 100);
        if (mn > 500) op = 99;
        if (op < 55) {
            A = a_insert_sorted(A, v); B = b_insert_sorted(B, v); c_insert_sorted(&C, v);
            int p = 0; while (p < mn && model[p] <= v) p++;
            for (int i = mn; i > p; i--) model[i] = model[i - 1];
            model[p] = v; mn++; ins++;
        } else if (op < 97) {
            int fa, fb; A = a_remove(A, v, &fa); B = b_remove(B, v, &fb); int fc = c_remove(&C, v);
            int p = 0; while (p < mn && model[p] != v) p++;
            CHECK(fa == fb && fb == fc && fa == (p < mn));
            if (fa) { for (int i = p; i + 1 < mn; i++) model[i] = model[i + 1]; mn--; rem++; } else miss++;
        } else {
            int n = c_remove_all_if_even(&C);
            int k = 0; for (int i = 0; i < mn; i++) if (model[i] % 2) model[k++] = model[i];
            CHECK(n == mn - k); mn = k; purged += n;
            /* rebuild A and B from C to keep lockstep */
            destroy(A); destroy(B); A = B = NULL;
            Node **pa = &A, **pb = &B;
            for (Node *p = C; p; p = p->next) { *pa = mk(p->v, NULL); pa = &(*pa)->next; *pb = mk(p->v, NULL); pb = &(*pb)->next; }
        }
        CHECK(same(A, B, C));
        int i = 0; for (Node *p = C; p; p = p->next) CHECK(i < mn && p->v == model[i++]);
        CHECK(i == mn);
    }
    printf("insert=%ld remove=%ld miss=%ld purged_even=%ld final=%d\n", ins, rem, miss, purged, mn);
    printf("head-case hits: explicit=%ld sentinel=%ld indirect=%ld\n", branches_a, branches_b, branches_c);
    printf("smallest 16:"); { int k = 0; for (Node *p = C; p && k < 16; p = p->next, k++) printf(" %d", p->v); }
    printf("\n");
    destroy(A); destroy(B); destroy(C);
    return 0;
}
