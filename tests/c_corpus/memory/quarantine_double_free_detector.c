/*
 * title: Free quarantine with poison verification and double-free detection
 * topic: memory
 * covers: delayed reuse, FIFO quarantine with byte budget, poison fill, write-after-free detection, double free
 * deps: libc
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POISON 0xDD
#define QBUDGET 256

typedef struct Blk {
    size_t n;
    int state; /* 1 live, 2 quarantined */
    struct Blk *qnext;
    unsigned char data[];
} Blk;

static Blk *qhead, *qtail;
static size_t qbytes;
static int uaf_writes, double_frees, evicted, invalid_frees;
#define NALL 64
static Blk *all[NALL];
static int nall = NALL;

static void *q_alloc(size_t n) {
    Blk *b = malloc(sizeof(Blk) + n);
    if (!b) return NULL;
    b->n = n; b->state = 1; b->qnext = NULL;
    memset(b->data, 0, n);
    for (int i = 0; i < nall; i++)
        if (!all[i]) { all[i] = b; return b->data; }
    free(b);
    return NULL;
}

static int is_known(const void *p) {
    for (int i = 0; i < nall; i++)
        if (all[i] && all[i]->data == p) return 1;
    return 0;
}

static void evict_one(void) {
    Blk *b = qhead;
    qhead = b->qnext;
    if (!qhead) qtail = NULL;
    for (size_t i = 0; i < b->n; i++)
        if (b->data[i] != POISON) { uaf_writes++; break; }
    qbytes -= b->n;
    for (int i = 0; i < nall; i++) if (all[i] == b) all[i] = NULL;
    free(b);
    evicted++;
}

static void q_free(void *p) {
    if (!is_known(p)) { invalid_frees++; return; } /* evicted (or never ours): cannot inspect */
    Blk *b = (Blk *)((unsigned char *)p - offsetof(Blk, data));
    if (b->state == 2) { double_frees++; return; }
    b->state = 2;
    memset(b->data, POISON, b->n);
    b->qnext = NULL;
    if (qtail) qtail->qnext = b; else qhead = b;
    qtail = b;
    qbytes += b->n;
    while (qbytes > QBUDGET) evict_one();
}

static void drain(void) { while (qhead) evict_one(); }

static uint32_t s = 2024u;
static uint32_t rnd(void) { s = s * 1664525u + 1013904223u; return s >> 8; }

int main(void) {
    /* phase 1: plain double free while still in quarantine */
    unsigned char *a = q_alloc(40);
    q_free(a);
    q_free(a);
    printf("phase1: double_frees=%d quarantined=%zu bytes\n", double_frees, qbytes);

    /* phase 2: write after free caught at eviction */
    unsigned char *b = q_alloc(32);
    unsigned char *c = q_alloc(32);
    q_free(b);
    b[5] = 0x42; /* stale write: memory is still valid because it is quarantined */
    q_free(c);
    drain();
    printf("phase2: uaf_writes=%d evicted=%d\n", uaf_writes, evicted);

    /* phase 3: random churn, count budget behaviour */
    unsigned char *live[8] = {0};
    size_t sz[8] = {0};
    int max_q = 0;
    for (int i = 0; i < 400; i++) {
        int slot = (int)(rnd() % 8);
        if (live[slot]) {
            for (size_t k = 0; k < sz[slot]; k++)
                if (live[slot][k] != (unsigned char)(slot + 1)) { fprintf(stderr, "live data corrupted\n"); return 1; }
            q_free(live[slot]);
            live[slot] = NULL;
        } else {
            sz[slot] = 8 + rnd() % 60;
            live[slot] = q_alloc(sz[slot]);
            if (!live[slot]) return 1;
            memset(live[slot], slot + 1, sz[slot]);
        }
        if ((int)qbytes > max_q) max_q = (int)qbytes;
        if (qbytes > QBUDGET) { fprintf(stderr, "quarantine over budget\n"); return 1; }
    }
    printf("phase3: max quarantine=%d (budget %d) evicted=%d\n", max_q, QBUDGET, evicted);
    for (int i = 0; i < 8; i++) if (live[i]) q_free(live[i]);
    drain();
    /* freeing a stale pointer after eviction is reported as unknown, not dereferenced */
    q_free(a);
    printf("phase4: invalid_frees=%d\n", invalid_frees);
    printf("summary: uaf=%d double=%d invalid=%d leftover=%d\n", uaf_writes, double_frees, invalid_frees, qhead != NULL);
    for (int i = 0; i < nall; i++) if (all[i]) return 1;
    return (uaf_writes == 1 && double_frees == 1 && invalid_frees == 1) ? 0 : 1;
}
