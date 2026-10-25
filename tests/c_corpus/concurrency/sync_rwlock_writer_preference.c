/*
 * title: Writer-preference rwlock with reader queue order demo
 * topic: concurrency
 * covers: rwlock from mutex and condvar, writer preference, reader starvation avoidance, admission order log
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
    int readers, writer, writers_waiting, readers_waiting;
} RW;

static RW rw = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0};

/* Writer preference: arriving readers queue whenever any writer is active OR waiting. */
static void rd_lock(void) {
    pthread_mutex_lock(&rw.mu);
    rw.readers_waiting++;
    while (rw.writer || rw.writers_waiting > 0)
        pthread_cond_wait(&rw.cv, &rw.mu);
    rw.readers_waiting--;
    rw.readers++;
    pthread_mutex_unlock(&rw.mu);
}
static int rd_trylock(void) {
    int ok = 0;
    pthread_mutex_lock(&rw.mu);
    if (!rw.writer && rw.writers_waiting == 0) {
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

enum { READERS = 4, WRITERS = 3, R_ITERS = 250, W_ITERS = 120 };

static long table[8];
static atomic_int active_readers, active_writers, bad, reader_progress;

static void *reader(void *p) {
    (void)p;
    for (int i = 0; i < R_ITERS; i++) {
        rd_lock();
        atomic_fetch_add(&active_readers, 1);
        if (atomic_load(&active_writers) != 0)
            atomic_fetch_add(&bad, 1);
        long first = table[0];
        for (int k = 1; k < 8; k++) {
            if (k == 4)
                sched_yield();
            if (table[k] != first)
                atomic_fetch_add(&bad, 1);
        }
        atomic_fetch_sub(&active_readers, 1);
        atomic_fetch_add(&reader_progress, 1);
        rd_unlock();
    }
    return NULL;
}

static void *writer(void *p) {
    (void)p;
    for (int i = 0; i < W_ITERS; i++) {
        wr_lock();
        if (atomic_fetch_add(&active_writers, 1) != 0 || atomic_load(&active_readers) != 0)
            atomic_fetch_add(&bad, 1);
        for (int k = 0; k < 8; k++) {
            table[k]++;
            if (k == 3)
                sched_yield();
        }
        atomic_fetch_sub(&active_writers, 1);
        wr_unlock();
    }
    return NULL;
}

static char order[8];
static atomic_int order_len;

static void *ordered_reader(void *p) {
    (void)p;
    rd_lock();
    order[atomic_fetch_add(&order_len, 1)] = 'R';
    rd_unlock();
    return NULL;
}
static void *ordered_writer(void *p) {
    (void)p;
    wr_lock();
    order[atomic_fetch_add(&order_len, 1)] = 'W';
    wr_unlock();
    return NULL;
}

static void wait_queue(int readers_q, int writers_q) {
    for (;;) {
        pthread_mutex_lock(&rw.mu);
        int ok = rw.readers_waiting == readers_q && rw.writers_waiting == writers_q;
        pthread_mutex_unlock(&rw.mu);
        if (ok)
            return;
        sched_yield();
    }
}

int main(void) {
    pthread_t th[READERS + WRITERS];
    for (int i = 0; i < READERS; i++)
        pthread_create(&th[i], NULL, reader, NULL);
    for (int i = 0; i < WRITERS; i++)
        pthread_create(&th[READERS + i], NULL, writer, NULL);
    for (int i = 0; i < READERS + WRITERS; i++)
        pthread_join(th[i], NULL);
    printf("stress: table[0]=%ld table[7]=%ld reads=%d\n", table[0], table[7], atomic_load(&reader_progress));
    printf("inconsistent observations %d\n", atomic_load(&bad));
    check(atomic_load(&bad) == 0, "exclusion or torn read");
    check(table[0] == WRITERS * W_ITERS && table[7] == table[0], "table totals");
    check(atomic_load(&reader_progress) == READERS * R_ITERS, "readers made progress");

    /* Demo A: with a writer waiting behind a reader, a new reader must not be admitted. */
    rd_lock();
    pthread_t w1, r1;
    pthread_create(&w1, NULL, ordered_writer, NULL);
    wait_queue(0, 1);
    int newcomer = rd_trylock();
    printf("demo A: new reader admitted while writer waits: %s\n", newcomer ? "yes" : "no");
    check(newcomer == 0, "writer preference");
    pthread_create(&r1, NULL, ordered_reader, NULL);
    wait_queue(1, 1);
    rd_unlock();
    pthread_join(w1, NULL);
    pthread_join(r1, NULL);
    /* Demo B: writer must be admitted before the queued reader. */
    order[atomic_load(&order_len)] = 0;
    printf("demo B: admission order %s\n", order);
    check(order[0] == 'W' && order[1] == 'R' && order_len == 2, "writer before queued reader");
    check(rw.readers == 0 && rw.writer == 0 && rw.readers_waiting == 0, "idle state");
    return 0;
}
