/*
 * title: Cycle detection with Floyd and Brent
 * topic: data_structures
 * covers: linked list cycles, tortoise and hare, Brent's algorithm, cycle entry, cycle length, brute-force check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x0DDBA11C0FFEE123ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 26); }

typedef struct Node { int id; struct Node *next; } Node;

typedef struct { int has_cycle; int entry; int mu; int lambda; long steps; } Result;

static Result floyd(Node *head) {
    Result r = {0, -1, 0, 0, 0};
    Node *t = head, *h = head;
    while (h && h->next) {
        t = t->next; h = h->next->next; r.steps += 3;
        if (t == h) {
            r.has_cycle = 1;
            /* phase 2: find entry */
            t = head; r.mu = 0;
            while (t != h) { t = t->next; h = h->next; r.mu++; r.steps += 2; }
            r.entry = t->id;
            /* phase 3: cycle length */
            Node *p = t->next; r.lambda = 1;
            while (p != t) { p = p->next; r.lambda++; r.steps++; }
            return r;
        }
    }
    return r;
}

static Result brent(Node *head) {
    Result r = {0, -1, 0, 0, 0};
    if (!head) return r;
    long power = 1; int lam = 1;
    Node *t = head, *h = head->next;
    r.steps++;
    while (h && t != h) {
        if (power == lam) { t = h; power *= 2; lam = 0; }
        h = h->next; lam++; r.steps++;
    }
    if (!h) return r;
    r.has_cycle = 1; r.lambda = lam;
    t = h = head;
    for (int i = 0; i < lam; i++) h = h->next;
    while (t != h) { t = t->next; h = h->next; r.mu++; r.steps += 2; }
    r.entry = t->id;
    return r;
}

/* brute force: visited marks by id */
static Result brute(Node *head, int n) {
    Result r = {0, -1, 0, 0, 0};
    int *seen = malloc((size_t)n * sizeof *seen); CHECK(seen);
    for (int i = 0; i < n; i++) seen[i] = -1;
    int pos = 0;
    for (Node *p = head; p; p = p->next, pos++) {
        if (seen[p->id] >= 0) { r.has_cycle = 1; r.entry = p->id; r.mu = seen[p->id]; r.lambda = pos - seen[p->id]; break; }
        seen[p->id] = pos;
    }
    free(seen);
    return r;
}

int main(void) {
    long fs = 0, bs = 0; int cyc = 0, tot = 0;
    static const int fixed[][2] = { {0, -1}, {1, -1}, {1, 0}, {2, 0}, {5, 2}, {8, 7}, {12, 0} };
    for (int c = 0; c < 7 + 300; c++) {
        int n, tailto;
        if (c < 7) { n = fixed[c][0]; tailto = fixed[c][1]; }
        else { n = (int)(rnd() % 60); tailto = (n && rnd() % 3) ? (int)(rnd() % (unsigned)n) : -1; }
        Node *nodes = malloc((size_t)(n ? n : 1) * sizeof *nodes); CHECK(nodes);
        for (int i = 0; i < n; i++) { nodes[i].id = i; nodes[i].next = i + 1 < n ? &nodes[i + 1] : NULL; }
        if (n && tailto >= 0) nodes[n - 1].next = &nodes[tailto];
        Node *head = n ? nodes : NULL;
        Result a = floyd(head), b = brent(head), g = brute(head, n ? n : 1);
        CHECK(a.has_cycle == g.has_cycle && b.has_cycle == g.has_cycle);
        if (g.has_cycle) {
            CHECK(a.entry == g.entry && b.entry == g.entry);
            CHECK(a.mu == g.mu && b.mu == g.mu);
            CHECK(a.lambda == g.lambda && b.lambda == g.lambda);
            CHECK(g.mu + g.lambda == n);
            cyc++;
        }
        if (c < 7) printf("case n=%d tail_to=%d -> cycle=%d entry=%d mu=%d lambda=%d\n", n, tailto, g.has_cycle, g.entry, g.mu, g.lambda);
        fs += a.steps; bs += b.steps; tot++;
        free(nodes);
    }
    printf("cases=%d with_cycle=%d floyd_steps=%ld brent_steps=%ld\n", tot, cyc, fs, bs);
    return 0;
}
