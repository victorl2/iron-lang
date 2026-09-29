/*
 * title: Three-level bitmap priority queue with FIFO buckets
 * topic: data_structures
 * covers: hierarchical bitmap, find-first-set without builtins, per-priority FIFO lists, next non-empty priority, cancel by handle
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0xB17Aull;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* index of the lowest set bit of a nonzero word */
static int ctz64(uint64_t x) {
    int n = 0;
    if ((x & 0xFFFFFFFFull) == 0) {
        n += 32;
        x >>= 32;
    }
    if ((x & 0xFFFFull) == 0) {
        n += 16;
        x >>= 16;
    }
    if ((x & 0xFFull) == 0) {
        n += 8;
        x >>= 8;
    }
    if ((x & 0xFull) == 0) {
        n += 4;
        x >>= 4;
    }
    if ((x & 0x3ull) == 0) {
        n += 2;
        x >>= 2;
    }
    if ((x & 1ull) == 0)
        n += 1;
    return n;
}

enum { PRIOS = 64 * 64 * 64, MAXT = 3000 };

typedef struct {
    int prio, next, prev;
    int queued;
    long seq;
} Task;

typedef struct {
    uint64_t l0[PRIOS / 64]; /* bit p set: bucket p non-empty */
    uint64_t l1[PRIOS / 4096];
    uint64_t l2;
    int head[PRIOS], tail[PRIOS];
    Task t[MAXT];
    long seq;
    long ops_bits; /* number of word probes, for the report */
} BPQ;

static BPQ *bq_new(void) {
    BPQ *q = calloc(1, sizeof(BPQ));
    for (int i = 0; i < PRIOS; i++)
        q->head[i] = q->tail[i] = -1;
    return q;
}

static void set_bit(BPQ *q, int p) {
    q->l0[p >> 6] |= 1ull << (p & 63);
    q->l1[p >> 12] |= 1ull << ((p >> 6) & 63);
    q->l2 |= 1ull << (p >> 12);
}

static void clear_bit(BPQ *q, int p) {
    q->l0[p >> 6] &= ~(1ull << (p & 63));
    if (q->l0[p >> 6] == 0) {
        q->l1[p >> 12] &= ~(1ull << ((p >> 6) & 63));
        if (q->l1[p >> 12] == 0)
            q->l2 &= ~(1ull << (p >> 12));
    }
}

static void enqueue(BPQ *q, int id, int prio) {
    Task *t = &q->t[id];
    t->prio = prio;
    t->seq = q->seq++;
    t->queued = 1;
    t->next = -1;
    t->prev = q->tail[prio];
    if (q->tail[prio] >= 0)
        q->t[q->tail[prio]].next = id;
    else
        q->head[prio] = id;
    q->tail[prio] = id;
    set_bit(q, prio);
}

static void unlink_task(BPQ *q, int id) {
    Task *t = &q->t[id];
    if (t->prev >= 0)
        q->t[t->prev].next = t->next;
    else
        q->head[t->prio] = t->next;
    if (t->next >= 0)
        q->t[t->next].prev = t->prev;
    else
        q->tail[t->prio] = t->prev;
    t->queued = 0;
    if (q->head[t->prio] < 0)
        clear_bit(q, t->prio);
}

/* smallest non-empty priority >= p, or -1 */
static int next_prio(BPQ *q, int p) {
    /* level 0 within the word */
    uint64_t w = q->l0[p >> 6] & (~0ull << (p & 63));
    q->ops_bits++;
    if (w)
        return (p & ~63) | ctz64(w);
    /* level 1 within the word above this level-0 word */
    int i0 = (p >> 6) & 63;
    uint64_t w1 = i0 == 63 ? 0 : q->l1[p >> 12] & (~0ull << (i0 + 1));
    q->ops_bits++;
    if (w1) {
        int j = ctz64(w1);
        return (p & ~4095) | (j << 6) | ctz64(q->l0[((p >> 12) << 6) | j]);
    }
    int i1 = p >> 12;
    uint64_t w2 = i1 == 63 ? 0 : q->l2 & (~0ull << (i1 + 1));
    q->ops_bits++;
    if (!w2)
        return -1;
    int a = ctz64(w2);
    int b = ctz64(q->l1[a]);
    return (a << 12) | (b << 6) | ctz64(q->l0[(a << 6) | b]);
}

static int dequeue(BPQ *q) {
    if (!q->l2)
        return -1;
    int a = ctz64(q->l2), b = ctz64(q->l1[a]);
    int p = (a << 12) | (b << 6) | ctz64(q->l0[(a << 6) | b]);
    q->ops_bits += 3;
    int id = q->head[p];
    unlink_task(q, id);
    return id;
}

int main(void) {
    BPQ *q = bq_new();
    int nq = 0, enq = 0, deq = 0, cancels = 0, probes = 0;
    long chk = 0;
    int free_ids[MAXT], nfree = 0;
    for (int i = MAXT - 1; i >= 0; i--)
        free_ids[nfree++] = i;
    for (int op = 0; op < 20000; op++) {
        int r = (int)(rng() % 100);
        if ((r < 50 || nq == 0) && nfree > 0) {
            int id = free_ids[--nfree];
            int shape = (int)(rng() % 4), prio;
            if (shape == 0)
                prio = (int)(rng() % 8); /* very hot low priorities: long FIFOs */
            else if (shape == 1)
                prio = (int)(rng() % 512);
            else if (shape == 2)
                prio = 4096 * (int)(rng() % 64) + (int)(rng() % 3);
            else
                prio = (int)(rng() % PRIOS);
            enqueue(q, id, prio);
            nq++;
            enq++;
        } else if (r < 75 && nq > 0) {
            /* model: minimum (prio, seq) among queued tasks */
            int best = -1;
            for (int i = 0; i < MAXT; i++)
                if (q->t[i].queued && (best < 0 || q->t[i].prio < q->t[best].prio || (q->t[i].prio == q->t[best].prio && q->t[i].seq < q->t[best].seq)))
                    best = i;
            int got = dequeue(q);
            check(got == best, "dequeue returns the highest-priority oldest task");
            chk += (long)q->t[got].prio * 3 + got % 11;
            free_ids[nfree++] = got;
            nq--;
            deq++;
        } else if (r < 85 && nq > 0) {
            int id = (int)(rng() % MAXT);
            while (!q->t[id].queued)
                id = (id + 1) % MAXT;
            unlink_task(q, id);
            free_ids[nfree++] = id;
            nq--;
            cancels++;
        } else {
            int p = (int)(rng() % PRIOS), want = -1;
            for (int i = 0; i < MAXT; i++)
                if (q->t[i].queued && q->t[i].prio >= p && (want < 0 || q->t[i].prio < want))
                    want = q->t[i].prio;
            check(next_prio(q, p) == want, "next non-empty priority at or above p");
            probes++;
        }
    }
    int mismatch_bits = 0;
    for (int p = 0; p < PRIOS; p += 61) {
        int nonempty = q->head[p] >= 0;
        int bit = (int)((q->l0[p >> 6] >> (p & 63)) & 1);
        if (nonempty != bit)
            mismatch_bits++;
    }
    check(mismatch_bits == 0, "bitmap agrees with the buckets");
    check(ctz64(1ull << 63) == 63 && ctz64(1) == 0 && ctz64(0x00F0ull) == 4, "ctz64 spot checks");
    printf("enqueue=%d dequeue=%d cancel=%d next_prio_queries=%d queued=%d\n", enq, deq, cancels, probes, nq);
    printf("checksum=%ld word_probes=%ld\n", chk, q->ops_bits);
    free(q);
    return 0;
}
