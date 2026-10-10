/*
 * title: Sharded rwlock with per-shard reader locks
 * topic: concurrency
 * covers: big-reader lock, sharded rwlock, writer acquires all shards in order, reader scalability pattern
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { SHARDS = 4, READERS = 6, WRITERS = 2, R_ITERS = 400, W_ITERS = 80 };

/* A reader takes only its own shard's read lock; a writer takes every shard's write lock, in order. */
typedef struct {
    pthread_rwlock_t shard[SHARDS];
} BigRW;

static BigRW big;

static void big_init(BigRW *b) {
    for (int i = 0; i < SHARDS; i++)
        pthread_rwlock_init(&b->shard[i], NULL);
}
static void big_destroy(BigRW *b) {
    for (int i = 0; i < SHARDS; i++)
        pthread_rwlock_destroy(&b->shard[i]);
}
static void big_rd_lock(BigRW *b, int reader_id) { pthread_rwlock_rdlock(&b->shard[reader_id % SHARDS]); }
static void big_rd_unlock(BigRW *b, int reader_id) { pthread_rwlock_unlock(&b->shard[reader_id % SHARDS]); }
static void big_wr_lock(BigRW *b) {
    for (int i = 0; i < SHARDS; i++)
        pthread_rwlock_wrlock(&b->shard[i]);
}
static void big_wr_unlock(BigRW *b) {
    for (int i = SHARDS - 1; i >= 0; i--)
        pthread_rwlock_unlock(&b->shard[i]);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define CELLS 6
static long cells[CELLS];
static atomic_int writers_inside, bad;
static long reads_by_shard[SHARDS];
static pthread_mutex_t stat_mu = PTHREAD_MUTEX_INITIALIZER;

static void *reader(void *p) {
    int id = (int)(intptr_t)p;
    long n = 0;
    for (int i = 0; i < R_ITERS; i++) {
        big_rd_lock(&big, id);
        if (atomic_load(&writers_inside) != 0)
            atomic_fetch_add(&bad, 1);
        long first = cells[0];
        for (int k = 1; k < CELLS; k++) {
            if (k == 3 && i % 3 == 0)
                sched_yield();
            if (cells[k] != first + k)
                atomic_fetch_add(&bad, 1);
        }
        n++;
        big_rd_unlock(&big, id);
    }
    pthread_mutex_lock(&stat_mu);
    reads_by_shard[id % SHARDS] += n;
    pthread_mutex_unlock(&stat_mu);
    return NULL;
}

static void *writer(void *p) {
    (void)p;
    for (int i = 0; i < W_ITERS; i++) {
        big_wr_lock(&big);
        if (atomic_fetch_add(&writers_inside, 1) != 0)
            atomic_fetch_add(&bad, 1);
        for (int k = 0; k < CELLS; k++) {
            cells[k]++;
            if (k == 2)
                sched_yield();
        }
        atomic_fetch_sub(&writers_inside, 1);
        big_wr_unlock(&big);
    }
    return NULL;
}

int main(void) {
    big_init(&big);
    for (int k = 0; k < CELLS; k++)
        cells[k] = k; /* invariant: cells[k] == cells[0] + k */
    pthread_t th[READERS + WRITERS];
    for (int i = 0; i < WRITERS; i++)
        pthread_create(&th[i], NULL, writer, NULL);
    for (int i = 0; i < READERS; i++)
        pthread_create(&th[WRITERS + i], NULL, reader, (void *)(intptr_t)i);
    for (int i = 0; i < READERS + WRITERS; i++)
        pthread_join(th[i], NULL);
    for (int s = 0; s < SHARDS; s++)
        printf("shard %d served %ld reads\n", s, reads_by_shard[s]);
    printf("cells:");
    for (int k = 0; k < CELLS; k++)
        printf(" %ld", cells[k]);
    printf("\ninconsistent observations %d\n", atomic_load(&bad));
    long total = 0;
    for (int s = 0; s < SHARDS; s++)
        total += reads_by_shard[s];
    check(total == READERS * R_ITERS, "read total");
    check(atomic_load(&bad) == 0, "exclusion");
    check(cells[0] == WRITERS * W_ITERS && cells[5] == cells[0] + 5, "final cells");
    /* writer lock is exclusive: a second writer attempt on shard 0 must fail while held */
    big_wr_lock(&big);
    int rc = pthread_rwlock_trywrlock(&big.shard[0]);
    int rd = pthread_rwlock_tryrdlock(&big.shard[SHARDS - 1]);
    printf("while writer holds all shards: trywrlock busy=%s tryrdlock busy=%s\n", rc != 0 ? "yes" : "no",
           rd != 0 ? "yes" : "no");
    check(rc != 0 && rd != 0, "shards held");
    big_wr_unlock(&big);
    big_destroy(&big);
    return 0;
}
