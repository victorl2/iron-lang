/*
 * title: Atomic ticket lock with proportional backoff and grant order log
 * topic: concurrency
 * covers: ticket lock on atomics, next/serving counters, proportional backoff, FIFO grant order, unsigned wraparound
 * deps: libc, pthread
 */
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    atomic_uint next;
    atomic_uint serving;
} TicketLock;

/* Counters start near UINT_MAX so the comparison logic must survive wraparound. */
static TicketLock tl = {UINT_MAX - 100u, UINT_MAX - 100u};

static unsigned ticket_lock(TicketLock *l) {
    unsigned my = atomic_fetch_add_explicit(&l->next, 1u, memory_order_relaxed);
    for (;;) {
        unsigned cur = atomic_load_explicit(&l->serving, memory_order_acquire);
        if (cur == my)
            return my;
        unsigned distance = my - cur; /* proportional backoff: wait longer the further back we are */
        for (unsigned i = 0; i < distance && i < 4; i++)
            sched_yield();
    }
}
static void ticket_unlock(TicketLock *l) {
    unsigned cur = atomic_load_explicit(&l->serving, memory_order_relaxed);
    atomic_store_explicit(&l->serving, cur + 1u, memory_order_release);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { T = 5, PER = 200 };

static unsigned grant_log[T * PER];
static int grants;
static long counter;
static atomic_int in_cs, violations;
static int per_thread[T];
static int own_order_bad[T];

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    unsigned prev = 0;
    for (int i = 0; i < PER; i++) {
        unsigned my = ticket_lock(&tl);
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        grant_log[grants++] = my;
        long v = counter;
        if (i % 6 == 0)
            sched_yield();
        counter = v + 1;
        per_thread[id]++;
        if (i > 0 && (int)(my - prev) <= 0)
            own_order_bad[id]++;
        prev = my;
        atomic_fetch_sub(&in_cs, 1);
        ticket_unlock(&tl);
    }
    return NULL;
}

int main(void) {
    unsigned base = atomic_load(&tl.next);
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    int fifo_breaks = 0;
    for (int k = 0; k < T * PER; k++)
        if (grant_log[k] != base + (unsigned)k)
            fifo_breaks++;
    int own_bad = 0;
    for (int i = 0; i < T; i++)
        own_bad += own_order_bad[i];
    printf("threads=%d per-thread=%d counter=%ld\n", T, PER, counter);
    printf("grants in exact ticket order: %s (breaks=%d)\n", fifo_breaks == 0 ? "yes" : "no", fifo_breaks);
    printf("per-thread order regressions %d, violations %d\n", own_bad, atomic_load(&violations));
    printf("counters wrapped past zero: %s\n", atomic_load(&tl.next) < 1000u ? "yes" : "no");
    printf("next-serving gap at rest: %u\n", atomic_load(&tl.next) - atomic_load(&tl.serving));
    check(counter == T * PER, "counter");
    check(fifo_breaks == 0, "fifo");
    check(own_bad == 0 && atomic_load(&violations) == 0, "exclusion");
    check(atomic_load(&tl.next) == atomic_load(&tl.serving), "rest state");
    return 0;
}
