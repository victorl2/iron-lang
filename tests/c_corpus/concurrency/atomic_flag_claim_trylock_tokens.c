/*
 * title: atomic_flag as claim token and try-lock with deferred flush
 * topic: concurrency
 * covers: atomic_flag test_and_set, claim-once jobs, try-lock, acquire/release, pending-work flush
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Correctness argument:
 * Claim tokens: test_and_set is an atomic RMW, so for each job exactly one caller observes the
 * old value "clear". That caller owns the job; others skip it. Every job runs exactly once.
 * Try-lock: the winner of test_and_set(acquire) owns the accumulator until clear(release).
 * Threads that lose keep their contribution in a private pending sum and retry later, so no
 * contribution is lost, and the final flush after join is deterministic.
 */
enum { JOBS = 500, T = 5, ITEMS = 4000 };

static atomic_flag claimed[JOBS];
static atomic_int runs[JOBS];
static atomic_long job_total;

static atomic_flag guard = ATOMIC_FLAG_INIT;
static long accumulator;       /* protected by guard */
static long acc_updates;       /* protected by guard */
static atomic_long deferred_total;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long job_value(int j) {
    return (long)((unsigned)j * 2654435761u % 1000u) + 1;
}

static int try_lock(void) {
    return !atomic_flag_test_and_set_explicit(&guard, memory_order_acquire);
}
static void unlock(void) {
    atomic_flag_clear_explicit(&guard, memory_order_release);
}

static void *worker(void *p) {
    int id = (int)(size_t)p;
    /* scan jobs starting at different offsets so threads collide on claims */
    for (int k = 0; k < JOBS; k++) {
        int j = (k + id * 97) % JOBS;
        if (!atomic_flag_test_and_set_explicit(&claimed[j], memory_order_acq_rel)) {
            atomic_fetch_add(&runs[j], 1);
            atomic_fetch_add(&job_total, job_value(j));
        }
    }
    long pending = 0;
    for (int i = 0; i < ITEMS; i++) {
        pending += (long)((i * (id + 3)) % 11);
        if (i % 4 == 3) {
            if (try_lock()) {
                accumulator += pending;
                acc_updates++;
                pending = 0;
                unlock();
            } else {
                atomic_fetch_add_explicit(&deferred_total, 1, memory_order_relaxed);
            }
        }
    }
    while (pending != 0) { /* final flush must eventually get the lock */
        if (try_lock()) {
            accumulator += pending;
            acc_updates++;
            pending = 0;
            unlock();
        } else {
            sched_yield();
        }
    }
    return NULL;
}

int main(void) {
    for (int j = 0; j < JOBS; j++)
        atomic_flag_clear(&claimed[j]);
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    long want_jobs = 0;
    for (int j = 0; j < JOBS; j++) {
        check(atomic_load(&runs[j]) == 1, "job ran exactly once");
        want_jobs += job_value(j);
    }
    check(atomic_load(&job_total) == want_jobs, "job total");
    long want_acc = 0;
    for (int id = 0; id < T; id++)
        for (int i = 0; i < ITEMS; i++)
            want_acc += (long)((i * (id + 3)) % 11);
    check(accumulator == want_acc, "accumulator");
    check(acc_updates >= T, "at least one flush per thread");
    printf("jobs=%d each ran once, job_total=%ld\n", JOBS, want_jobs);
    printf("accumulator=%ld from %d items per thread\n", accumulator, ITEMS);
    printf("guard released: %s\n", try_lock() ? "yes" : "no");
    unlock();
    return 0;
}
