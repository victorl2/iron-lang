/*
 * title: Lock-free inbox stack drained with exchange and reversed to FIFO
 * topic: concurrency
 * covers: CAS push, atomic_exchange take-all, list reversal, ABA-free by construction, per-producer order, malloc'd nodes
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Producers push onto a singly linked stack with a CAS loop. The single consumer never pops
 * one node: it swaps the whole list out with atomic_exchange(head, NULL) and walks the private
 * chain. That makes the structure immune to ABA (no thread ever compares a stale head against a
 * recycled node that it plans to dereference) and lets the consumer free nodes safely.
 * The swapped chain is newest-first; reversing it restores each producer's push order.
 *
 * Oracle: total count and sum, per-producer sequence numbers arriving strictly increasing in
 * the consumer's reversed batches, and a batch counter >= 1.
 */
enum { P = 4, PER = 5000 };

typedef struct Msg {
    struct Msg *next;
    unsigned prod, seq;
} Msg;

static _Atomic(Msg *) inbox;
static atomic_int producers_done;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void *producer(void *p) {
    unsigned id = (unsigned)(size_t)p;
    for (unsigned i = 0; i < PER; i++) {
        Msg *m = malloc(sizeof *m);
        check(m != NULL, "alloc");
        m->prod = id;
        m->seq = i;
        Msg *h = atomic_load_explicit(&inbox, memory_order_relaxed);
        do {
            m->next = h;
        } while (!atomic_compare_exchange_weak_explicit(&inbox, &h, m, memory_order_release,
                                                        memory_order_relaxed));
        if (i % 512 == 0)
            sched_yield();
    }
    atomic_fetch_add(&producers_done, 1);
    return NULL;
}

static Msg *reverse(Msg *h) {
    Msg *r = NULL;
    while (h) {
        Msg *n = h->next;
        h->next = r;
        r = h;
        h = n;
    }
    return r;
}

static long batches, order_errors, total;
static unsigned long sum;
static unsigned last_seq[P];
static int seen_any[P];

static void drain(void) {
    Msg *chain = atomic_exchange_explicit(&inbox, NULL, memory_order_acquire);
    if (!chain)
        return;
    batches++;
    chain = reverse(chain);
    while (chain) {
        Msg *m = chain;
        chain = chain->next;
        check(m->prod < P, "producer id");
        if (seen_any[m->prod] && m->seq <= last_seq[m->prod])
            order_errors++;
        seen_any[m->prod] = 1;
        last_seq[m->prod] = m->seq;
        total++;
        sum += m->prod * 1000000ul + m->seq;
        free(m);
    }
}

int main(void) {
    /* deterministic reversal unit test */
    Msg a = {NULL, 0, 0}, b = {&a, 0, 1}, c = {&b, 0, 2};
    Msg *r = reverse(&c);
    check(r == &a && r->next == &b && r->next->next == &c && c.next == NULL, "reverse");
    printf("reversal of newest-first chain 2,1,0 gives %u,%u,%u\n", r->seq, r->next->seq, r->next->next->seq);

    pthread_t th[P];
    for (int i = 0; i < P; i++)
        check(pthread_create(&th[i], NULL, producer, (void *)(size_t)i) == 0, "create");
    while (atomic_load(&producers_done) < P) {
        drain();
        sched_yield();
    }
    for (int i = 0; i < P; i++)
        pthread_join(th[i], NULL);
    drain();
    unsigned long want = 0;
    for (unsigned p = 0; p < P; p++)
        for (unsigned i = 0; i < PER; i++)
            want += p * 1000000ul + i;
    check(total == (long)P * PER, "message count");
    check(sum == want, "message sum");
    check(order_errors == 0, "per-producer order");
    check(batches >= 1, "at least one batch");
    check(atomic_load(&inbox) == NULL, "inbox empty");
    printf("messages=%ld sum=%lu\n", total, sum);
    printf("per-producer order violations: %ld\n", order_errors);
    return 0;
}
