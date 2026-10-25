/*
 * title: Lock-free bounded MPMC ring with per-cell sequence numbers
 * topic: concurrency
 * covers: C11 atomics, CAS on enqueue/dequeue positions, sequence-stamped cells, wraparound, full/empty detection
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

enum { CAP = 16, MASK = CAP - 1, NP = 4, NC = 3, PER = 700 };

typedef struct {
    atomic_size_t seq;
    unsigned val;
} Cell;

typedef struct {
    Cell cells[CAP];
    atomic_size_t enq, deq;
} Ring;

static Ring ring;
static atomic_long consumed;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void ring_init(Ring *r) {
    for (size_t i = 0; i < CAP; i++) {
        atomic_init(&r->cells[i].seq, i);
        r->cells[i].val = 0;
    }
    atomic_init(&r->enq, 0);
    atomic_init(&r->deq, 0);
}

static int ring_push(Ring *r, unsigned v) {
    size_t pos = atomic_load(&r->enq);
    for (;;) {
        Cell *c = &r->cells[pos & MASK];
        size_t seq = atomic_load(&c->seq);
        ptrdiff_t dif = (ptrdiff_t)(seq - pos);
        if (dif == 0) {
            if (atomic_compare_exchange_weak(&r->enq, &pos, pos + 1)) {
                c->val = v;
                atomic_store(&c->seq, pos + 1);
                return 1;
            }
        } else if (dif < 0) {
            return 0; /* full */
        } else {
            pos = atomic_load(&r->enq);
        }
    }
}

static int ring_pop(Ring *r, unsigned *v) {
    size_t pos = atomic_load(&r->deq);
    for (;;) {
        Cell *c = &r->cells[pos & MASK];
        size_t seq = atomic_load(&c->seq);
        ptrdiff_t dif = (ptrdiff_t)(seq - (pos + 1));
        if (dif == 0) {
            if (atomic_compare_exchange_weak(&r->deq, &pos, pos + 1)) {
                *v = c->val;
                atomic_store(&c->seq, pos + MASK + 1);
                return 1;
            }
        } else if (dif < 0) {
            return 0; /* empty */
        } else {
            pos = atomic_load(&r->deq);
        }
    }
}

typedef struct {
    long got, sum;
    unsigned xr;
    unsigned last[NP];
    int bad;
    long per[NP];
} Cons;

static void *producer(void *arg) {
    unsigned id = (unsigned)(long)arg;
    for (unsigned i = 0; i < PER; i++) {
        unsigned v = id << 16 | i;
        while (!ring_push(&ring, v))
            sched_yield();
    }
    return NULL;
}

static void *consumer(void *arg) {
    Cons *c = arg;
    for (int i = 0; i < NP; i++)
        c->last[i] = 0xffffffffu;
    while (atomic_load(&consumed) < (long)NP * PER) {
        unsigned v;
        if (!ring_pop(&ring, &v)) {
            sched_yield();
            continue;
        }
        atomic_fetch_add(&consumed, 1);
        unsigned p = v >> 16, s = v & 0xffffu;
        if (c->last[p] != 0xffffffffu && s <= c->last[p])
            c->bad++;
        c->last[p] = s;
        c->per[p]++;
        c->got++;
        c->sum += (long)v;
        c->xr ^= v * 2654435761u;
    }
    return NULL;
}

int main(void) {
    ring_init(&ring);
    /* single-threaded behaviour: fill, overflow, drain in FIFO order, repeat over wraparound */
    for (int round = 0; round < 3; round++) {
        int pushed = 0;
        while (ring_push(&ring, 1000u * (unsigned)round + (unsigned)pushed))
            pushed++;
        unsigned v;
        int ordered = 1, popped = 0;
        while (ring_pop(&ring, &v)) {
            if (v != 1000u * (unsigned)round + (unsigned)popped)
                ordered = 0;
            popped++;
        }
        printf("round %d: pushed %d until full, popped %d in order %d\n", round, pushed, popped, ordered);
        check(pushed == CAP && popped == CAP && ordered, "fill and drain");
    }
    pthread_t pt[NP], ct[NC];
    Cons cons[NC] = {{0}};
    for (int i = 0; i < NC; i++)
        check(pthread_create(&ct[i], NULL, consumer, &cons[i]) == 0, "consumer");
    for (long i = 0; i < NP; i++)
        check(pthread_create(&pt[i], NULL, producer, (void *)i) == 0, "producer");
    for (int i = 0; i < NP; i++)
        pthread_join(pt[i], NULL);
    for (int i = 0; i < NC; i++)
        pthread_join(ct[i], NULL);
    long total = 0, sum = 0, per[NP] = {0};
    unsigned xr = 0;
    for (int i = 0; i < NC; i++) {
        check(cons[i].bad == 0, "per-producer order per consumer");
        total += cons[i].got;
        sum += cons[i].sum;
        xr ^= cons[i].xr;
        for (int p = 0; p < NP; p++)
            per[p] += cons[i].per[p];
    }
    long esum = 0;
    unsigned exr = 0;
    for (unsigned p = 0; p < NP; p++)
        for (unsigned i = 0; i < PER; i++) {
            unsigned v = p << 16 | i;
            esum += (long)v;
            exr ^= v * 2654435761u;
        }
    check(total == (long)NP * PER && sum == esum && xr == exr, "multiset preserved");
    unsigned junk;
    check(!ring_pop(&ring, &junk), "ring empty at end");
    for (int p = 0; p < NP; p++)
        printf("producer %d: %ld items consumed\n", p, per[p]);
    printf("total %ld sum %ld xor %08x\n", total, sum, xr);
    return 0;
}
