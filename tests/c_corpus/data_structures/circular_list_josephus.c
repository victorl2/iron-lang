/*
 * title: Circular singly linked list and Josephus problem
 * topic: data_structures
 * covers: circular list, tail pointer, Josephus elimination, closed form recurrence, list rotation, splice
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef struct Node { int id; struct Node *next; } Node;
/* A circular list is represented by a pointer to its LAST node; last->next is the first. */

static Node *cl_append(Node *last, int id) {
    Node *n = malloc(sizeof *n); CHECK(n);
    n->id = id;
    if (!last) { n->next = n; return n; }
    n->next = last->next; last->next = n;
    return n;
}
static Node *cl_build(int n) { Node *last = NULL; for (int i = 1; i <= n; i++) last = cl_append(last, i); return last; }
static Node *cl_rotate(Node *last, int k) { while (k-- > 0 && last) last = last->next; return last; }
static Node *cl_remove_first(Node *last, int *id) {
    Node *first = last->next; *id = first->id;
    if (first == last) { free(first); return NULL; }
    last->next = first->next; free(first);
    return last;
}
/* concatenate b after a in O(1) */
static Node *cl_concat(Node *a, Node *b) {
    if (!a) return b;
    if (!b) return a;
    Node *first_a = a->next;
    a->next = b->next; b->next = first_a;
    return b;
}
static int cl_len(const Node *last) { if (!last) return 0; int n = 1; for (const Node *p = last->next; p != last; p = p->next) n++; return n; }

/* Josephus: every k-th person is eliminated; returns the survivor and fills the elimination order */
static int josephus_list(int n, int k, int *order) {
    Node *last = cl_build(n);
    int cnt = 0, id;
    while (last) {
        /* advance so that the first node is the one to eliminate: skip k-1 */
        last = cl_rotate(last, (k - 1) % (cl_len(last) ? cl_len(last) : 1));
        last = cl_remove_first(last, &id);
        order[cnt++] = id;
    }
    return order[n - 1];
}
/* Recurrence J(1) = 0, J(n) = (J(n-1) + k) mod n, 0-based */
static int josephus_rec(int n, int k) { int r = 0; for (int i = 2; i <= n; i++) r = (r + k) % i; return r + 1; }
/* Array simulation */
static int josephus_array(int n, int k, int *order) {
    int *a = malloc((size_t)n * sizeof *a); CHECK(a);
    for (int i = 0; i < n; i++) a[i] = i + 1;
    int len = n, pos = 0, cnt = 0;
    while (len) {
        pos = (pos + k - 1) % len;
        order[cnt++] = a[pos];
        for (int i = pos; i + 1 < len; i++) a[i] = a[i + 1];
        len--; if (len) pos %= len;
    }
    free(a);
    return order[n - 1];
}
/* power-of-two closed form for k = 2 */
static int josephus_k2(int n) { int p = 1; while (p * 2 <= n) p *= 2; return 2 * (n - p) + 1; }

int main(void) {
    static const int ns[] = { 1, 2, 5, 7, 10, 41, 100 };
    static const int ks[] = { 1, 2, 3, 5 };
    for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
        printf("n=%-3d", ns[i]);
        for (size_t j = 0; j < sizeof ks / sizeof ks[0]; j++) {
            int o1[128], o2[128];
            int a = josephus_list(ns[i], ks[j], o1), b = josephus_array(ns[i], ks[j], o2), c = josephus_rec(ns[i], ks[j]);
            CHECK(a == b && b == c);
            for (int t = 0; t < ns[i]; t++) CHECK(o1[t] == o2[t]);
            if (ks[j] == 2) CHECK(a == josephus_k2(ns[i]));
            printf(" k=%d->%-3d", ks[j], a);
        }
        printf("\n");
    }
    int order[16]; josephus_list(10, 3, order);
    printf("order n=10 k=3:"); for (int i = 0; i < 10; i++) printf(" %d", order[i]);
    printf("\n");
    /* exhaustive comparison of the recurrence with the list simulation */
    long agree = 0;
    for (int n = 1; n <= 60; n++) for (int k = 1; k <= 12; k++) {
        int o[64]; CHECK(josephus_list(n, k, o) == josephus_rec(n, k)); agree++;
    }
    printf("agreement checks=%ld\n", agree);
    /* splice test */
    Node *a = cl_build(4), *b = NULL;
    for (int i = 10; i < 13; i++) b = cl_append(b, i);
    Node *c = cl_concat(a, b);
    printf("concat len=%d:", cl_len(c));
    for (Node *p = c->next; ; p = p->next) { printf(" %d", p->id); if (p == c) break; }
    printf("\n");
    int id; while (c) c = cl_remove_first(c, &id);
    return 0;
}
