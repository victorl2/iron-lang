/*
 * title: Dining philosophers with N-1 seats
 * topic: concurrency
 * covers: counting semaphore built on condvar, pigeonhole deadlock avoidance, per-fork mutexes
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 7, MEALS = 300 };

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int v;
} Sem;

static void sem_make(Sem *s, int v) {
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->v = v;
}
static void sem_p(Sem *s) {
    pthread_mutex_lock(&s->m);
    while (s->v == 0)
        pthread_cond_wait(&s->c, &s->m);
    s->v--;
    pthread_mutex_unlock(&s->m);
}
static void sem_v(Sem *s) {
    pthread_mutex_lock(&s->m);
    s->v++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}
static void sem_drop(Sem *s) {
    pthread_mutex_destroy(&s->m);
    pthread_cond_destroy(&s->c);
}

static Sem seats;
static pthread_mutex_t fork_mu[N];
static atomic_int seated, seated_max;
static int fork_uses[N]; /* each guarded by its fork mutex */

typedef struct {
    int id, meals;
    unsigned long weight;
} Phil;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *philosopher(void *arg) {
    Phil *p = arg;
    int l = p->id, r = (p->id + 1) % N;
    for (int m = 0; m < MEALS; m++) {
        sem_p(&seats);
        int now = atomic_fetch_add(&seated, 1) + 1;
        int cur = atomic_load(&seated_max);
        while (now > cur && !atomic_compare_exchange_weak(&seated_max, &cur, now)) {
        }
        /* everybody uses the same left-then-right order: only the seat limit prevents deadlock */
        pthread_mutex_lock(&fork_mu[l]);
        pthread_mutex_lock(&fork_mu[r]);
        fork_uses[l]++;
        fork_uses[r]++;
        p->meals++;
        p->weight += (unsigned long)(p->id + 1) * (unsigned long)(m + 1);
        pthread_mutex_unlock(&fork_mu[r]);
        pthread_mutex_unlock(&fork_mu[l]);
        atomic_fetch_sub(&seated, 1);
        sem_v(&seats);
    }
    return NULL;
}

int main(void) {
    sem_make(&seats, N - 1);
    for (int i = 0; i < N; i++)
        pthread_mutex_init(&fork_mu[i], NULL);
    Phil ph[N] = {{0, 0, 0}};
    pthread_t th[N];
    for (int i = 0; i < N; i++) {
        ph[i].id = i;
        check(pthread_create(&th[i], NULL, philosopher, &ph[i]) == 0, "create");
    }
    for (int i = 0; i < N; i++)
        pthread_join(th[i], NULL);

    for (int i = 0; i < N; i++) {
        check(ph[i].meals == MEALS, "meals");
        printf("philosopher %d meals=%d weight=%lu\n", i, ph[i].meals, ph[i].weight);
    }
    for (int f = 0; f < N; f++) {
        /* fork f is used by philosopher f and philosopher f-1 */
        int expect = ph[f].meals + ph[(f + N - 1) % N].meals;
        check(fork_uses[f] == expect, "fork uses");
        printf("fork %d used %d times\n", f, fork_uses[f]);
    }
    printf("seat limit %d respected: %s\n", N - 1, atomic_load(&seated_max) <= N - 1 ? "yes" : "no");
    check(atomic_load(&seated_max) <= N - 1, "seat limit");
    for (int i = 0; i < N; i++)
        pthread_mutex_destroy(&fork_mu[i]);
    sem_drop(&seats);
    return 0;
}
