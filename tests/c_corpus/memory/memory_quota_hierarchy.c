/*
 * title: Hierarchical memory quotas per subsystem
 * topic: memory
 * covers: budgets, parent/child charging, rejection at limit, refunds, high-water marks, invariant checks
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Quota {
    const char *name;
    struct Quota *parent;
    size_t limit, used, peak;
    long rejects;
} Quota;

/* charge walks up the chain; on any refusal everything charged so far is refunded */
static int q_charge(Quota *q, size_t n) {
    Quota *c;
    for (c = q; c; c = c->parent) {
        if (c->used + n > c->limit) {
            c->rejects++;
            for (Quota *u = q; u != c; u = u->parent) u->used -= n;
            return 0;
        }
        c->used += n;
        if (c->used > c->peak) c->peak = c->used;
    }
    return 1;
}
static void q_refund(Quota *q, size_t n) {
    for (Quota *c = q; c; c = c->parent) {
        if (c->used < n) { fprintf(stderr, "refund underflow in %s\n", c->name); exit(1); }
        c->used -= n;
    }
}

typedef struct { size_t n; Quota *q; } Hdr;

static void *q_alloc(Quota *q, size_t n) {
    if (!q_charge(q, n)) return NULL;
    Hdr *h = malloc(sizeof(Hdr) + n);
    if (!h) { q_refund(q, n); return NULL; }
    h->n = n; h->q = q;
    return h + 1;
}
static void q_free(void *p) {
    if (!p) return;
    Hdr *h = (Hdr *)p - 1;
    q_refund(h->q, h->n);
    free(h);
}

static unsigned r = 31337;
static unsigned rnd(void) { r = r * 1103515245u + 12345u; return (r >> 8) & 0xFFFFFF; }

static void show(const Quota *q) {
    printf("  %-8s used=%5zu peak=%5zu limit=%5zu rejects=%ld\n", q->name, q->used, q->peak, q->limit, q->rejects);
}

int main(void) {
    Quota total = {"total", NULL, 6000, 0, 0, 0};
    Quota net = {"net", &total, 3000, 0, 0, 0};
    Quota fs = {"fs", &total, 4000, 0, 0, 0};
    Quota rx = {"net.rx", &net, 1500, 0, 0, 0};
    Quota tx = {"net.tx", &net, 2500, 0, 0, 0};
    Quota *leaves[] = {&rx, &tx, &fs};

    void *held[3][64];
    size_t nheld[3] = {0};
    long ok = 0, fail = 0;
    for (int i = 0; i < 3000; i++) {
        int li = (int)(rnd() % 3);
        Quota *q = leaves[li];
        if (rnd() % 100 < 55 && nheld[li] < 64) {
            size_t n = 16 + rnd() % 300;
            void *p = q_alloc(q, n);
            if (p) { held[li][nheld[li]++] = p; ok++; } else fail++;
        } else if (nheld[li]) {
            size_t idx = rnd() % nheld[li];
            q_free(held[li][idx]);
            held[li][idx] = held[li][--nheld[li]];
        }
        /* invariants: parent's usage covers children; nothing exceeds its limit */
        if (net.used != rx.used + tx.used || total.used != net.used + fs.used) { fprintf(stderr, "sum invariant broken at %d\n", i); return 1; }
        if (rx.used > rx.limit || tx.used > tx.limit || fs.used > fs.limit || net.used > net.limit || total.used > total.limit) return 1;
    }
    printf("alloc ok=%ld rejected=%ld\n", ok, fail);
    show(&total); show(&net); show(&fs); show(&rx); show(&tx);

    /* a parent-level refusal must refund the child */
    for (int li = 0; li < 3; li++) { while (nheld[li]) q_free(held[li][--nheld[li]]); }
    Quota tiny = {"tiny", &total, 100000, 0, 0, 0};
    void *big1 = q_alloc(&tiny, 5000);
    void *big2 = q_alloc(&tiny, 2000);
    printf("tiny: first=%d second=%d tiny.used=%zu total.used=%zu total.rejects=%ld\n",
           big1 != NULL, big2 != NULL, tiny.used, total.used, total.rejects);
    q_free(big1);
    printf("after free: total.used=%zu\n", total.used);
    return (total.used == 0 && tiny.used == 0 && net.used == 0) ? 0 : 1;
}
