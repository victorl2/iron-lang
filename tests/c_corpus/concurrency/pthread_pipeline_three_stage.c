/*
 * title: Three-stage pipeline over blocking queues
 * topic: concurrency
 * covers: chained queues, end-of-stream marker, stage transforms, ordered output through single-consumer stages
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { QCAP = 4, N = 200, EOS = -1 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t not_empty, not_full;
    long buf[QCAP];
    int head, count;
} Queue;

static void q_init(Queue *q) {
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void q_put(Queue *q, long v) {
    pthread_mutex_lock(&q->mu);
    while (q->count == QCAP)
        pthread_cond_wait(&q->not_full, &q->mu);
    q->buf[(q->head + q->count++) % QCAP] = v;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

static long q_get(Queue *q) {
    pthread_mutex_lock(&q->mu);
    while (q->count == 0)
        pthread_cond_wait(&q->not_empty, &q->mu);
    long v = q->buf[q->head];
    q->head = (q->head + 1) % QCAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return v;
}

static Queue q1, q2, q3;
static long collected[N];
static int ncollected;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *stage_source(void *p) {
    (void)p;
    for (long i = 1; i <= N; i++)
        q_put(&q1, i);
    q_put(&q1, EOS);
    return NULL;
}

static void *stage_square_odd(void *p) {
    (void)p;
    for (;;) {
        long v = q_get(&q1);
        if (v == EOS) {
            q_put(&q2, EOS);
            return NULL;
        }
        q_put(&q2, (v % 2) ? v * v : v);
    }
}

static void *stage_filter_mod3(void *p) {
    (void)p;
    for (;;) {
        long v = q_get(&q2);
        if (v == EOS) {
            q_put(&q3, EOS);
            return NULL;
        }
        if (v % 3 != 0)
            q_put(&q3, v + 1);
    }
}

static void *stage_sink(void *p) {
    (void)p;
    for (;;) {
        long v = q_get(&q3);
        if (v == EOS)
            return NULL;
        collected[ncollected++] = v;
    }
}

int main(void) {
    q_init(&q1);
    q_init(&q2);
    q_init(&q3);
    pthread_t t[4];
    void *(*fn[4])(void *) = {stage_sink, stage_filter_mod3, stage_square_odd, stage_source};
    for (int i = 0; i < 4; i++)
        check(pthread_create(&t[i], NULL, fn[i], NULL) == 0, "create");
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);

    long expect[N];
    int ne = 0;
    for (long i = 1; i <= N; i++) {
        long v = (i % 2) ? i * i : i;
        if (v % 3 != 0)
            expect[ne++] = v + 1;
    }
    check(ne == ncollected, "count");
    long sum = 0;
    for (int i = 0; i < ne; i++) {
        check(collected[i] == expect[i], "order preserved through single-consumer stages");
        sum += collected[i];
    }
    printf("items out=%d sum=%ld\n", ncollected, sum);
    printf("first:");
    for (int i = 0; i < 8; i++)
        printf(" %ld", collected[i]);
    printf("\nlast: %ld\n", collected[ncollected - 1]);
    return 0;
}
