/*
 * title: Counting semaphore built from mutex and condvar
 * topic: concurrency
 * covers: semaphore construction, bounded concurrency, high-water mark, wait/post
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int count;
} Sem;

static void sem_open_(Sem *s, int n) {
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->count = n;
}

static void sem_wait_(Sem *s) {
    pthread_mutex_lock(&s->mu);
    while (s->count == 0)
        pthread_cond_wait(&s->cv, &s->mu);
    s->count--;
    pthread_mutex_unlock(&s->mu);
}

static void sem_post_(Sem *s) {
    pthread_mutex_lock(&s->mu);
    s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

static int sem_trywait_(Sem *s) {
    int ok = 0;
    pthread_mutex_lock(&s->mu);
    if (s->count > 0) {
        s->count--;
        ok = 1;
    }
    pthread_mutex_unlock(&s->mu);
    return ok;
}

enum { LIMIT = 3, T = 8, ROUNDS = 100 };

static Sem slots;
static pthread_mutex_t stat_mu = PTHREAD_MUTEX_INITIALIZER;
static int inside, high_water, violations;
static long entries;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *worker(void *p) {
    (void)p;
    for (int i = 0; i < ROUNDS; i++) {
        sem_wait_(&slots);
        pthread_mutex_lock(&stat_mu);
        inside++;
        entries++;
        if (inside > high_water)
            high_water = inside;
        if (inside > LIMIT)
            violations++;
        pthread_mutex_unlock(&stat_mu);
        sched_yield();
        pthread_mutex_lock(&stat_mu);
        inside--;
        pthread_mutex_unlock(&stat_mu);
        sem_post_(&slots);
    }
    return NULL;
}

int main(void) {
    sem_open_(&slots, LIMIT);
    /* single-threaded semantics first */
    int got = 0;
    while (sem_trywait_(&slots))
        got++;
    printf("trywait drained %d permits\n", got);
    check(got == LIMIT, "permits");
    for (int i = 0; i < LIMIT; i++)
        sem_post_(&slots);

    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, NULL) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    check(violations == 0, "limit never exceeded");
    check(high_water <= LIMIT && high_water >= 1, "high water in range");
    check(inside == 0, "nobody left inside");
    check(slots.count == LIMIT, "all permits returned");
    check(entries == (long)T * ROUNDS, "entries");
    printf("entries=%ld\n", entries);
    printf("limit respected: %s\n", violations == 0 ? "yes" : "no");
    printf("permits at end=%d\n", slots.count);
    pthread_mutex_destroy(&slots.mu);
    pthread_cond_destroy(&slots.cv);
    return 0;
}
