/*
 * title: Countdown latch with threshold waiters
 * topic: concurrency
 * covers: latch, wait-until-at-most-k, stepwise release order, underflow detection, lost wakeup check
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
    int count;
    int underflows;
    int waiting;
} Latch;

static void latch_init(Latch *l, int n) {
    pthread_mutex_init(&l->mu, NULL);
    pthread_cond_init(&l->cv, NULL);
    l->count = n;
    l->underflows = 0;
    l->waiting = 0;
}
static void latch_destroy(Latch *l) {
    pthread_mutex_destroy(&l->mu);
    pthread_cond_destroy(&l->cv);
}
static void latch_count_down(Latch *l) {
    pthread_mutex_lock(&l->mu);
    if (l->count == 0)
        l->underflows++;
    else {
        l->count--;
        pthread_cond_broadcast(&l->cv); /* waiters have different thresholds: wake them all to re-check */
    }
    pthread_mutex_unlock(&l->mu);
}
/* Blocks until count <= k; returns the count observed on wakeup. */
static int latch_wait_at_most(Latch *l, int k) {
    pthread_mutex_lock(&l->mu);
    l->waiting++;
    while (l->count > k)
        pthread_cond_wait(&l->cv, &l->mu);
    l->waiting--;
    int seen = l->count;
    pthread_mutex_unlock(&l->mu);
    return seen;
}
static int latch_try_wait(Latch *l) {
    pthread_mutex_lock(&l->mu);
    int z = l->count == 0;
    pthread_mutex_unlock(&l->mu);
    return z;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { START = 6, NWAIT = START + 1 };

static Latch step_latch;
static atomic_int released;
static int woke_count[NWAIT];
static int released_seq[NWAIT];
static atomic_int release_pos;

static void *threshold_waiter(void *p) {
    int thr = (int)(intptr_t)p;
    int seen = latch_wait_at_most(&step_latch, thr);
    woke_count[thr] = seen;
    released_seq[atomic_fetch_add(&release_pos, 1)] = thr;
    atomic_fetch_add(&released, 1);
    return NULL;
}

static void wait_until_waiting(Latch *l, int n) {
    for (;;) {
        pthread_mutex_lock(&l->mu);
        int w = l->waiting;
        pthread_mutex_unlock(&l->mu);
        if (w == n)
            return;
        sched_yield();
    }
}

enum { CT = 6, PER = 70, MAINS = 3 };
static Latch big;
static atomic_int big_released;

static void *counter_thread(void *p) {
    (void)p;
    for (int i = 0; i < PER; i++)
        latch_count_down(&big);
    return NULL;
}
static void *zero_waiter(void *p) {
    (void)p;
    int seen = latch_wait_at_most(&big, 0);
    if (seen == 0)
        atomic_fetch_add(&big_released, 1);
    return NULL;
}

int main(void) {
    /* Part 1: stepwise. One waiter per threshold 0..START, main counts down one at a time. */
    latch_init(&step_latch, START);
    pthread_t th[NWAIT];
    /* thresholds 0..START-1 block; threshold START is already satisfied */
    for (int thr = 0; thr < NWAIT; thr++)
        pthread_create(&th[thr], NULL, threshold_waiter, (void *)(intptr_t)thr);
    wait_until_waiting(&step_latch, START); /* thresholds 0..START-1 are blocked */
    while (atomic_load(&released) < 1) /* threshold START is already satisfied and returns at once */
        sched_yield();
    for (int step = 1; step <= START; step++) {
        latch_count_down(&step_latch);
        int expect = 1 + step; /* thresholds >= START - step are satisfied */
        while (atomic_load(&released) != expect)
            sched_yield();
        printf("after %d count-downs: %d waiters released\n", step, expect);
    }
    for (int i = 0; i < NWAIT; i++)
        pthread_join(th[i], NULL);
    int ok = 1;
    for (int thr = 0; thr < NWAIT; thr++) {
        if (woke_count[thr] > thr)
            ok = 0;
    }
    /* release sequence must be non-increasing in threshold: higher thresholds fire first */
    for (int i = 1; i < NWAIT; i++)
        if (released_seq[i] > released_seq[i - 1])
            ok = 0;
    printf("threshold release order non-increasing: %s\n", ok ? "yes" : "no");
    check(ok, "threshold order");
    check(latch_try_wait(&step_latch), "step latch at zero");
    latch_count_down(&step_latch);
    printf("extra count-down underflows recorded: %d\n", step_latch.underflows);
    check(step_latch.underflows == 1 && step_latch.count == 0, "underflow guard");
    latch_destroy(&step_latch);

    /* Part 2: many concurrent counters, several zero waiters (lost wakeup check). */
    latch_init(&big, CT * PER);
    pthread_t cts[CT], zs[MAINS];
    for (int i = 0; i < MAINS; i++)
        pthread_create(&zs[i], NULL, zero_waiter, NULL);
    for (int i = 0; i < CT; i++)
        pthread_create(&cts[i], NULL, counter_thread, NULL);
    for (int i = 0; i < CT; i++)
        pthread_join(cts[i], NULL);
    for (int i = 0; i < MAINS; i++)
        pthread_join(zs[i], NULL);
    printf("concurrent: count=%d zero waiters released=%d underflows=%d\n", big.count,
           atomic_load(&big_released), big.underflows);
    check(big.count == 0 && atomic_load(&big_released) == MAINS && big.underflows == 0, "concurrent latch");
    latch_destroy(&big);
    return 0;
}
