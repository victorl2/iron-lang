/*
 * title: Dining philosophers with a waiter (arbitrator)
 * topic: concurrency
 * covers: arbitrator, monitor with condition variable, deadlock freedom, atomic exclusivity checks
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 5, MEALS = 400 };

/* The waiter owns the table state: a philosopher may only pick up both forks at once. */
static pthread_mutex_t waiter_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t waiter_cv = PTHREAD_COND_INITIALIZER;
static int fork_free[N];
static int waits_total; /* protected by waiter_mu, only used as a liveness sanity value */

static atomic_int fork_in_use[N];
static atomic_int diners_now;
static atomic_int diners_max;

typedef struct {
    int id;
    int meals;
    unsigned checksum;
    int violations;
} Phil;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void waiter_request(int id) {
    int l = id, r = (id + 1) % N;
    pthread_mutex_lock(&waiter_mu);
    while (!(fork_free[l] && fork_free[r])) {
        waits_total++;
        pthread_cond_wait(&waiter_cv, &waiter_mu);
    }
    fork_free[l] = 0;
    fork_free[r] = 0;
    pthread_mutex_unlock(&waiter_mu);
}

static void waiter_release(int id) {
    int l = id, r = (id + 1) % N;
    pthread_mutex_lock(&waiter_mu);
    fork_free[l] = 1;
    fork_free[r] = 1;
    pthread_cond_broadcast(&waiter_cv);
    pthread_mutex_unlock(&waiter_mu);
}

static void note_max(int now) {
    int cur = atomic_load(&diners_max);
    while (now > cur && !atomic_compare_exchange_weak(&diners_max, &cur, now)) {
    }
}

static void *philosopher(void *arg) {
    Phil *p = arg;
    int l = p->id, r = (p->id + 1) % N;
    for (int m = 0; m < MEALS; m++) {
        waiter_request(p->id);
        /* independent check that nobody else holds either fork */
        if (atomic_exchange(&fork_in_use[l], 1) != 0)
            p->violations++;
        if (atomic_exchange(&fork_in_use[r], 1) != 0)
            p->violations++;
        note_max(atomic_fetch_add(&diners_now, 1) + 1);
        p->meals++;
        p->checksum = p->checksum * 31u + (unsigned)(p->id * 1000 + m);
        atomic_fetch_sub(&diners_now, 1);
        atomic_store(&fork_in_use[l], 0);
        atomic_store(&fork_in_use[r], 0);
        waiter_release(p->id);
    }
    return NULL;
}

int main(void) {
    for (int i = 0; i < N; i++)
        fork_free[i] = 1;
    Phil ph[N] = {{0, 0, 0, 0}};
    pthread_t th[N];
    for (int i = 0; i < N; i++) {
        ph[i].id = i;
        check(pthread_create(&th[i], NULL, philosopher, &ph[i]) == 0, "create");
    }
    for (int i = 0; i < N; i++)
        pthread_join(th[i], NULL);

    long total = 0;
    for (int i = 0; i < N; i++) {
        check(ph[i].violations == 0, "fork exclusivity");
        check(ph[i].meals == MEALS, "meals");
        total += ph[i].meals;
        printf("philosopher %d meals=%d checksum=%08x\n", i, ph[i].meals, ph[i].checksum);
    }
    /* each fork is used by its two neighbours, so a fork serves 2*MEALS meals */
    printf("total meals=%ld fork uses=%d\n", total, 2 * MEALS);
    printf("simultaneous diners never exceeded %d: %s\n", N / 2, atomic_load(&diners_max) <= N / 2 ? "yes" : "no");
    check(atomic_load(&diners_max) <= N / 2, "at most two eat at once");
    check(atomic_load(&diners_now) == 0, "table empty");
    for (int i = 0; i < N; i++)
        check(fork_free[i] == 1, "forks returned");
    (void)waits_total;
    return 0;
}
