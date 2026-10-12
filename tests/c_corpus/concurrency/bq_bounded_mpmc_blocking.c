/*
 * title: Bounded blocking MPMC queue with close semantics
 * topic: concurrency
 * covers: bounded ring buffer, not-full/not-empty condvars, close and drain, per-producer order, high-water mark
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { CAP = 8, NPROD = 3, NCONS = 4, PER = 600 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t not_full, not_empty;
    int buf[CAP];
    int head, count, closed;
    int high_water;
} BQ;

static BQ q = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, {0}, 0, 0, 0, 0};

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* returns 0 on success, -1 if the queue was closed */
static int bq_put(int v) {
    pthread_mutex_lock(&q.mu);
    while (q.count == CAP && !q.closed)
        pthread_cond_wait(&q.not_full, &q.mu);
    if (q.closed) {
        pthread_mutex_unlock(&q.mu);
        return -1;
    }
    q.buf[(q.head + q.count) % CAP] = v;
    q.count++;
    if (q.count > q.high_water)
        q.high_water = q.count;
    pthread_cond_signal(&q.not_empty);
    pthread_mutex_unlock(&q.mu);
    return 0;
}

/* returns 0 on success, -1 when closed and drained */
static int bq_get(int *out) {
    pthread_mutex_lock(&q.mu);
    while (q.count == 0 && !q.closed)
        pthread_cond_wait(&q.not_empty, &q.mu);
    if (q.count == 0) {
        pthread_mutex_unlock(&q.mu);
        return -1;
    }
    *out = q.buf[q.head];
    q.head = (q.head + 1) % CAP;
    q.count--;
    pthread_cond_signal(&q.not_full);
    pthread_mutex_unlock(&q.mu);
    return 0;
}

static void bq_close(void) {
    pthread_mutex_lock(&q.mu);
    q.closed = 1;
    pthread_cond_broadcast(&q.not_empty);
    pthread_cond_broadcast(&q.not_full);
    pthread_mutex_unlock(&q.mu);
}

typedef struct {
    int id;
    long got;
    long sum;
    int last[NPROD];
    int order_bad;
    int per_prod[NPROD];
} Cons;

static void *producer(void *p) {
    int id = (int)(long)p;
    for (int i = 0; i < PER; i++) {
        int v = id * 100000 + i;
        check(bq_put(v) == 0, "put on open queue");
    }
    return NULL;
}

static void *consumer(void *p) {
    Cons *c = p;
    for (int i = 0; i < NPROD; i++)
        c->last[i] = -1;
    int v;
    while (bq_get(&v) == 0) {
        int pr = v / 100000, seq = v % 100000;
        if (seq <= c->last[pr])
            c->order_bad++;
        c->last[pr] = seq;
        c->per_prod[pr]++;
        c->got++;
        c->sum += v;
    }
    return NULL;
}

int main(void) {
    pthread_t pt[NPROD], ct[NCONS];
    Cons cons[NCONS] = {{0}};
    for (int i = 0; i < NCONS; i++) {
        cons[i].id = i;
        check(pthread_create(&ct[i], NULL, consumer, &cons[i]) == 0, "create consumer");
    }
    for (long i = 0; i < NPROD; i++)
        check(pthread_create(&pt[i], NULL, producer, (void *)i) == 0, "create producer");
    for (int i = 0; i < NPROD; i++)
        pthread_join(pt[i], NULL);
    bq_close();
    for (int i = 0; i < NCONS; i++)
        pthread_join(ct[i], NULL);

    long total = 0, sum = 0;
    int per_prod[NPROD] = {0};
    for (int i = 0; i < NCONS; i++) {
        check(cons[i].order_bad == 0, "per-producer order at each consumer");
        total += cons[i].got;
        sum += cons[i].sum;
        for (int p = 0; p < NPROD; p++)
            per_prod[p] += cons[i].per_prod[p];
    }
    long expect = 0;
    for (int p = 0; p < NPROD; p++)
        for (int i = 0; i < PER; i++)
            expect += p * 100000L + i;
    check(total == (long)NPROD * PER, "total count");
    check(sum == expect, "checksum");
    check(q.count == 0, "queue drained");
    check(q.high_water <= CAP && q.high_water >= 1, "high water within capacity");
    check(bq_put(1) == -1, "put after close fails");

    printf("capacity %d producers %d consumers %d\n", CAP, NPROD, NCONS);
    for (int p = 0; p < NPROD; p++)
        printf("producer %d delivered %d\n", p, per_prod[p]);
    printf("total %ld sum %ld\n", total, sum);
    printf("order preserved per producer: yes\n");
    printf("high water within capacity: yes\n");
    printf("put after close: rejected\n");
    return 0;
}
