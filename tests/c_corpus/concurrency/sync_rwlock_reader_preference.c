/*
 * title: Reader-preference rwlock and writer starvation demo
 * topic: concurrency
 * covers: rwlock from mutex and condvar, reader preference, writer starvation, paired-field invariant, try-read
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int readers;
    int writer;
    int writers_waiting;
} RW;

static RW rw = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0};

/* Reader preference: readers only defer to an ACTIVE writer, never to a waiting one. */
static void rd_lock(void) {
    pthread_mutex_lock(&rw.mu);
    while (rw.writer)
        pthread_cond_wait(&rw.cv, &rw.mu);
    rw.readers++;
    pthread_mutex_unlock(&rw.mu);
}
static int rd_trylock(void) {
    int ok = 0;
    pthread_mutex_lock(&rw.mu);
    if (!rw.writer) {
        rw.readers++;
        ok = 1;
    }
    pthread_mutex_unlock(&rw.mu);
    return ok;
}
static void rd_unlock(void) {
    pthread_mutex_lock(&rw.mu);
    if (--rw.readers == 0)
        pthread_cond_broadcast(&rw.cv);
    pthread_mutex_unlock(&rw.mu);
}
static void wr_lock(void) {
    pthread_mutex_lock(&rw.mu);
    rw.writers_waiting++;
    while (rw.writer || rw.readers > 0)
        pthread_cond_wait(&rw.cv, &rw.mu);
    rw.writers_waiting--;
    rw.writer = 1;
    pthread_mutex_unlock(&rw.mu);
}
static void wr_unlock(void) {
    pthread_mutex_lock(&rw.mu);
    rw.writer = 0;
    pthread_cond_broadcast(&rw.cv);
    pthread_mutex_unlock(&rw.mu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { READERS = 4, WRITERS = 2, R_ITERS = 300, W_ITERS = 100 };

static int pair_a, pair_b;
static atomic_int active_readers, active_writers, exclusion_violations, torn;
static long writes_done, reads_done;

static void *reader(void *p) {
    (void)p;
    for (int i = 0; i < R_ITERS; i++) {
        rd_lock();
        atomic_fetch_add(&active_readers, 1);
        if (atomic_load(&active_writers) != 0)
            atomic_fetch_add(&exclusion_violations, 1);
        int a = pair_a;
        if (i % 3 == 0)
            sched_yield();
        int b = pair_b;
        if (a != b)
            atomic_fetch_add(&torn, 1);
        atomic_fetch_sub(&active_readers, 1);
        rd_unlock();
    }
    return NULL;
}

static void *writer(void *p) {
    (void)p;
    for (int i = 0; i < W_ITERS; i++) {
        wr_lock();
        if (atomic_fetch_add(&active_writers, 1) != 0 || atomic_load(&active_readers) != 0)
            atomic_fetch_add(&exclusion_violations, 1);
        pair_a++;
        sched_yield();
        pair_b++;
        writes_done++;
        atomic_fetch_sub(&active_writers, 1);
        wr_unlock();
    }
    return NULL;
}

static void *blocked_writer(void *p) {
    int *done = p;
    wr_lock();
    pair_a++;
    pair_b++;
    *done = 1;
    wr_unlock();
    return NULL;
}

static void wait_writers_waiting(int n) {
    for (;;) {
        pthread_mutex_lock(&rw.mu);
        int w = rw.writers_waiting;
        pthread_mutex_unlock(&rw.mu);
        if (w == n)
            return;
        sched_yield();
    }
}

int main(void) {
    pthread_t th[READERS + WRITERS];
    for (int i = 0; i < WRITERS; i++)
        pthread_create(&th[i], NULL, writer, NULL);
    for (int i = 0; i < READERS; i++)
        pthread_create(&th[WRITERS + i], NULL, reader, NULL);
    for (int i = 0; i < READERS + WRITERS; i++)
        pthread_join(th[i], NULL);
    reads_done = (long)READERS * R_ITERS;
    printf("stress: %ld reads, %ld writes, final pair (%d,%d)\n", reads_done, writes_done, pair_a, pair_b);
    printf("torn reads %d, exclusion violations %d\n", atomic_load(&torn), atomic_load(&exclusion_violations));
    check(atomic_load(&torn) == 0 && atomic_load(&exclusion_violations) == 0, "exclusion");
    check(pair_a == WRITERS * W_ITERS && pair_b == pair_a, "pair total");

    /* Policy demo: a reader holds the lock, a writer queues behind it. */
    int done = 0;
    rd_lock();
    pthread_t w;
    pthread_create(&w, NULL, blocked_writer, &done);
    wait_writers_waiting(1);
    int newcomer = rd_trylock();
    printf("policy: new reader admitted while writer waits: %s\n", newcomer ? "yes" : "no");
    check(newcomer == 1, "reader preference");
    printf("policy: writer done before readers leave: %s\n", done ? "yes" : "no");
    check(done == 0, "writer must still wait");
    rd_unlock();
    rd_unlock();
    pthread_join(w, NULL);
    printf("policy: writer done after readers leave: %s\n", done ? "yes" : "no");
    check(done == 1, "writer finished");
    check(rw.readers == 0 && rw.writer == 0 && rw.writers_waiting == 0, "idle state");
    return 0;
}
