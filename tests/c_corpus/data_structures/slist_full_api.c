/*
 * title: Singly linked list with indexed API
 * topic: data_structures
 * covers: singly linked list, insert/remove at index, tail pointer, find, model check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 20); }

typedef struct Node { int val; struct Node *next; } Node;
typedef struct { Node *head, *tail; size_t len; } List;

static Node *mk(int v, Node *next) { Node *n = malloc(sizeof *n); CHECK(n); n->val = v; n->next = next; return n; }

static void push_front(List *l, int v) { l->head = mk(v, l->head); if (!l->tail) l->tail = l->head; l->len++; }
static void push_back(List *l, int v) {
    Node *n = mk(v, NULL);
    if (l->tail) l->tail->next = n; else l->head = n;
    l->tail = n; l->len++;
}
static int pop_front(List *l) {
    CHECK(l->head);
    Node *n = l->head; int v = n->val;
    l->head = n->next;
    if (!l->head) l->tail = NULL;
    free(n); l->len--;
    return v;
}
static int pop_back(List *l) {
    CHECK(l->head);
    if (l->head == l->tail) return pop_front(l);
    Node *p = l->head;
    while (p->next != l->tail) p = p->next;
    int v = l->tail->val;
    free(l->tail); p->next = NULL; l->tail = p; l->len--;
    return v;
}
static void insert_at(List *l, size_t i, int v) {
    CHECK(i <= l->len);
    if (i == 0) { push_front(l, v); return; }
    if (i == l->len) { push_back(l, v); return; }
    Node *p = l->head;
    for (size_t k = 1; k < i; k++) p = p->next;
    p->next = mk(v, p->next);
    l->len++;
}
static int remove_at(List *l, size_t i) {
    CHECK(i < l->len);
    if (i == 0) return pop_front(l);
    Node *p = l->head;
    for (size_t k = 1; k < i; k++) p = p->next;
    Node *d = p->next; int v = d->val;
    p->next = d->next;
    if (d == l->tail) l->tail = p;
    free(d); l->len--;
    return v;
}
static int *at(List *l, size_t i) {
    CHECK(i < l->len);
    Node *p = l->head;
    while (i--) p = p->next;
    return &p->val;
}
static long index_of(const List *l, int v) {
    long i = 0;
    for (Node *p = l->head; p; p = p->next, i++) if (p->val == v) return i;
    return -1;
}
static size_t remove_all(List *l, int v) {
    size_t n = 0;
    Node **pp = &l->head;
    Node *last = NULL;
    while (*pp) {
        if ((*pp)->val == v) { Node *d = *pp; *pp = d->next; free(d); n++; l->len--; }
        else { last = *pp; pp = &(*pp)->next; }
    }
    l->tail = last;
    return n;
}
static void clear(List *l) { while (l->head) pop_front(l); }

#define MAXN 512
static int model[MAXN];
static size_t mlen;

static void verify(const List *l) {
    CHECK(l->len == mlen);
    size_t i = 0;
    Node *last = NULL;
    for (Node *p = l->head; p; p = p->next, i++) { CHECK(i < mlen && p->val == model[i]); last = p; }
    CHECK(i == mlen && l->tail == last);
}

int main(void) {
    List l = {0};
    long ops[9] = {0};
    for (int step = 0; step < 5000; step++) {
        unsigned op = rnd() % 100;
        int v = (int)(rnd() % 20);
        if (mlen >= 400) op = 99;
        if (op < 15) { push_front(&l, v); memmove(model + 1, model, mlen * sizeof(int)); model[0] = v; mlen++; ops[0]++; }
        else if (op < 35) { push_back(&l, v); model[mlen++] = v; ops[1]++; }
        else if (op < 50) {
            size_t i = rnd() % (mlen + 1);
            insert_at(&l, i, v); memmove(model + i + 1, model + i, (mlen - i) * sizeof(int)); model[i] = v; mlen++; ops[2]++;
        } else if (op < 58 && mlen) { CHECK(pop_front(&l) == model[0]); memmove(model, model + 1, --mlen * sizeof(int)); ops[3]++; }
        else if (op < 66 && mlen) { CHECK(pop_back(&l) == model[--mlen]); ops[4]++; }
        else if (op < 78 && mlen) {
            size_t i = rnd() % mlen; int e = model[i];
            CHECK(remove_at(&l, i) == e); memmove(model + i, model + i + 1, (mlen - i - 1) * sizeof(int)); mlen--; ops[5]++;
        } else if (op < 86 && mlen) {
            size_t i = rnd() % mlen; *at(&l, i) = v; model[i] = v; ops[6]++;
        } else if (op < 94) {
            long g = -1; for (size_t i = 0; i < mlen; i++) if (model[i] == v) { g = (long)i; break; }
            CHECK(index_of(&l, v) == g); ops[7]++;
        } else if (op < 98) {
            size_t n = remove_all(&l, v), m = 0;
            for (size_t i = 0; i < mlen; i++) if (model[i] != v) model[m++] = model[i];
            CHECK(n == mlen - m); mlen = m; ops[8]++;
        } else if (mlen > 100) {
            while (mlen > 20) { CHECK(pop_front(&l) == model[0]); memmove(model, model + 1, --mlen * sizeof(int)); }
        }
        verify(&l);
    }
    printf("push_front=%ld push_back=%ld insert=%ld pop_front=%ld pop_back=%ld\n", ops[0], ops[1], ops[2], ops[3], ops[4]);
    printf("remove_at=%ld set=%ld find=%ld remove_all=%ld\n", ops[5], ops[6], ops[7], ops[8]);
    printf("final len=%zu:", l.len);
    for (Node *p = l.head; p; p = p->next) printf(" %d", p->val);
    printf("\n");
    clear(&l);
    CHECK(l.head == NULL && l.tail == NULL && l.len == 0);
    return 0;
}
