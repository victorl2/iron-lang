/*
 * title: Phase-fair rwlock alternating reader and writer phases
 * topic: concurrency
 * covers: phase-fair rwlock, reader batch admission, alternation between phases, admission order log
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Readers arriving while a writer is active or waiting join a batch that is admitted as a
 * whole when the writer leaves. Writers therefore never starve, and neither do readers.
 */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int readers_active, writer_active, writers_waiting, readers_waiting;
    unsigned read_epoch;
} RW;

static RW rw = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0};

static void rd_lock(void) {
    pthread_mutex_lock(&rw.mu);
    if (!rw.writer_active && rw.writers_waiting == 0) {
        rw.readers_active++;
    } else {
        unsigned my = rw.read_epoch;
        rw.readers_waiting++;
        while (rw.read_epoch == my)
            pthread_cond_wait(&rw.cv, &rw.mu);
        /* counted as active by the writer that admitted the batch */
    }
    pthread_mutex_unlock(&rw.mu);
}
static void rd_unlock(void) {
    pthread_mutex_lock(&rw.mu);
    if (--rw.readers_active == 0)
        pthread_cond_broadcast(&rw.cv);
    pthread_mutex_unlock(&rw.mu);
}
static void wr_lock(void) {
    pthread_mutex_lock(&rw.mu);
    rw.writers_waiting++;
    while (rw.writer_active || rw.readers_active > 0)
        pthread_cond_wait(&rw.cv, &rw.mu);
    rw.writers_waiting--;
    rw.writer_active = 1;
    pthread_mutex_unlock(&rw.mu);
}
static void wr_unlock(void) {
    pthread_mutex_lock(&rw.mu);
    rw.writer_active = 0;
    if (rw.readers_waiting > 0) { /* hand the lock to the whole waiting reader batch */
        rw.readers_active += rw.readers_waiting;
        rw.readers_waiting = 0;
        rw.read_epoch++;
    }
    pthread_cond_broadcast(&rw.cv);
    pthread_mutex_unlock(&rw.mu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { READERS = 4, WRITERS = 3, R_ITERS = 250, W_ITERS = 100 };

static int lo, hi;
static atomic_int in_r, in_w, bad;
static atomic_int max_readers;
static long reads_ok;
static pthread_mutex_t stat_mu = PTHREAD_MUTEX_INITIALIZER;

static void *reader(void *p) {
    (void)p;
    long ok = 0;
    for (int i = 0; i < R_ITERS; i++) {
        rd_lock();
        int now = atomic_fetch_add(&in_r, 1) + 1;
        if (atomic_load(&in_w) != 0)
            atomic_fetch_add(&bad, 1);
        int m = atomic_load(&max_readers);
        while (now > m && !atomic_compare_exchange_weak(&max_readers, &m, now))
            ;
        int a = lo;
        if (i % 4 == 0)
            sched_yield();
        if (a == hi)
            ok++;
        else
            atomic_fetch_add(&bad, 1);
        atomic_fetch_sub(&in_r, 1);
        rd_unlock();
    }
    pthread_mutex_lock(&stat_mu);
    reads_ok += ok;
    pthread_mutex_unlock(&stat_mu);
    return NULL;
}
static void *writer(void *p) {
    (void)p;
    for (int i = 0; i < W_ITERS; i++) {
        wr_lock();
        if (atomic_fetch_add(&in_w, 1) != 0 || atomic_load(&in_r) != 0)
            atomic_fetch_add(&bad, 1);
        lo++;
        sched_yield();
        hi++;
        atomic_fetch_sub(&in_w, 1);
        wr_unlock();
    }
    return NULL;
}

static char order[16];
static atomic_int order_len;
static void *log_reader(void *p) {
    (void)p;
    rd_lock();
    order[atomic_fetch_add(&order_len, 1)] = 'R';
    rd_unlock();
    return NULL;
}
static void *log_writer(void *p) {
    (void)p;
    wr_lock();
    order[atomic_fetch_add(&order_len, 1)] = 'W';
    wr_unlock();
    return NULL;
}
static void wait_queue(int r, int w) {
    for (;;) {
        pthread_mutex_lock(&rw.mu);
        int ok = rw.readers_waiting == r && rw.writers_waiting == w;
        pthread_mutex_unlock(&rw.mu);
        if (ok)
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
    printf("stress: writes=%d/%d reads consistent=%ld of %d\n", lo, hi, reads_ok, READERS * R_ITERS);
    printf("violations %d, reader concurrency bounded: %s\n", atomic_load(&bad),
           atomic_load(&max_readers) <= READERS ? "yes" : "no");
    check(atomic_load(&bad) == 0, "violations");
    check(lo == WRITERS * W_ITERS && hi == lo, "write count");
    check(reads_ok == READERS * R_ITERS, "reads");

    /* Scenario 1: writer active, 2 readers and 1 writer queue up. Phase fairness: RR then W. */
    wr_lock();
    pthread_t t[4];
    pthread_create(&t[0], NULL, log_reader, NULL);
    pthread_create(&t[1], NULL, log_reader, NULL);
    wait_queue(2, 0);
    pthread_create(&t[2], NULL, log_writer, NULL);
    wait_queue(2, 1);
    wr_unlock();
    for (int i = 0; i < 3; i++)
        pthread_join(t[i], NULL);
    order[atomic_load(&order_len)] = 0;
    printf("scenario 1 order: %s\n", order);
    check(order[0] == 'R' && order[1] == 'R' && order[2] == 'W' && order_len == 3, "reader batch first");

    /* Scenario 2: reader active, writer queues, then a new reader queues behind that writer. Order W then R. */
    order_len = 0;
    rd_lock();
    pthread_create(&t[0], NULL, log_writer, NULL);
    wait_queue(0, 1);
    pthread_create(&t[1], NULL, log_reader, NULL);
    wait_queue(1, 1);
    rd_unlock();
    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);
    order[atomic_load(&order_len)] = 0;
    printf("scenario 2 order: %s\n", order);
    check(order[0] == 'W' && order[1] == 'R' && order_len == 2, "writer then reader");
    check(rw.readers_active == 0 && !rw.writer_active && rw.readers_waiting == 0, "idle");
    printf("read epochs advanced at least twice: %s\n", rw.read_epoch >= 2 ? "yes" : "no");
    return 0;
}
