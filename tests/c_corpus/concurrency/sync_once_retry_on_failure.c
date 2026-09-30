/*
 * title: Once-initialization with failure and retry
 * topic: concurrency
 * covers: call-once from atomics and condvar, UNINIT/RUNNING/DONE states, failed init resets state, waiters retry, fast path
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { UNINIT = 0, RUNNING = 1, DONE = 2 };

typedef struct {
    atomic_int state;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} Once;

typedef int (*InitFn)(void *);

/* Returns 0 once initialized. If THIS caller's init attempt fails, returns its nonzero code and the
 * state goes back to UNINIT so another caller can try. Callers that only waited never see the failure. */
static int once_call(Once *o, InitFn fn, void *arg) {
    if (atomic_load_explicit(&o->state, memory_order_acquire) == DONE)
        return 0; /* fast path */
    pthread_mutex_lock(&o->mu);
    for (;;) {
        int st = atomic_load_explicit(&o->state, memory_order_relaxed);
        if (st == DONE) {
            pthread_mutex_unlock(&o->mu);
            return 0;
        }
        if (st == UNINIT) {
            atomic_store_explicit(&o->state, RUNNING, memory_order_relaxed);
            pthread_mutex_unlock(&o->mu);
            int rc = fn(arg);
            pthread_mutex_lock(&o->mu);
            atomic_store_explicit(&o->state, rc == 0 ? DONE : UNINIT, memory_order_release);
            pthread_cond_broadcast(&o->cv);
            pthread_mutex_unlock(&o->mu);
            return rc;
        }
        pthread_cond_wait(&o->cv, &o->mu); /* somebody else is running init */
    }
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { NRES = 4, T = 6, FAILS_BASE = 2 };

typedef struct {
    int id;
    int fail_first;      /* the first N attempts fail */
    atomic_int attempts; /* every call of init */
    atomic_int running;  /* concurrent runs of init: must never exceed 1 */
    atomic_int overlap;
    int table[32];       /* the resource being built */
} Resource;

static Resource res[NRES];
static Once once[NRES];

static int init_resource(void *arg) {
    Resource *r = arg;
    if (atomic_fetch_add(&r->running, 1) != 0)
        atomic_fetch_add(&r->overlap, 1);
    int attempt = atomic_fetch_add(&r->attempts, 1);
    for (int i = 0; i < 32; i++) {
        r->table[i] = r->id * 100 + i;
        if (i == 16)
            sched_yield();
    }
    atomic_fetch_sub(&r->running, 1);
    return attempt < r->fail_first ? 100 + attempt : 0;
}

static int failures_seen[T];
static int bad_visibility[T];

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    for (int round = 0; round < NRES; round++) {
        int k = (round + id) % NRES;
        for (;;) {
            int rc = once_call(&once[k], init_resource, &res[k]);
            if (rc == 0)
                break;
            failures_seen[id]++; /* our own attempt failed: try again */
        }
        for (int i = 0; i < 32; i++)
            if (res[k].table[i] != k * 100 + i)
                bad_visibility[id]++;
    }
    return NULL;
}

int main(void) {
    for (int k = 0; k < NRES; k++) {
        res[k].id = k;
        res[k].fail_first = FAILS_BASE * k; /* 0, 2, 4, 6 failures before success */
        atomic_store(&once[k].state, UNINIT);
        pthread_mutex_init(&once[k].mu, NULL);
        pthread_cond_init(&once[k].cv, NULL);
    }
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    int total_fail_seen = 0, bad = 0, expect_fail = 0;
    for (int i = 0; i < T; i++) {
        total_fail_seen += failures_seen[i];
        bad += bad_visibility[i];
    }
    for (int k = 0; k < NRES; k++) {
        int att = atomic_load(&res[k].attempts);
        printf("resource %d: init attempts %d (failures scripted %d), overlapping runs %d, state %s\n", k, att,
               res[k].fail_first, atomic_load(&res[k].overlap),
               atomic_load(&once[k].state) == DONE ? "DONE" : "not done");
        check(att == res[k].fail_first + 1, "attempt count");
        check(atomic_load(&res[k].overlap) == 0, "init overlap");
        check(atomic_load(&once[k].state) == DONE, "done");
        expect_fail += res[k].fail_first;
    }
    printf("failures reported to callers %d (expected %d), invisible initializations %d\n", total_fail_seen,
           expect_fail, bad);
    check(total_fail_seen == expect_fail && bad == 0, "failure accounting");
    /* a finished once never calls init again */
    int before = atomic_load(&res[3].attempts);
    int rc = once_call(&once[3], init_resource, &res[3]);
    printf("call after completion: rc=%d, extra attempts %d\n", rc, atomic_load(&res[3].attempts) - before);
    check(rc == 0 && atomic_load(&res[3].attempts) == before, "fast path");
    for (int k = 0; k < NRES; k++) {
        pthread_mutex_destroy(&once[k].mu);
        pthread_cond_destroy(&once[k].cv);
    }
    return 0;
}
