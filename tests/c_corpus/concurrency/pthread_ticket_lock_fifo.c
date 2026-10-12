/*
 * title: FIFO ticket lock from mutex and condvar
 * topic: concurrency
 * covers: ticket lock, now-serving counter, fairness by construction, forced acquisition order
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 6, ROUNDS = 50 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned next_ticket, now_serving;
} TicketLock;

static TicketLock tl = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};

static void tl_lock(unsigned *my) {
    pthread_mutex_lock(&tl.mu);
    *my = tl.next_ticket++;
    while (tl.now_serving != *my)
        pthread_cond_wait(&tl.cv, &tl.mu);
    pthread_mutex_unlock(&tl.mu);
}

static void tl_unlock(void) {
    pthread_mutex_lock(&tl.mu);
    tl.now_serving++;
    pthread_cond_broadcast(&tl.cv);
    pthread_mutex_unlock(&tl.mu);
}

static long shared_counter;
static unsigned served_order[T * ROUNDS];
static int served;

typedef struct {
    int id;
    unsigned last_ticket;
    int fifo_violations;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *worker(void *p) {
    Arg *a = p;
    for (int i = 0; i < ROUNDS; i++) {
        unsigned my;
        tl_lock(&my);
        /* inside the critical section tickets must have been served strictly in order */
        if (served > 0 && served_order[served - 1] + 1 != my)
            a->fifo_violations++;
        if ((unsigned)served != my)
            a->fifo_violations++;
        served_order[served++] = my;
        shared_counter++;
        if (i > 0 && my <= a->last_ticket)
            a->fifo_violations++;
        a->last_ticket = my;
        tl_unlock();
    }
    return NULL;
}

int main(void) {
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        args[i].last_ticket = 0;
        args[i].fifo_violations = 0;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    int bad = 0;
    for (int i = 0; i < T; i++) {
        pthread_join(th[i], NULL);
        bad += args[i].fifo_violations;
    }
    check(bad == 0, "fifo order");
    check(shared_counter == T * ROUNDS, "counter");
    check(tl.next_ticket == T * ROUNDS && tl.now_serving == T * ROUNDS, "ticket counters");
    for (int i = 0; i < T * ROUNDS; i++)
        check(served_order[i] == (unsigned)i, "served order is 0,1,2,...");
    printf("tickets issued=%u served=%u\n", tl.next_ticket, tl.now_serving);
    printf("counter=%ld violations=%d\n", shared_counter, bad);
    return 0;
}
