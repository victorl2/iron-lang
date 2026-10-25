/*
 * title: Join ordering independent of completion order
 * topic: concurrency
 * covers: pthread_join in arbitrary order, results by index, forced finish order via condvar chain
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 6 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int next_to_finish;  /* threads finish in decreasing id order: T-1, T-2, ... */
    int finish_seq[T];
    int nfinished;
} Order;

typedef struct {
    Order *o;
    int id;
    long value;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *worker(void *p) {
    Arg *a = p;
    long v = 1;
    for (int i = 0; i < 20 + a->id; i++)
        v = (v * 31 + a->id) % 1000003;
    a->value = v;
    Order *o = a->o;
    pthread_mutex_lock(&o->mu);
    while (o->next_to_finish != a->id)
        pthread_cond_wait(&o->cv, &o->mu);
    o->finish_seq[o->nfinished++] = a->id;
    o->next_to_finish--;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mu);
    return (void *)(size_t)(a->id + 100);
}

int main(void) {
    Order o = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, T - 1, {0}, 0};
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        args[i].o = &o;
        args[i].id = i;
        args[i].value = 0;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    /* join in ascending order although thread T-1 finishes first and thread 0 last */
    for (int i = 0; i < T; i++) {
        void *r;
        check(pthread_join(th[i], &r) == 0, "join");
        check((size_t)r == (size_t)(i + 100), "return value belongs to this thread");
        printf("joined %d -> %zu, value %ld\n", i, (size_t)r, args[i].value);
    }
    printf("finish order:");
    for (int i = 0; i < T; i++) {
        check(o.finish_seq[i] == T - 1 - i, "forced finish order");
        printf(" %d", o.finish_seq[i]);
    }
    printf("\n");
    return 0;
}
