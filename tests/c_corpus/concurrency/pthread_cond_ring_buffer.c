/*
 * title: Bounded ring buffer with condition variables
 * topic: concurrency
 * covers: producer/consumer, predicate loops, not-full/not-empty condvars, checksums
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { CAP = 5, PRODUCERS = 3, CONSUMERS = 3, PER = 500 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t not_full, not_empty;
    unsigned buf[CAP];
    int head, count;
    int producers_left;
    int max_seen;
} Ring;

typedef struct {
    Ring *r;
    int id;
    unsigned long long sum;
    long items;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned value_for(int producer, int i) {
    unsigned x = (unsigned)(producer * 100000 + i) * 2654435761u;
    return (x >> 7) & 0xffffu;
}

static void *producer(void *p) {
    Arg *a = p;
    Ring *r = a->r;
    for (int i = 0; i < PER; i++) {
        unsigned v = value_for(a->id, i);
        pthread_mutex_lock(&r->mu);
        while (r->count == CAP)
            pthread_cond_wait(&r->not_full, &r->mu);
        r->buf[(r->head + r->count) % CAP] = v;
        r->count++;
        if (r->count > r->max_seen)
            r->max_seen = r->count;
        pthread_cond_signal(&r->not_empty);
        pthread_mutex_unlock(&r->mu);
        a->sum += v;
        a->items++;
    }
    pthread_mutex_lock(&r->mu);
    r->producers_left--;
    pthread_cond_broadcast(&r->not_empty); /* wake consumers so they can see the end */
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

static void *consumer(void *p) {
    Arg *a = p;
    Ring *r = a->r;
    for (;;) {
        pthread_mutex_lock(&r->mu);
        while (r->count == 0 && r->producers_left > 0)
            pthread_cond_wait(&r->not_empty, &r->mu);
        if (r->count == 0) {
            pthread_mutex_unlock(&r->mu);
            break;
        }
        unsigned v = r->buf[r->head];
        r->head = (r->head + 1) % CAP;
        r->count--;
        pthread_cond_signal(&r->not_full);
        pthread_mutex_unlock(&r->mu);
        a->sum += v;
        a->items++;
    }
    return NULL;
}

int main(void) {
    Ring r = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER,
              {0}, 0, 0, PRODUCERS, 0};
    Arg pa[PRODUCERS], ca[CONSUMERS];
    pthread_t pt[PRODUCERS], ct[CONSUMERS];
    for (int i = 0; i < CONSUMERS; i++) {
        ca[i].r = &r;
        ca[i].id = i;
        ca[i].sum = 0;
        ca[i].items = 0;
        check(pthread_create(&ct[i], NULL, consumer, &ca[i]) == 0, "create c");
    }
    for (int i = 0; i < PRODUCERS; i++) {
        pa[i].r = &r;
        pa[i].id = i;
        pa[i].sum = 0;
        pa[i].items = 0;
        check(pthread_create(&pt[i], NULL, producer, &pa[i]) == 0, "create p");
    }
    for (int i = 0; i < PRODUCERS; i++)
        pthread_join(pt[i], NULL);
    for (int i = 0; i < CONSUMERS; i++)
        pthread_join(ct[i], NULL);

    unsigned long long ps = 0, cs = 0, expect = 0;
    long pi = 0, ci = 0;
    for (int i = 0; i < PRODUCERS; i++) {
        ps += pa[i].sum;
        pi += pa[i].items;
        for (int k = 0; k < PER; k++)
            expect += value_for(i, k);
    }
    for (int i = 0; i < CONSUMERS; i++) {
        cs += ca[i].sum;
        ci += ca[i].items;
    }
    check(pi == (long)PRODUCERS * PER, "produced count");
    check(ci == pi, "consumed count");
    check(ps == expect, "producer sum");
    check(cs == ps, "consumer sum");
    check(r.count == 0, "drained");
    check(r.max_seen <= CAP, "capacity respected");
    printf("produced=%ld consumed=%ld\n", pi, ci);
    printf("checksum=%llu\n", cs);
    printf("capacity respected: %s\n", r.max_seen <= CAP ? "yes" : "no");
    return 0;
}
