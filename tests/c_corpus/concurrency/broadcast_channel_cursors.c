/*
 * title: Broadcast channel with per-subscriber cursors and late join
 * topic: concurrency
 * covers: shared ring log, subscriber cursors, slowest-reader backpressure, late subscription, distinct reductions
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { CAP = 8, MSGS = 300, MAXSUB = 4, LATE_AT = 100 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long ring[CAP];
static long head;                /* number of messages published */
static long cursor[MAXSUB];      /* next message each subscriber will read */
static int active[MAXSUB];
static int closed, late_registered;

typedef struct {
    int id;
    long first_seq, count, sum, xr, weighted;
    int in_order;
} Sub;

static Sub subs[MAXSUB];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long slowest_locked(void) {
    long m = head;
    for (int i = 0; i < MAXSUB; i++)
        if (active[i] && cursor[i] < m)
            m = cursor[i];
    return m;
}

static long value(long seq) {
    return (seq * 2654435761L) % 100000 + seq;
}

static void *publisher(void *arg) {
    for (long s = 0; s < MSGS; s++) {
        pthread_mutex_lock(&mu);
        if (s == LATE_AT) {
            /* hold the stream here until the late subscriber has joined */
            while (!late_registered)
                pthread_cond_wait(&cv, &mu);
        }
        while (head - slowest_locked() >= CAP)
            pthread_cond_wait(&cv, &mu);
        ring[head % CAP] = value(s);
        head++;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    closed = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void *subscriber(void *arg) {
    Sub *me = arg;
    pthread_mutex_lock(&mu);
    me->first_seq = cursor[me->id];
    me->in_order = 1;
    for (;;) {
        while (cursor[me->id] == head && !closed)
            pthread_cond_wait(&cv, &mu);
        if (cursor[me->id] == head && closed)
            break;
        long seq = cursor[me->id];
        long v = ring[seq % CAP];
        cursor[me->id]++;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
        if (v != value(seq))
            me->in_order = 0;
        me->count++;
        me->sum += v;
        me->xr ^= v;
        me->weighted += v * (me->id + 1) % 977;
        pthread_mutex_lock(&mu);
    }
    active[me->id] = 0; /* leave: no longer holds back the publisher */
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    pthread_t pt, st[MAXSUB];
    /* three subscribers register before the stream starts */
    pthread_mutex_lock(&mu);
    for (int i = 0; i < 3; i++) {
        active[i] = 1;
        cursor[i] = 0;
        subs[i].id = i;
    }
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < 3; i++)
        check(pthread_create(&st[i], NULL, subscriber, &subs[i]) == 0, "subscriber");
    check(pthread_create(&pt, NULL, publisher, NULL) == 0, "publisher");

    /* the fourth subscriber joins once exactly LATE_AT messages were published */
    pthread_mutex_lock(&mu);
    while (head < LATE_AT)
        pthread_cond_wait(&cv, &mu);
    check(head == LATE_AT, "publisher holds at the join point");
    active[3] = 1;
    cursor[3] = head;
    subs[3].id = 3;
    late_registered = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    check(pthread_create(&st[3], NULL, subscriber, &subs[3]) == 0, "late subscriber");

    pthread_join(pt, NULL);
    for (int i = 0; i < MAXSUB; i++)
        pthread_join(st[i], NULL);

    long full_sum = 0, full_xr = 0, late_sum = 0;
    for (long s = 0; s < MSGS; s++) {
        full_sum += value(s);
        full_xr ^= value(s);
        if (s >= LATE_AT)
            late_sum += value(s);
    }
    for (int i = 0; i < 3; i++) {
        check(subs[i].in_order, "in order");
        check(subs[i].count == MSGS && subs[i].sum == full_sum && subs[i].xr == full_xr, "full stream");
    }
    check(subs[3].in_order && subs[3].first_seq == LATE_AT, "late subscriber starts at join point");
    check(subs[3].count == MSGS - LATE_AT && subs[3].sum == late_sum, "late subscriber tail");
    for (int i = 0; i < MAXSUB; i++)
        printf("subscriber %d: first seq %ld, received %ld, sum %ld, weighted %ld\n", i, subs[i].first_seq,
               subs[i].count, subs[i].sum, subs[i].weighted);
    printf("early subscribers identical, late subscriber saw the suffix from %d\n", LATE_AT);
    return 0;
}
