/*
 * title: Linked list quicksort with three-way partition
 * topic: data_structures
 * covers: list quicksort, three-way partition, relinking without allocation, duplicates, tail concatenation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x5DEECE66DULL * 977ULL + 11ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 21); }

typedef struct Node { int key, seq; struct Node *next; } Node;

typedef struct { Node *head, *tail; } Seg;

static void seg_add(Seg *s, Node *n) {
    n->next = NULL;
    if (s->tail) s->tail->next = n; else s->head = n;
    s->tail = n;
}

static long partitions, pivots_seen, max_depth;

/* Sorts by key; the sort is stable because segments are built by appending in order. */
static void qsort_list(Node *h, Node **out_head, Node **out_tail, int depth) {
    if (!h) { *out_head = *out_tail = NULL; return; }
    if (depth > max_depth) max_depth = depth;
    if (!h->next) { *out_head = *out_tail = h; return; }
    /* median of first, middle, last keys */
    int n = 0; Node *p = h;
    for (; p; p = p->next) n++;
    Node *mid = h, *last = h;
    for (int i = 0; i < n / 2; i++) mid = mid->next;
    while (last->next) last = last->next;
    int a = h->key, b = mid->key, c = last->key;
    int pivot = (a < b) ? ((b < c) ? b : (a < c ? c : a)) : ((a < c) ? a : (b < c ? c : b));
    Seg lt = {0}, eq = {0}, gt = {0};
    while (h) {
        Node *nx = h->next;
        if (h->key < pivot) seg_add(&lt, h);
        else if (h->key == pivot) seg_add(&eq, h);
        else seg_add(&gt, h);
        h = nx;
    }
    partitions++; pivots_seen++;
    Node *lh, *lt_, *gh, *gt_;
    qsort_list(lt.head, &lh, &lt_, depth + 1);
    qsort_list(gt.head, &gh, &gt_, depth + 1);
    Node *head = NULL, *tail = NULL;
    Node *parts[3][2] = { {lh, lt_}, {eq.head, eq.tail}, {gh, gt_} };
    for (int i = 0; i < 3; i++) {
        if (!parts[i][0]) continue;
        if (tail) tail->next = parts[i][0]; else head = parts[i][0];
        tail = parts[i][1];
    }
    tail->next = NULL;
    *out_head = head; *out_tail = tail;
}

static int cmp_rec(const void *x, const void *y) {
    const Node *a = *(Node *const *)x, *b = *(Node *const *)y;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

static Node *make(int n, int range) {
    Node *h = NULL, **pp = &h;
    for (int i = 0; i < n; i++) {
        Node *x = malloc(sizeof *x); CHECK(x);
        x->key = (int)(rnd() % (unsigned)range); x->seq = i; x->next = NULL;
        *pp = x; pp = &x->next;
    }
    return h;
}

int main(void) {
    static const int cfg[][2] = { {0, 1}, {1, 1}, {2, 2}, {50, 3}, {200, 1000}, {500, 5}, {1000, 100000}, {800, 1} };
    for (size_t t = 0; t < sizeof cfg / sizeof cfg[0]; t++) {
        int n = cfg[t][0], range = cfg[t][1];
        Node *h = make(n, range);
        Node **arr = malloc((size_t)(n ? n : 1) * sizeof *arr); CHECK(arr);
        int i = 0;
        for (Node *p = h; p; p = p->next) arr[i++] = p;
        qsort(arr, (size_t)n, sizeof *arr, cmp_rec);
        partitions = 0; max_depth = 0;
        Node *sh, *st;
        qsort_list(h, &sh, &st, 0);
        i = 0;
        long chain = 0;
        for (Node *p = sh; p; p = p->next, i++) {
            CHECK(i < n && p == arr[i]); /* identical node identity, includes stability */
            chain += (long)p->key * (i % 7 + 1);
            if (!p->next) CHECK(p == st);
        }
        CHECK(i == n);
        printf("n=%-5d range=%-6d partitions=%-4ld depth=%-3ld weighted=%ld\n", n, range, partitions, max_depth, chain);
        for (Node *p = sh; p;) { Node *nx = p->next; free(p); p = nx; }
        free(arr);
    }
    return 0;
}
