/*
 * title: Fixed thread pool with ticketed results collected in order
 * topic: concurrency
 * covers: fixed worker pool, submit returns ticket, per-ticket completion state, ordered collection, function pointers
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef long (*TaskFn)(long);

typedef struct {
    TaskFn fn;
    long arg;
    long result;
    int done;
} Ticket;

enum { NW = 4, NT = 60 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER, done_cv = PTHREAD_COND_INITIALIZER;
static Ticket tickets[NT];
static int ntickets, next_run, stop;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long collatz_steps(long n) {
    long s = 0;
    while (n != 1) {
        n = (n % 2) ? 3 * n + 1 : n / 2;
        s++;
    }
    return s;
}

static long digit_sum_sq(long n) {
    long s = 0;
    while (n > 0) {
        long d = n % 10;
        s += d * d;
        n /= 10;
    }
    return s;
}

static long tri_mod(long n) {
    return (n * (n + 1) / 2) % 1009;
}

static long popcount_l(long n) {
    long c = 0;
    unsigned long u = (unsigned long)n;
    while (u) {
        c += (long)(u & 1u);
        u >>= 1;
    }
    return c;
}

static void *worker(void *arg) {
    pthread_mutex_lock(&mu);
    for (;;) {
        while (next_run >= ntickets && !stop)
            pthread_cond_wait(&work_cv, &mu);
        if (next_run >= ntickets && stop)
            break;
        int t = next_run++;
        TaskFn fn = tickets[t].fn;
        long a = tickets[t].arg;
        pthread_mutex_unlock(&mu);
        long r = fn(a);
        pthread_mutex_lock(&mu);
        tickets[t].result = r;
        tickets[t].done = 1;
        pthread_cond_broadcast(&done_cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

static int submit(TaskFn fn, long arg) {
    pthread_mutex_lock(&mu);
    check(ntickets < NT, "ticket capacity");
    int t = ntickets++;
    tickets[t].fn = fn;
    tickets[t].arg = arg;
    tickets[t].done = 0;
    pthread_cond_signal(&work_cv);
    pthread_mutex_unlock(&mu);
    return t;
}

static long collect(int t) {
    pthread_mutex_lock(&mu);
    while (!tickets[t].done)
        pthread_cond_wait(&done_cv, &mu);
    long r = tickets[t].result;
    pthread_mutex_unlock(&mu);
    return r;
}

int main(void) {
    TaskFn fns[4] = {collatz_steps, digit_sum_sq, tri_mod, popcount_l};
    const char *names[4] = {"collatz", "digitsq", "tri1009", "popcnt"};
    pthread_t th[NW];
    for (int i = 0; i < NW; i++)
        check(pthread_create(&th[i], NULL, worker, NULL) == 0, "create");
    int id[NT];
    for (int i = 0; i < NT; i++)
        id[i] = submit(fns[i % 4], 27 + 13L * i);
    long sum = 0, seqsum = 0;
    for (int i = 0; i < NT; i++) {
        check(id[i] == i, "tickets issued in submit order");
        long r = collect(id[i]);
        long expect = fns[i % 4](27 + 13L * i);
        check(r == expect, "result matches sequential evaluation");
        sum += r;
        seqsum += expect;
        if (i < 12)
            printf("ticket %2d %-8s(%3ld) = %ld\n", i, names[i % 4], 27 + 13L * i, r);
    }
    pthread_mutex_lock(&mu);
    stop = 1;
    pthread_cond_broadcast(&work_cv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);
    check(sum == seqsum, "sums agree");
    check(next_run == NT, "every ticket executed once");
    printf("tickets %d total %ld\n", NT, sum);
    return 0;
}
