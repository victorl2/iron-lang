/*
 * title: Cyclic countdown with completion action and cycle observers
 * topic: concurrency
 * covers: cyclic countdown, auto-reset on zero, exactly-once action per cycle, observers waiting for cycle k, lost wakeup check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define EVENTS_PER_CYCLE 10
enum { CYCLES = 60, PRODUCERS = 4, OBSERVERS = 3 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int remaining;
    int cycles_done;
} Cyclic;

static Cyclic cd = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, EVENTS_PER_CYCLE, 0};

static int action_runs[CYCLES + 1];
static int action_order_bad;
static atomic_int action_total;
static long action_witness; /* written only by the action */

/* Runs once per completed cycle, on the thread that delivered the last event, without holding the lock. */
static void on_cycle(int cycle_number) {
    action_runs[cycle_number]++;
    if (atomic_fetch_add(&action_total, 1) + 1 != cycle_number)
        action_order_bad++; /* cycles complete in order, so the k-th run is cycle k */
    action_witness += cycle_number;
}

static void event(void) {
    int finished = 0;
    pthread_mutex_lock(&cd.mu);
    if (--cd.remaining == 0) {
        cd.remaining = EVENTS_PER_CYCLE; /* reset immediately: next cycle can start counting */
        finished = ++cd.cycles_done;
    }
    pthread_mutex_unlock(&cd.mu);
    if (finished) {
        /* action may run late relative to other threads; observers are released only after it ran */
        while (atomic_load(&action_total) != finished - 1)
            sched_yield();
        on_cycle(finished);
        pthread_mutex_lock(&cd.mu);
        pthread_cond_broadcast(&cd.cv);
        pthread_mutex_unlock(&cd.mu);
    }
}

static int actions_published; /* cycles whose action finished; guarded by cd.mu */
static void publish_check(void) {
    pthread_mutex_lock(&cd.mu);
    actions_published = atomic_load(&action_total);
    pthread_mutex_unlock(&cd.mu);
}

static void wait_cycle(int k) {
    pthread_mutex_lock(&cd.mu);
    while (atomic_load(&action_total) < k)
        pthread_cond_wait(&cd.cv, &cd.mu);
    pthread_mutex_unlock(&cd.mu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int obs_ok[OBSERVERS];
static void *producer(void *p) {
    int id = (int)(intptr_t)p;
    /* threads deliver different numbers of events; the totals add up to CYCLES * EVENTS_PER_CYCLE */
    int total = CYCLES * EVENTS_PER_CYCLE;
    int mine = total / PRODUCERS + (id < total % PRODUCERS ? 1 : 0);
    for (int i = 0; i < mine; i++) {
        event();
        if (i % 11 == 0)
            sched_yield();
    }
    return NULL;
}
static void *observer(void *p) {
    int id = (int)(intptr_t)p;
    int ok = 1;
    /* observer id waits for every (id+1)-th cycle and checks the action ran before it was released */
    for (int k = id + 1; k <= CYCLES; k += id + 1) {
        wait_cycle(k);
        if (action_runs[k] != 1)
            ok = 0;
    }
    obs_ok[id] = ok;
    return NULL;
}

int main(void) {
    pthread_t pt[PRODUCERS], ot[OBSERVERS];
    for (int i = 0; i < OBSERVERS; i++)
        pthread_create(&ot[i], NULL, observer, (void *)(intptr_t)i);
    for (int i = 0; i < PRODUCERS; i++)
        pthread_create(&pt[i], NULL, producer, (void *)(intptr_t)i);
    for (int i = 0; i < PRODUCERS; i++)
        pthread_join(pt[i], NULL);
    for (int i = 0; i < OBSERVERS; i++)
        pthread_join(ot[i], NULL);
    publish_check();

    int not_once = 0;
    for (int k = 1; k <= CYCLES; k++)
        if (action_runs[k] != 1)
            not_once++;
    printf("events per cycle %d, cycles %d, producers %d\n", EVENTS_PER_CYCLE, CYCLES, PRODUCERS);
    printf("cycles completed %d, actions run %d, cycles with action not exactly once %d\n", cd.cycles_done,
           atomic_load(&action_total), not_once);
    printf("action order violations %d, witness sum %ld\n", action_order_bad, action_witness);
    for (int i = 0; i < OBSERVERS; i++)
        printf("observer %d (every %d cycles) saw actions done before release: %s\n", i, i + 1,
               obs_ok[i] ? "yes" : "no");
    check(cd.cycles_done == CYCLES && atomic_load(&action_total) == CYCLES && not_once == 0, "cycles");
    check(action_order_bad == 0 && action_witness == (long)CYCLES * (CYCLES + 1) / 2, "order");
    for (int i = 0; i < OBSERVERS; i++)
        check(obs_ok[i], "observer");
    check(cd.remaining == EVENTS_PER_CYCLE && actions_published == CYCLES, "reset");
    return 0;
}
