/*
 * title: Blocking resource pool with exclusive leases and targeted borrow
 * topic: concurrency
 * covers: bounded object pool, blocking acquire, try_acquire, acquire by id, exclusive-use invariant, lease accounting
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { K = 3, NT = 8, ITERS = 120 };

typedef struct {
    int id;
    int in_use;
    long uses;
    long acc; /* only modified while leased */
} Res;

static Res res[K];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int leased, max_leased, waiting_specific;
static int exclusivity_violations;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static Res *acquire_locked(int want) { /* want < 0: any free resource */
    for (int i = 0; i < K; i++)
        if ((want < 0 || want == i) && !res[i].in_use) {
            res[i].in_use = 1;
            res[i].uses++;
            leased++;
            if (leased > max_leased)
                max_leased = leased;
            return &res[i];
        }
    return NULL;
}

static Res *acquire(int want) {
    pthread_mutex_lock(&mu);
    Res *r;
    if (want >= 0)
        waiting_specific++;
    pthread_cond_broadcast(&cv);
    while ((r = acquire_locked(want)) == NULL)
        pthread_cond_wait(&cv, &mu);
    if (want >= 0)
        waiting_specific--;
    pthread_mutex_unlock(&mu);
    return r;
}

static Res *try_acquire(void) {
    pthread_mutex_lock(&mu);
    Res *r = acquire_locked(-1);
    pthread_mutex_unlock(&mu);
    return r;
}

static void release(Res *r) {
    pthread_mutex_lock(&mu);
    check(r->in_use, "release of a leased resource");
    r->in_use = 0;
    leased--;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static void *worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < ITERS; i++) {
        Res *r = acquire(-1);
        /* the lease is exclusive: a witness counter should move by exactly what we add */
        long before = r->acc;
        r->acc += id * 1000 + i;
        if (r->acc != before + id * 1000 + i)
            exclusivity_violations++;
        release(r);
    }
    return NULL;
}

static Res *targeted_result;

static void *targeted(void *arg) {
    targeted_result = acquire(1);
    targeted_result->acc += 7;
    return NULL;
}

int main(void) {
    for (int i = 0; i < K; i++)
        res[i].id = i;

    /* Deterministic part: drain the pool with try_acquire, then a targeted acquire must wait. */
    Res *held[K];
    for (int i = 0; i < K; i++) {
        held[i] = try_acquire();
        check(held[i] != NULL, "pool has K resources");
    }
    Res *none = try_acquire();
    printf("try_acquire on exhausted pool: %s\n", none ? "got one" : "none");
    check(none == NULL, "exhausted");
    pthread_t tt;
    check(pthread_create(&tt, NULL, targeted, NULL) == 0, "targeted");
    pthread_mutex_lock(&mu);
    while (waiting_specific == 0)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    /* release resources 0 and 2 first: the targeted borrower wants id 1 and must keep waiting */
    release(held[0]);
    release(held[2]);
    Res *again = try_acquire();
    printf("after freeing ids 0 and 2, try_acquire got id %d\n", again ? again->id : -1);
    check(again != NULL && again->id == 0, "lowest free id first");
    pthread_mutex_lock(&mu);
    int still_waiting = waiting_specific;
    pthread_mutex_unlock(&mu);
    check(still_waiting == 1, "targeted borrower still blocked");
    release(again);
    release(held[1]);
    pthread_join(tt, NULL);
    printf("targeted borrower got id %d\n", targeted_result->id);
    check(targeted_result->id == 1, "got the requested id");
    release(targeted_result);

    /* Contention: many workers share K resources. */
    pthread_t th[NT];
    for (long i = 0; i < NT; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)i) == 0, "create");
    for (int i = 0; i < NT; i++)
        pthread_join(th[i], NULL);
    long uses = 0, acc = 0, expect = 7;
    for (int i = 0; i < K; i++) {
        check(!res[i].in_use, "all returned");
        uses += res[i].uses;
        acc += res[i].acc;
    }
    for (long id = 0; id < NT; id++)
        for (int i = 0; i < ITERS; i++)
            expect += id * 1000 + i;
    check(exclusivity_violations == 0, "leases exclusive");
    check(acc == expect, "accumulated value conserved");
    check(max_leased <= K && leased == 0, "never more leases than resources");
    /* leases outside the contention phase: three drains, one lowest-id lease, one targeted */
    check(uses == (long)NT * ITERS + 3 + 1 + 1, "lease count");
    printf("leases in contention phase: %d\n", NT * ITERS);
    printf("accumulated value %ld across %d resources\n", acc, K);
    printf("never more than %d leases at once: yes\n", K);
    return 0;
}
