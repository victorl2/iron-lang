/*
 * title: Auto-reset event with coalescing and handshaked pulses
 * topic: concurrency
 * covers: auto-reset event, exactly-one wakeup per set, coalesced sets, try-wait, lost wakeup check
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
    int signaled;
    int waiters;
} AutoEvent;

static AutoEvent ev = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};

/* A set with a waiter present releases exactly one waiter; with none it stays latched (not counted twice). */
static void ae_set(void) {
    pthread_mutex_lock(&ev.mu);
    ev.signaled = 1;
    pthread_cond_signal(&ev.cv);
    pthread_mutex_unlock(&ev.mu);
}
static void ae_wait(void) {
    pthread_mutex_lock(&ev.mu);
    ev.waiters++;
    while (!ev.signaled)
        pthread_cond_wait(&ev.cv, &ev.mu);
    ev.signaled = 0; /* consume the signal: auto reset */
    ev.waiters--;
    pthread_mutex_unlock(&ev.mu);
}
static int ae_try_wait(void) {
    pthread_mutex_lock(&ev.mu);
    int got = ev.signaled;
    ev.signaled = 0;
    pthread_mutex_unlock(&ev.mu);
    return got;
}
static int ae_waiters(void) {
    pthread_mutex_lock(&ev.mu);
    int w = ev.waiters;
    pthread_mutex_unlock(&ev.mu);
    return w;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { W = 4, PULSES = 400 };

static atomic_int wakes;
static int per_waiter[W];
static atomic_int quit;

static void *waiter(void *p) {
    int id = (int)(intptr_t)p;
    for (;;) {
        ae_wait();
        if (atomic_load(&quit))
            return NULL;
        per_waiter[id]++;
        atomic_fetch_add(&wakes, 1);
    }
}

static void wait_wakes(int n) {
    while (atomic_load(&wakes) != n)
        sched_yield();
}

int main(void) {
    /* Latched set with nobody waiting: consumed exactly once. */
    ae_set();
    ae_set();
    ae_set();
    int a = ae_try_wait();
    int b = ae_try_wait();
    printf("three sets before any wait: try_wait -> %d then %d\n", a, b);
    check(a == 1 && b == 0, "coalescing");

    pthread_t th[W];
    for (int i = 0; i < W; i++)
        pthread_create(&th[i], NULL, waiter, (void *)(intptr_t)i);
    while (ae_waiters() != W)
        sched_yield();

    /* Each set must wake exactly one of the W blocked waiters. */
    for (int k = 1; k <= 3; k++) {
        ae_set();
        wait_wakes(k);
        while (ae_waiters() != W) /* the woken thread loops back and blocks again */
            sched_yield();
        printf("set #%d: total wakeups %d\n", k, atomic_load(&wakes));
        check(atomic_load(&wakes) == k, "exactly one wake per set");
    }

    /* Handshaked stress: many pulses, none may be lost or duplicated. */
    for (int k = 4; k <= PULSES; k++) {
        ae_set();
        wait_wakes(k);
        while (ae_waiters() != W)
            sched_yield();
    }
    int total = 0;
    for (int i = 0; i < W; i++)
        total += per_waiter[i];
    printf("pulses sent %d, wakeups %d, sum over waiters %d\n", PULSES, atomic_load(&wakes), total);
    check(atomic_load(&wakes) == PULSES && total == PULSES, "pulse accounting");

    /* Shutdown: raise quit and release every waiter with one set per waiter. */
    atomic_store(&quit, 1);
    for (int i = 0; i < W; i++) {
        ae_set();
        while (ae_waiters() > W - 1 - i)
            sched_yield();
    }
    for (int i = 0; i < W; i++)
        pthread_join(th[i], NULL);
    printf("all %d waiters shut down, signal state %d\n", W, ev.signaled);
    check(ev.signaled == 0 && ev.waiters == 0, "final state");
    return 0;
}
