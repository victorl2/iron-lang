/*
 * title: Mutex-protected counters with a paired invariant
 * topic: concurrency
 * covers: mutex critical sections, multi-field invariants, lock scope
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* Invariant under the lock: a + b == TOTAL always, and hits == sum of per-thread ops. */
enum { TOTAL = 1000, THREADS = 6, OPS = 4000 };

typedef struct {
    pthread_mutex_t mu;
    long a, b;
    long hits;
    long violations;
} Shared;

typedef struct {
    Shared *s;
    unsigned seed;
    long moved;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned next(unsigned *st) {
    *st ^= *st << 13;
    *st ^= *st >> 17;
    *st ^= *st << 5;
    return *st;
}

static void *worker(void *p) {
    Arg *w = p;
    Shared *s = w->s;
    for (int i = 0; i < OPS; i++) {
        unsigned r = next(&w->seed);
        long amt = (long)(r % 7) + 1;
        pthread_mutex_lock(&s->mu);
        if (s->a + s->b != TOTAL)
            s->violations++;
        if (r & 0x100) {
            if (s->a >= amt) {
                s->a -= amt;
                s->b += amt;
                w->moved += amt;
            }
        } else {
            if (s->b >= amt) {
                s->b -= amt;
                s->a += amt;
                w->moved += amt;
            }
        }
        s->hits++;
        pthread_mutex_unlock(&s->mu);
    }
    return NULL;
}

int main(void) {
    Shared s = {PTHREAD_MUTEX_INITIALIZER, TOTAL, 0, 0, 0};
    Arg args[THREADS];
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++) {
        args[i].s = &s;
        args[i].seed = 0x9e3779b9u * (unsigned)(i + 1);
        args[i].moved = 0;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int i = 0; i < THREADS; i++)
        check(pthread_join(th[i], NULL) == 0, "join");
    check(s.a + s.b == TOTAL, "final invariant");
    check(s.violations == 0, "no violations");
    check(s.hits == (long)THREADS * OPS, "hits");
    check(s.a >= 0 && s.b >= 0, "non-negative");
    printf("hits=%ld\n", s.hits);
    printf("violations=%ld\n", s.violations);
    printf("sum a+b=%ld\n", s.a + s.b);
    long moved = 0;
    for (int i = 0; i < THREADS; i++)
        moved += args[i].moved > 0;
    printf("threads that moved money=%ld\n", moved);
    pthread_mutex_destroy(&s.mu);
    return 0;
}
