/*
 * title: Macro-generated typed doubly linked lists
 * topic: data_structures
 * covers: token pasting templates, code generation by macros, typed containers, list sort and filter
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 8675309u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

/* DEFINE_LIST(T, NAME, LESS) generates NAME##_list with a full API. LESS(a, b) is a strict order. */
#define DEFINE_LIST(T, NAME, LESS)                                                      \
    typedef struct NAME##_node { T v; struct NAME##_node *prev, *next; } NAME##_node;    \
    typedef struct { NAME##_node *head, *tail; size_t len; } NAME##_list;                \
    static void NAME##_init(NAME##_list *l) { l->head = l->tail = NULL; l->len = 0; }    \
    static NAME##_node *NAME##_push_back(NAME##_list *l, T v) {                          \
        NAME##_node *n = malloc(sizeof *n);                                              \
        CHECK(n);                                                                        \
        n->v = v; n->next = NULL; n->prev = l->tail;                                     \
        if (l->tail) l->tail->next = n; else l->head = n;                                \
        l->tail = n; l->len++;                                                           \
        return n;                                                                        \
    }                                                                                    \
    static NAME##_node *NAME##_push_front(NAME##_list *l, T v) {                         \
        NAME##_node *n = malloc(sizeof *n);                                              \
        CHECK(n);                                                                        \
        n->v = v; n->prev = NULL; n->next = l->head;                                     \
        if (l->head) l->head->prev = n; else l->tail = n;                                \
        l->head = n; l->len++;                                                           \
        return n;                                                                        \
    }                                                                                    \
    static void NAME##_unlink(NAME##_list *l, NAME##_node *n) {                          \
        if (n->prev) n->prev->next = n->next; else l->head = n->next;                    \
        if (n->next) n->next->prev = n->prev; else l->tail = n->prev;                    \
        free(n); l->len--;                                                               \
    }                                                                                    \
    static T NAME##_pop_front(NAME##_list *l) {                                          \
        CHECK(l->head);                                                                  \
        T v = l->head->v;                                                                \
        NAME##_unlink(l, l->head);                                                       \
        return v;                                                                        \
    }                                                                                    \
    static T NAME##_pop_back(NAME##_list *l) {                                           \
        CHECK(l->tail);                                                                  \
        T v = l->tail->v;                                                                \
        NAME##_unlink(l, l->tail);                                                       \
        return v;                                                                        \
    }                                                                                    \
    static void NAME##_reverse(NAME##_list *l) {                                         \
        NAME##_node *n = l->head;                                                        \
        while (n) { NAME##_node *nx = n->next; n->next = n->prev; n->prev = nx; n = nx; }\
        n = l->head; l->head = l->tail; l->tail = n;                                     \
    }                                                                                    \
    static size_t NAME##_remove_if(NAME##_list *l, int (*pred)(T)) {                     \
        size_t removed = 0;                                                              \
        NAME##_node *n = l->head;                                                        \
        while (n) {                                                                      \
            NAME##_node *nx = n->next;                                                   \
            if (pred(n->v)) { NAME##_unlink(l, n); removed++; }                          \
            n = nx;                                                                      \
        }                                                                                \
        return removed;                                                                  \
    }                                                                                    \
    /* stable bottom-up merge sort on the node chain */                                  \
    static void NAME##_sort(NAME##_list *l) {                                            \
        if (l->len < 2) return;                                                          \
        for (size_t width = 1; width < l->len; width *= 2) {                             \
            NAME##_node *cur = l->head, *newhead = NULL, *newtail = NULL;                \
            while (cur) {                                                                \
                NAME##_node *a = cur, *b = a;                                            \
                for (size_t i = 0; i < width && b; i++) b = b->next;                     \
                NAME##_node *c = b;                                                      \
                for (size_t i = 0; i < width && c; i++) c = c->next;                     \
                NAME##_node *ae = b, *be = c;                                            \
                while (a != ae || b != be) {                                             \
                    NAME##_node *take;                                                   \
                    if (a != ae && (b == be || !(LESS(b->v, a->v)))) { take = a; a = a->next; } \
                    else { take = b; b = b->next; }                                      \
                    take->next = NULL;                                                   \
                    if (newtail) newtail->next = take; else newhead = take;              \
                    newtail = take;                                                      \
                }                                                                        \
                cur = c;                                                                 \
            }                                                                            \
            l->head = newhead;                                                           \
            NAME##_node *p = NULL;                                                       \
            for (NAME##_node *n = l->head; n; n = n->next) { n->prev = p; p = n; }       \
            l->tail = p;                                                                 \
        }                                                                                \
    }                                                                                    \
    static void NAME##_clear(NAME##_list *l) { while (l->head) NAME##_unlink(l, l->head); }

typedef struct { int x, y; } Pt;

#define LESS_INT(a, b) ((a) < (b))
#define LESS_DBL(a, b) ((a) < (b))
#define LESS_PT(a, b) ((a).x < (b).x)
#define LESS_STR(a, b) (strcmp((a), (b)) < 0)

DEFINE_LIST(int, ilist, LESS_INT)
DEFINE_LIST(double, dlist, LESS_DBL)
DEFINE_LIST(Pt, plist, LESS_PT)
DEFINE_LIST(const char *, slist, LESS_STR)

static int is_odd(int v) { return v & 1; }
static int is_neg(double v) { return v < 0; }
static int far_pt(Pt p) { return p.x * p.x + p.y * p.y > 2500; }
static int short_str(const char *s) { return strlen(s) < 4; }

int main(void) {
    /* int list against an array model */
    ilist_list il;
    ilist_init(&il);
    int model[500], mn = 0;
    for (int step = 0; step < 1500; step++) {
        unsigned op = rnd() % 8;
        int v = (int)(rnd() % 1000);
        if (op < 3 && mn < 500) { ilist_push_back(&il, v); model[mn++] = v; }
        else if (op < 5 && mn < 500) { ilist_push_front(&il, v); memmove(model + 1, model, (size_t)mn * sizeof(int)); model[0] = v; mn++; }
        else if (op < 6 && mn) { CHECK(ilist_pop_front(&il) == model[0]); memmove(model, model + 1, (size_t)(mn - 1) * sizeof(int)); mn--; }
        else if (op < 7 && mn) { CHECK(ilist_pop_back(&il) == model[mn - 1]); mn--; }
        CHECK((int)il.len == mn);
    }
    int i = 0;
    for (ilist_node *n = il.head; n; n = n->next) CHECK(n->v == model[i++]);
    for (ilist_node *n = il.tail; n; n = n->prev) CHECK(n->v == model[--i]);
    CHECK(i == 0);
    size_t removed = ilist_remove_if(&il, is_odd);
    int k = 0;
    for (int j = 0; j < mn; j++) if (!(model[j] & 1)) model[k++] = model[j];
    CHECK(il.len == (size_t)k && removed == (size_t)(mn - k));
    mn = k;
    ilist_reverse(&il);
    for (int a = 0, b = mn - 1; a < b; a++, b--) { int t = model[a]; model[a] = model[b]; model[b] = t; }
    i = 0;
    for (ilist_node *n = il.head; n; n = n->next) CHECK(n->v == model[i++]);
    ilist_sort(&il);
    for (ilist_node *n = il.head; n && n->next; n = n->next) {
        CHECK(n->v <= n->next->v);
        CHECK(n->next->prev == n);
    }
    printf("int list: %zu even values, min=%d max=%d, removed %zu odd\n", il.len, il.head->v, il.tail->v, removed);
    ilist_clear(&il);

    dlist_list dl;
    dlist_init(&dl);
    for (int j = 0; j < 200; j++) dlist_push_back(&dl, ((double)(rnd() % 20001) - 10000.0) / 8.0);
    size_t neg = dlist_remove_if(&dl, is_neg);
    dlist_sort(&dl);
    double total = 0;
    for (dlist_node *n = dl.head; n; n = n->next) { total += n->v; if (n->next) CHECK(n->v <= n->next->v); }
    printf("double list: %zu non-negative (dropped %zu), max=%.3f total=%.3f\n", dl.len, neg, dl.tail->v, total);
    dlist_clear(&dl);

    plist_list pl;
    plist_init(&pl);
    for (int j = 0; j < 120; j++) {
        int px = (int)(rnd() % 101) - 50, py = (int)(rnd() % 101) - 50;
        Pt p = { px, py };
        plist_push_back(&pl, p);
    }
    plist_sort(&pl); /* orders by x only */
    size_t far = plist_remove_if(&pl, far_pt);
    for (plist_node *n = pl.head; n && n->next; n = n->next) CHECK(n->v.x <= n->next->v.x);
    printf("point list: %zu within radius 50 (dropped %zu), first x=%d last x=%d\n", pl.len, far, pl.head->v.x, pl.tail->v.x);
    plist_clear(&pl);

    slist_list sl;
    slist_init(&sl);
    static const char *words[] = { "pear", "fig", "apple", "kiwi", "date", "lime", "plum", "sloe", "yam", "quince", "nut" };
    for (int j = 0; j < 11; j++) slist_push_back(&sl, words[j]);
    slist_sort(&sl);
    size_t sh = slist_remove_if(&sl, short_str);
    printf("string list (%zu removed as short):", sh);
    for (slist_node *n = sl.head; n; n = n->next) printf(" %s", n->v);
    printf("\n");
    CHECK(strcmp(slist_pop_front(&sl), "apple") == 0);
    slist_clear(&sl);

    /* deque behaviour of the generated API on every instantiation */
    Pt a = { 1, 2 }, b = { 3, 4 }, c = { 5, 6 };
    plist_push_back(&pl, a); plist_push_back(&pl, b); plist_push_front(&pl, c);
    plist_reverse(&pl);
    CHECK(plist_pop_front(&pl).x == 3 && plist_pop_back(&pl).x == 5 && plist_pop_front(&pl).x == 1 && pl.len == 0);
    slist_push_back(&sl, "b"); slist_push_front(&sl, "a"); slist_push_back(&sl, "c");
    slist_reverse(&sl);
    CHECK(strcmp(slist_pop_back(&sl), "a") == 0 && strcmp(slist_pop_front(&sl), "c") == 0);
    slist_clear(&sl);
    dlist_push_front(&dl, 1.5); dlist_push_back(&dl, 2.5);
    dlist_reverse(&dl);
    CHECK(dlist_pop_front(&dl) == 2.5 && dlist_pop_back(&dl) == 1.5);
    printf("deque round trips ok on point, string and double lists\n");
    return 0;
}
