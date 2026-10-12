/*
 * title: Fair counting semaphore with grant statistics
 * topic: concurrency
 * covers: counting semaphore, ticket-ordered FIFO grants, try-acquire, in-flight bound, fairness counters
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 6, PERMITS = 2, ROUNDS = 150 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int count;
    unsigned next_ticket, serving;
    unsigned long grants;
} Sem;

static Sem sem = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PERMITS, 0, 0, 0};

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* Waiters are served strictly in ticket order, even when permits are free. */
static unsigned sem_acquire(void) {
    pthread_mutex_lock(&sem.mu);
    unsigned my = sem.next_ticket++;
    while (sem.serving != my || sem.count == 0)
        pthread_cond_wait(&sem.cv, &sem.mu);
    sem.count--;
    sem.serving++;
    sem.grants++;
    pthread_cond_broadcast(&sem.cv);
    pthread_mutex_unlock(&sem.mu);
    return my;
}

static int sem_try_acquire(void) {
    int ok = 0;
    pthread_mutex_lock(&sem.mu);
    if (sem.serving == sem.next_ticket && sem.count > 0) {
        sem.count--;
        ok = 1;
    }
    pthread_mutex_unlock(&sem.mu);
    return ok;
}

static void sem_release(void) {
    pthread_mutex_lock(&sem.mu);
    sem.count++;
    pthread_cond_broadcast(&sem.cv);
    pthread_mutex_unlock(&sem.mu);
}

static atomic_int inflight, max_inflight, over_limit;

typedef struct {
    int id;
    int acquired;
    unsigned last_ticket;
    int order_violations;
} Arg;

static void *worker(void *p) {
    Arg *a = p;
    for (int i = 0; i < ROUNDS; i++) {
        unsigned tk = sem_acquire();
        if (i > 0 && tk <= a->last_ticket)
            a->order_violations++;
        a->last_ticket = tk;
        int now = atomic_fetch_add(&inflight, 1) + 1;
        if (now > PERMITS)
            atomic_fetch_add(&over_limit, 1);
        int m = atomic_load(&max_inflight);
        while (now > m && !atomic_compare_exchange_weak(&max_inflight, &m, now))
            ;
        a->acquired++;
        atomic_fetch_sub(&inflight, 1);
        sem_release();
    }
    return NULL;
}

int main(void) {
    pthread_t th[T];
    Arg args[T] = {{0}};
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    int mn = ROUNDS, mx = 0, viol = 0;
    printf("threads=%d permits=%d rounds=%d\n", T, PERMITS, ROUNDS);
    for (int i = 0; i < T; i++) {
        printf("thread %d acquired %d\n", i, args[i].acquired);
        if (args[i].acquired < mn) mn = args[i].acquired;
        if (args[i].acquired > mx) mx = args[i].acquired;
        viol += args[i].order_violations;
    }
    printf("total grants %lu, tickets issued %u\n", sem.grants, sem.next_ticket);
    printf("fairness spread %d\n", mx - mn);
    printf("own ticket order violations %d\n", viol);
    printf("over-limit observations %d, max in-flight within limit %s\n", atomic_load(&over_limit),
           atomic_load(&max_inflight) <= PERMITS ? "yes" : "no");
    check(atomic_load(&over_limit) == 0, "permit limit exceeded");
    check(sem.grants == (unsigned long)T * ROUNDS, "grant count");
    check(sem.next_ticket == sem.serving, "tickets drained");
    check(mx == mn && viol == 0, "fairness");
    check(sem.count == PERMITS, "permits restored");

    /* try-acquire drains exactly PERMITS permits, then fails. */
    int got = 0;
    for (int i = 0; i < PERMITS + 3; i++)
        got += sem_try_acquire();
    printf("try_acquire successes %d of %d\n", got, PERMITS + 3);
    check(got == PERMITS, "try_acquire count");
    for (int i = 0; i < got; i++)
        sem_release();
    check(sem.count == PERMITS, "release restored");
    printf("final count %d\n", sem.count);
    return 0;
}
