/*
 * title: Self-organizing list heuristics
 * topic: data_structures
 * covers: move-to-front, transpose, frequency count, search cost, Zipf-like access, list heuristics
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x428A2F98D728AE22ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 31); }

typedef struct Node { int key; long hits; struct Node *next; } Node;
typedef enum { H_NONE, H_MTF, H_TRANSPOSE, H_COUNT, H_COUNT_N } Heur;
static const char *hname[] = { "static", "move-to-front", "transpose", "frequency-count" };

typedef struct { Node *head; Heur h; long cost, lookups; } List;

static void list_build(List *l, Heur h, int n) {
    l->head = NULL; l->h = h; l->cost = l->lookups = 0;
    for (int k = n - 1; k >= 0; k--) { Node *x = malloc(sizeof *x); CHECK(x); x->key = k; x->hits = 0; x->next = l->head; l->head = x; }
}
static void list_free(List *l) { while (l->head) { Node *n = l->head->next; free(l->head); l->head = n; } }

static int lookup(List *l, int key) {
    Node *prev = NULL, *pprev = NULL, *p = l->head;
    long steps = 1;
    while (p && p->key != key) { pprev = prev; prev = p; p = p->next; steps++; }
    CHECK(p != NULL);
    l->cost += steps; l->lookups++;
    p->hits++;
    switch (l->h) {
    case H_MTF:
        if (prev) { prev->next = p->next; p->next = l->head; l->head = p; }
        break;
    case H_TRANSPOSE:
        if (prev) { /* swap p with prev */
            prev->next = p->next; p->next = prev;
            if (pprev) pprev->next = p; else l->head = p;
        }
        break;
    case H_COUNT: {
        /* bubble p toward the front while it has more hits than its predecessor */
        if (prev && prev->hits < p->hits) {
            /* detach p */
            prev->next = p->next;
            Node *q = NULL, *c = l->head;
            while (c->hits >= p->hits) { q = c; c = c->next; }
            p->next = c; if (q) q->next = p; else l->head = p;
        }
        break;
    }
    default: break;
    }
    return (int)steps;
}
static int length(const List *l) { int n = 0; for (Node *p = l->head; p; p = p->next) n++; return n; }

/* Zipf-like sampler: probability ~ 1 / (rank + 1), built from a cumulative table in integers. */
static int sample(const int *cum, int n, int total) {
    int r = (int)(rnd() % (unsigned)total);
    int lo = 0, hi = n - 1;
    while (lo < hi) { int mid = (lo + hi) / 2; if (cum[mid] > r) hi = mid; else lo = mid + 1; }
    return lo;
}

int main(void) {
    enum { N = 100, Q = 20000 };
    int cum[N], total = 0;
    for (int i = 0; i < N; i++) { total += 100000 / (i + 1); cum[i] = total; }
    /* shuffle key identity so rank order differs from initial list order */
    int perm[N]; for (int i = 0; i < N; i++) perm[i] = i;
    for (int i = N - 1; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); int t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
    int *seq = malloc(Q * sizeof *seq); CHECK(seq);
    long counts[N] = {0};
    for (int i = 0; i < Q; i++) { seq[i] = perm[sample(cum, N, total)]; counts[seq[i]]++; }

    long best = -1;
    for (int h = H_NONE; h < H_COUNT_N; h++) {
        List l; list_build(&l, (Heur)h, N);
        for (int i = 0; i < Q; i++) lookup(&l, seq[i]);
        CHECK(length(&l) == N);
        long tot = 0; for (Node *p = l.head; p; p = p->next) { CHECK(p->hits == counts[p->key]); tot += p->hits; }
        CHECK(tot == Q);
        printf("%-16s total_cost=%-8ld avg_x100=%-5ld front:", hname[h], l.cost, l.cost * 100 / Q);
        int k = 0; for (Node *p = l.head; p && k < 5; p = p->next, k++) printf(" %d", p->key);
        printf("\n");
        if (h != H_NONE && (best < 0 || l.cost < best)) best = l.cost;
        if (h == H_COUNT) { /* frequency-count list is sorted by hits descending */
            for (Node *p = l.head; p->next; p = p->next) CHECK(p->hits >= p->next->hits);
        }
        list_free(&l);
    }
    /* offline optimum: static list sorted by true frequency */
    long opt = 0; int order[N];
    for (int i = 0; i < N; i++) order[i] = i;
    for (int i = 1; i < N; i++) { int x = order[i], j = i - 1; while (j >= 0 && (counts[order[j]] < counts[x] || (counts[order[j]] == counts[x] && order[j] > x))) { order[j + 1] = order[j]; j--; } order[j + 1] = x; }
    for (int i = 0; i < N; i++) opt += counts[order[i]] * (i + 1);
    printf("optimal static cost=%ld best heuristic=%ld ratio_x100=%ld\n", opt, best, best * 100 / opt);
    free(seq);
    return 0;
}
