/*
 * title: Intrusive lock-free MPSC queue with stub node
 * topic: concurrency
 * covers: atomic exchange push, single consumer pop, stub node re-insertion, transient inconsistent state, per-producer FIFO
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    _Atomic(struct Node *) next;
    unsigned prod, seq;
} Node;

typedef struct {
    _Atomic(Node *) head; /* producers exchange here */
    Node *tail;           /* consumer only */
    Node stub;
} MPSC;

typedef enum { POP_OK, POP_EMPTY, POP_RETRY } PopRes;

enum { NP = 4, PER = 2000 };

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void q_init(MPSC *q) {
    atomic_init(&q->stub.next, NULL);
    atomic_init(&q->head, &q->stub);
    q->tail = &q->stub;
}

static void q_push(MPSC *q, Node *n) {
    atomic_store(&n->next, NULL);
    Node *prev = atomic_exchange(&q->head, n);
    atomic_store(&prev->next, n); /* between the two steps the list looks broken to the consumer */
}

static PopRes q_pop(MPSC *q, Node **out) {
    Node *tail = q->tail;
    Node *next = atomic_load(&tail->next);
    if (tail == &q->stub) {
        if (!next)
            return POP_EMPTY;
        q->tail = next;
        tail = next;
        next = atomic_load(&tail->next);
    }
    if (next) {
        q->tail = next;
        *out = tail;
        return POP_OK;
    }
    Node *head = atomic_load(&q->head);
    if (tail != head)
        return POP_RETRY; /* a producer is between its exchange and its link store */
    q_push(q, &q->stub);
    next = atomic_load(&tail->next);
    if (next) {
        q->tail = next;
        *out = tail;
        return POP_OK;
    }
    return POP_RETRY;
}

static MPSC queue;
static Node *nodes[NP];

static void *producer(void *arg) {
    unsigned id = (unsigned)(long)arg;
    for (unsigned i = 0; i < PER; i++) {
        nodes[id][i].prod = id;
        nodes[id][i].seq = i;
        q_push(&queue, &nodes[id][i]);
    }
    return NULL;
}

int main(void) {
    q_init(&queue);
    Node *n;
    check(q_pop(&queue, &n) == POP_EMPTY, "empty at start");
    for (int p = 0; p < NP; p++) {
        nodes[p] = calloc(PER, sizeof(Node));
        check(nodes[p] != NULL, "alloc");
    }
    /* single-threaded FIFO check with the stub cycling through the list */
    Node local[5];
    for (unsigned i = 0; i < 5; i++) {
        local[i].prod = 9;
        local[i].seq = i;
        q_push(&queue, &local[i]);
    }
    printf("local order:");
    for (int i = 0; i < 5; i++) {
        PopRes r;
        while ((r = q_pop(&queue, &n)) == POP_RETRY)
            ;
        check(r == POP_OK, "pop local");
        printf(" %u", n->seq);
    }
    printf("\n");
    check(q_pop(&queue, &n) == POP_EMPTY, "empty after local drain");

    pthread_t th[NP];
    for (long p = 0; p < NP; p++)
        check(pthread_create(&th[p], NULL, producer, (void *)p) == 0, "producer");
    unsigned last[NP];
    long per[NP] = {0}, total = 0;
    unsigned long long sum = 0;
    for (int p = 0; p < NP; p++)
        last[p] = 0xffffffffu;
    int bad = 0;
    while (total < (long)NP * PER) {
        PopRes r = q_pop(&queue, &n);
        if (r != POP_OK) {
            sched_yield();
            continue;
        }
        check(n->prod < NP, "producer id");
        if (last[n->prod] != 0xffffffffu && n->seq != last[n->prod] + 1)
            bad++;
        if (last[n->prod] == 0xffffffffu && n->seq != 0)
            bad++;
        last[n->prod] = n->seq;
        per[n->prod]++;
        sum += (unsigned long long)n->prod * 100000u + n->seq;
        total++;
    }
    for (int p = 0; p < NP; p++)
        pthread_join(th[p], NULL);
    check(bad == 0, "per-producer FIFO with no gaps");
    unsigned long long expect = 0;
    for (unsigned p = 0; p < NP; p++)
        for (unsigned i = 0; i < PER; i++)
            expect += (unsigned long long)p * 100000u + i;
    check(sum == expect, "sum");
    check(q_pop(&queue, &n) == POP_EMPTY, "empty at end");
    for (int p = 0; p < NP; p++) {
        printf("producer %d: %ld items, last seq %u\n", p, per[p], last[p]);
        free(nodes[p]);
    }
    printf("total %ld checksum %llu\n", total, sum);
    return 0;
}
