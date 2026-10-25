/*
 * title: Condition-variable signal with ticket dispenser
 * topic: concurrency
 * covers: pthread_cond_signal, single wakeup, spurious wakeup safety, work tickets
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { WAITERS = 5, TICKETS = 200 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int available; /* tickets that may be taken */
    int closed;
    int wakeups;   /* wakeups observed, including ones that found nothing */
} Desk;

typedef struct {
    Desk *d;
    int taken;
    long sum;
    int last;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int ticket_counter = 0; /* protected by desk mutex */

static void *waiter(void *p) {
    Arg *a = p;
    Desk *d = a->d;
    pthread_mutex_lock(&d->mu);
    for (;;) {
        while (d->available == 0 && !d->closed) {
            pthread_cond_wait(&d->cv, &d->mu); /* predicate loop guards spurious wakeups */
            d->wakeups++;
        }
        if (d->available == 0 && d->closed)
            break;
        d->available--;
        int t = ++ticket_counter;
        a->taken++;
        a->sum += t;
        a->last = t;
    }
    pthread_mutex_unlock(&d->mu);
    return NULL;
}

int main(void) {
    Desk d = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0};
    Arg args[WAITERS];
    pthread_t th[WAITERS];
    for (int i = 0; i < WAITERS; i++) {
        args[i].d = &d;
        args[i].taken = 0;
        args[i].sum = 0;
        args[i].last = 0;
        check(pthread_create(&th[i], NULL, waiter, &args[i]) == 0, "create");
    }
    /* issue tickets one signal at a time, with batches issued under a single lock */
    int issued = 0;
    while (issued < TICKETS) {
        int batch = 1 + (issued % 4);
        if (issued + batch > TICKETS)
            batch = TICKETS - issued;
        pthread_mutex_lock(&d.mu);
        d.available += batch;
        for (int k = 0; k < batch; k++)
            pthread_cond_signal(&d.cv);
        pthread_mutex_unlock(&d.mu);
        issued += batch;
    }
    pthread_mutex_lock(&d.mu);
    d.closed = 1;
    pthread_cond_broadcast(&d.cv);
    pthread_mutex_unlock(&d.mu);
    for (int i = 0; i < WAITERS; i++)
        pthread_join(th[i], NULL);

    int taken = 0;
    long sum = 0;
    for (int i = 0; i < WAITERS; i++) {
        taken += args[i].taken;
        sum += args[i].sum;
    }
    check(taken == TICKETS, "all tickets taken");
    check(ticket_counter == TICKETS, "counter");
    check(sum == (long)TICKETS * (TICKETS + 1) / 2, "ticket numbers unique and complete");
    check(d.available == 0, "none left");
    printf("issued=%d taken=%d\n", issued, taken);
    printf("ticket number sum=%ld\n", sum);
    return 0;
}
