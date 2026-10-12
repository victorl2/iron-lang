/*
 * title: One-shot event with payload publication and relay chain
 * topic: concurrency
 * covers: manual-set one-shot event, idempotent set, late waiters, payload visibility, event chain relay
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int is_set;
    int waiters;
    int set_calls;
} Event;

static void ev_init(Event *e) {
    pthread_mutex_init(&e->mu, NULL);
    pthread_cond_init(&e->cv, NULL);
    e->is_set = e->waiters = e->set_calls = 0;
}
static void ev_destroy(Event *e) {
    pthread_mutex_destroy(&e->mu);
    pthread_cond_destroy(&e->cv);
}
/* Returns 1 for the call that flipped the event, 0 for redundant sets. */
static int ev_set(Event *e) {
    pthread_mutex_lock(&e->mu);
    e->set_calls++;
    int first = !e->is_set;
    e->is_set = 1;
    if (first)
        pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->mu);
    return first;
}
static void ev_wait(Event *e) {
    pthread_mutex_lock(&e->mu);
    e->waiters++;
    while (!e->is_set)
        pthread_cond_wait(&e->cv, &e->mu);
    e->waiters--;
    pthread_mutex_unlock(&e->mu);
}
static int ev_is_set(Event *e) {
    pthread_mutex_lock(&e->mu);
    int s = e->is_set;
    pthread_mutex_unlock(&e->mu);
    return s;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { WAITERS = 6, CHAIN = 12 };

static Event gate;
static int payload[16];
static int payload_seen_ok[WAITERS];

static void *gate_waiter(void *p) {
    int id = (int)(intptr_t)p;
    ev_wait(&gate);
    int ok = 1;
    for (int i = 0; i < 16; i++)
        if (payload[i] != i * i + 1)
            ok = 0;
    payload_seen_ok[id] = ok;
    return NULL;
}

static Event link_ev[CHAIN + 1];
static long relay_value[CHAIN + 1];
static int relay_order[CHAIN];
static atomic_int relay_pos;

static void *relay(void *p) {
    int i = (int)(intptr_t)p;
    ev_wait(&link_ev[i]);
    relay_value[i + 1] = relay_value[i] * 3 + i;
    relay_order[atomic_fetch_add(&relay_pos, 1)] = i;
    ev_set(&link_ev[i + 1]);
    return NULL;
}

int main(void) {
    ev_init(&gate);
    pthread_t th[WAITERS];
    for (int i = 0; i < WAITERS; i++)
        pthread_create(&th[i], NULL, gate_waiter, (void *)(intptr_t)i);
    for (;;) { /* all waiters blocked before the event fires */
        pthread_mutex_lock(&gate.mu);
        int w = gate.waiters;
        pthread_mutex_unlock(&gate.mu);
        if (w == WAITERS)
            break;
        sched_yield();
    }
    check(!ev_is_set(&gate), "not set yet");
    for (int i = 0; i < 16; i++)
        payload[i] = i * i + 1; /* published before set */
    int r1 = ev_set(&gate);
    int r2 = ev_set(&gate);
    int r3 = ev_set(&gate);
    for (int i = 0; i < WAITERS; i++)
        pthread_join(th[i], NULL);
    int seen = 0;
    for (int i = 0; i < WAITERS; i++)
        seen += payload_seen_ok[i];
    printf("gate: set() results %d %d %d, waiters that saw the payload %d of %d\n", r1, r2, r3, seen, WAITERS);
    check(r1 == 1 && r2 == 0 && r3 == 0 && seen == WAITERS, "gate");
    ev_wait(&gate); /* late waiter returns immediately */
    printf("late waiter passed straight through: yes, total set calls %d\n", gate.set_calls);
    check(gate.waiters == 0 && gate.set_calls == 3, "gate state");
    ev_destroy(&gate);

    /* Relay: thread i waits for link i, computes, and sets link i+1. Started in reverse order. */
    for (int i = 0; i <= CHAIN; i++)
        ev_init(&link_ev[i]);
    relay_value[0] = 1;
    pthread_t rt[CHAIN];
    for (int i = CHAIN - 1; i >= 0; i--)
        pthread_create(&rt[i], NULL, relay, (void *)(intptr_t)i);
    ev_set(&link_ev[0]);
    for (int i = 0; i < CHAIN; i++)
        pthread_join(rt[i], NULL);
    ev_wait(&link_ev[CHAIN]);
    long expect = 1;
    for (int i = 0; i < CHAIN; i++)
        expect = expect * 3 + i;
    int in_order = 1;
    for (int i = 0; i < CHAIN; i++)
        if (relay_order[i] != i)
            in_order = 0;
    printf("relay: final value %ld (expected %ld), completed in chain order: %s\n", relay_value[CHAIN], expect,
           in_order ? "yes" : "no");
    check(relay_value[CHAIN] == expect && in_order, "relay");
    for (int i = 0; i <= CHAIN; i++)
        ev_destroy(&link_ev[i]);
    return 0;
}
