/*
 * title: Michael-Scott queue over a node pool with tagged indices
 * topic: concurrency
 * covers: lock-free FIFO queue, dummy node, tail helping, tagged (index,tag) links, recycled nodes via free stack
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Links are 64-bit words (tag << 32 | index); index NIL marks the end. A dequeued dummy node
 * returns to a tagged free stack and can be reused as a new tail at once, so the tags on
 * head/tail/next are what defeat ABA. Enqueue may find the tail lagging and help it forward.
 *
 * Correctness argument: linearization points are the successful CAS that links a node after the
 * last node (enqueue) and the successful CAS that advances head (dequeue). Values are read
 * before that CAS but only trusted when the CAS succeeds (tag unchanged).
 */
enum { POOL = 64, NP = 3, NC = 3, PER = 3000 };
#define NIL 0xffffffffu

typedef struct {
    atomic_uint_least64_t next;
    atomic_uint value;
    atomic_uint fl_next; /* free stack link (index+1) */
} Node;

static Node nodes[POOL];
static atomic_uint_least64_t qhead, qtail, freehead;
static atomic_long consumed;
static unsigned long csum[NC];
static long order_errors[NC];

static uint64_t pk(uint32_t tag, uint32_t idx) { return ((uint64_t)tag << 32) | idx; }
static uint32_t tg(uint64_t w) { return (uint32_t)(w >> 32); }
static uint32_t ix(uint64_t w) { return (uint32_t)w; }

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void free_push(uint32_t n) {
    uint64_t h = atomic_load(&freehead);
    do {
        atomic_store(&nodes[n].fl_next, ix(h));
    } while (!atomic_compare_exchange_weak(&freehead, &h, pk(tg(h) + 1, n + 1)));
}

static int free_pop(uint32_t *n) {
    uint64_t h = atomic_load(&freehead);
    for (;;) {
        if (ix(h) == 0)
            return 0;
        uint32_t nx = atomic_load(&nodes[ix(h) - 1].fl_next);
        if (atomic_compare_exchange_weak(&freehead, &h, pk(tg(h) + 1, nx))) {
            *n = ix(h) - 1;
            return 1;
        }
    }
}

static void enqueue(unsigned v) {
    uint32_t n;
    while (!free_pop(&n))
        sched_yield();
    atomic_store(&nodes[n].value, v);
    uint64_t old = atomic_load(&nodes[n].next);
    atomic_store(&nodes[n].next, pk(tg(old), NIL));
    for (;;) {
        uint64_t tail = atomic_load(&qtail);
        uint64_t next = atomic_load(&nodes[ix(tail)].next);
        if (tail != atomic_load(&qtail))
            continue;
        if (ix(next) == NIL) {
            if (atomic_compare_exchange_weak(&nodes[ix(tail)].next, &next, pk(tg(next) + 1, n))) {
                uint64_t t2 = tail;
                atomic_compare_exchange_strong(&qtail, &t2, pk(tg(tail) + 1, n));
                return;
            }
        } else {
            uint64_t t2 = tail; /* tail is lagging: help */
            atomic_compare_exchange_strong(&qtail, &t2, pk(tg(tail) + 1, ix(next)));
        }
    }
}

static int dequeue(unsigned *v) {
    for (;;) {
        uint64_t head = atomic_load(&qhead);
        uint64_t tail = atomic_load(&qtail);
        uint64_t next = atomic_load(&nodes[ix(head)].next);
        if (head != atomic_load(&qhead))
            continue;
        if (ix(head) == ix(tail)) {
            if (ix(next) == NIL)
                return 0;
            uint64_t t2 = tail;
            atomic_compare_exchange_strong(&qtail, &t2, pk(tg(tail) + 1, ix(next)));
        } else {
            unsigned val = atomic_load(&nodes[ix(next)].value);
            uint64_t h2 = head;
            if (atomic_compare_exchange_weak(&qhead, &h2, pk(tg(head) + 1, ix(next)))) {
                *v = val;
                free_push(ix(head)); /* old dummy is recycled */
                return 1;
            }
        }
    }
}

static void *producer(void *p) {
    unsigned id = (unsigned)(size_t)p;
    for (unsigned i = 0; i < (unsigned)PER; i++)
        enqueue(id * 100000u + i);
    return NULL;
}

static void *consumer(void *p) {
    int id = (int)(size_t)p;
    long last[NP];
    for (int i = 0; i < NP; i++)
        last[i] = -1;
    while (atomic_load(&consumed) < (long)NP * PER) {
        unsigned v;
        if (dequeue(&v)) {
            atomic_fetch_add(&consumed, 1);
            unsigned pr = v / 100000u, seq = v % 100000u;
            check(pr < (unsigned)NP, "producer id");
            if ((long)seq <= last[pr])
                order_errors[id]++;
            last[pr] = (long)seq;
            csum[id] += v;
        } else {
            sched_yield();
        }
    }
    return NULL;
}

int main(void) {
    atomic_store(&freehead, 0);
    for (uint32_t i = 1; i < (uint32_t)POOL; i++)
        free_push(i);
    atomic_store(&nodes[0].next, pk(0, NIL));
    atomic_store(&qhead, pk(0, 0));
    atomic_store(&qtail, pk(0, 0));
    pthread_t p[NP], c[NC];
    for (int i = 0; i < NC; i++)
        check(pthread_create(&c[i], NULL, consumer, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < NP; i++)
        check(pthread_create(&p[i], NULL, producer, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < NP; i++)
        pthread_join(p[i], NULL);
    for (int i = 0; i < NC; i++)
        pthread_join(c[i], NULL);
    unsigned long total = 0, want = 0;
    long errs = 0;
    for (int i = 0; i < NC; i++) {
        total += csum[i];
        errs += order_errors[i];
    }
    for (unsigned pr = 0; pr < (unsigned)NP; pr++)
        for (unsigned i = 0; i < (unsigned)PER; i++)
            want += pr * 100000u + i;
    unsigned dummy;
    check(!dequeue(&dummy), "queue empty at end");
    check(total == want, "sum of dequeued values");
    check(errs == 0, "per-producer FIFO order");
    int nfree = 0;
    uint32_t n;
    while (free_pop(&n))
        nfree++;
    check(nfree == POOL - 1, "pool conserved: all but the dummy are free");
    printf("dequeued=%ld sum=%lu\n", atomic_load(&consumed), total);
    printf("per-producer FIFO violations: %ld\n", errs);
    printf("free nodes at end: %d of %d\n", nfree, POOL);
    return 0;
}
