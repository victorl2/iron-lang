/*
 * title: trylock with exponential back-off
 * topic: concurrency
 * covers: pthread_mutex_trylock, EBUSY, back-off loop, sched_yield, contention accounting
 * deps: libc, pthread, posix
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 4, ITERS = 2000 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long counter;

typedef struct {
    int id;
    long acquired;
    long failed;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void backoff_lock(long *failed) {
    unsigned delay = 1;
    for (;;) {
        int rc = pthread_mutex_trylock(&mu);
        if (rc == 0)
            return;
        check(rc == EBUSY, "trylock only fails with EBUSY");
        (*failed)++;
        for (unsigned i = 0; i < delay; i++)
            sched_yield();
        if (delay < 16)
            delay <<= 1;
    }
}

static void *worker(void *p) {
    Arg *a = p;
    for (int i = 0; i < ITERS; i++) {
        backoff_lock(&a->failed);
        counter++;
        a->acquired++;
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static void *probe(void *p) {
    int *rc = p;
    *rc = pthread_mutex_trylock(&mu);
    if (*rc == 0)
        pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    /* Deterministic part: a held mutex always reports EBUSY to other threads. */
    pthread_mutex_lock(&mu);
    int rc = -1;
    pthread_t pr;
    pthread_create(&pr, NULL, probe, &rc);
    pthread_join(pr, NULL);
    printf("probe while held: %s\n", rc == EBUSY ? "EBUSY" : "other");
    check(rc == EBUSY, "EBUSY while held");
    pthread_mutex_unlock(&mu);
    pthread_create(&pr, NULL, probe, &rc);
    pthread_join(pr, NULL);
    printf("probe when free: %s\n", rc == 0 ? "acquired" : "other");
    check(rc == 0, "free mutex acquired");

    /* Contended part: the number of failures varies, the acquisitions do not. */
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        args[i].acquired = 0;
        args[i].failed = 0;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    long acquired = 0;
    for (int i = 0; i < T; i++) {
        pthread_join(th[i], NULL);
        check(args[i].acquired == ITERS, "each thread acquired ITERS times");
        acquired += args[i].acquired;
    }
    check(counter == (long)T * ITERS, "counter");
    printf("counter=%ld acquisitions=%ld\n", counter, acquired);
    return 0;
}
