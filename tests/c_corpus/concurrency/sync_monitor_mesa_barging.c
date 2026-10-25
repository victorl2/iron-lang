/*
 * title: Mesa monitor with named condition queues and barging demo
 * topic: concurrency
 * covers: monitor, Mesa signal semantics, condition queues, while-loop rechecks, signal stealing, drain condition
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { CAP = 4 };

/* One monitor lock, three condition queues: not_full, not_empty and drained. */
typedef struct {
    pthread_mutex_t mon;
    pthread_cond_t not_full, not_empty, drained;
    int items[CAP];
    int head, count;
    long puts, gets;
    int producers_active;
    int rechecks;      /* wakeups that found the predicate false again */
    int waiting_empty; /* consumers blocked on not_empty */
} Monitor;

static Monitor m = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER,
                    PTHREAD_COND_INITIALIZER,  {0},                      0,
                    0,                         0,                        0,
                    0,                         0,                        0};

static void put_locked(int v) {
    m.items[(m.head + m.count) % CAP] = v;
    m.count++;
    m.puts++;
}
static int get_locked(void) {
    int v = m.items[m.head];
    m.head = (m.head + 1) % CAP;
    m.count--;
    m.gets++;
    return v;
}

static void mon_put(int v) {
    pthread_mutex_lock(&m.mon);
    while (m.count == CAP)
        pthread_cond_wait(&m.not_full, &m.mon);
    put_locked(v);
    pthread_cond_signal(&m.not_empty);
    pthread_mutex_unlock(&m.mon);
}

static int mon_get(void) {
    pthread_mutex_lock(&m.mon);
    m.waiting_empty++;
    int woke = 0;
    while (m.count == 0) {
        if (woke)
            m.rechecks++; /* Mesa: being signalled does not guarantee the condition still holds */
        pthread_cond_wait(&m.not_empty, &m.mon);
        woke = 1;
    }
    m.waiting_empty--;
    int v = get_locked();
    pthread_cond_signal(&m.not_full);
    if (m.count == 0)
        pthread_cond_broadcast(&m.drained);
    pthread_mutex_unlock(&m.mon);
    return v;
}

static void mon_wait_drained(void) {
    pthread_mutex_lock(&m.mon);
    while (m.producers_active > 0 || m.count > 0)
        pthread_cond_wait(&m.drained, &m.mon);
    pthread_mutex_unlock(&m.mon);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { P = 3, C = 3, PER = 400 };

static long consumed_sum[C];
static int consumed_n[C];

static void *producer(void *p) {
    int id = (int)(intptr_t)p;
    for (int i = 0; i < PER; i++)
        mon_put(id * 1000 + i);
    pthread_mutex_lock(&m.mon);
    if (--m.producers_active == 0)
        pthread_cond_broadcast(&m.drained);
    pthread_mutex_unlock(&m.mon);
    return NULL;
}
static void *consumer(void *p) {
    int id = (int)(intptr_t)p;
    for (;;) {
        int v = mon_get();
        if (v < 0)
            return NULL;
        consumed_sum[id] += v;
        consumed_n[id]++;
    }
}

static int stolen_item;
static atomic_int demo_got;
static void *demo_consumer(void *p) {
    (void)p;
    int v = mon_get();
    atomic_store(&demo_got, v);
    return NULL;
}

int main(void) {
    /* Demo: the signaller keeps the monitor and steals the item it just signalled for. */
    pthread_t dc;
    pthread_create(&dc, NULL, demo_consumer, NULL);
    for (;;) {
        pthread_mutex_lock(&m.mon);
        int w = m.waiting_empty;
        pthread_mutex_unlock(&m.mon);
        if (w == 1)
            break;
        sched_yield();
    }
    pthread_mutex_lock(&m.mon);
    put_locked(111);
    pthread_cond_signal(&m.not_empty); /* Mesa: signaller continues to run inside the monitor */
    stolen_item = get_locked();        /* barging: take the item before the woken consumer can */
    pthread_mutex_unlock(&m.mon);
    /* Consumer wakes, finds the buffer empty again and must wait a second time. */
    for (;;) {
        pthread_mutex_lock(&m.mon);
        int r = m.rechecks;
        pthread_mutex_unlock(&m.mon);
        if (r >= 1)
            break;
        sched_yield();
    }
    check(atomic_load(&demo_got) == 0, "consumer must not have an item yet");
    mon_put(222);
    pthread_join(dc, NULL);
    printf("demo: signaller stole %d, woken consumer ended up with %d, rechecked at least once: %s\n", stolen_item,
           atomic_load(&demo_got), m.rechecks >= 1 ? "yes" : "no");
    check(stolen_item == 111 && atomic_load(&demo_got) == 222, "barging demo");

    /* Stress with three condition queues. */
    m.producers_active = P;
    pthread_t pt[P], ct[C];
    for (int i = 0; i < C; i++)
        pthread_create(&ct[i], NULL, consumer, (void *)(intptr_t)i);
    for (int i = 0; i < P; i++)
        pthread_create(&pt[i], NULL, producer, (void *)(intptr_t)i);
    mon_wait_drained();
    for (int i = 0; i < C; i++) /* one poison item per consumer */
        mon_put(-1);
    for (int i = 0; i < P; i++)
        pthread_join(pt[i], NULL);
    for (int i = 0; i < C; i++)
        pthread_join(ct[i], NULL);

    long sum = 0, expect = 0;
    int n = 0;
    for (int i = 0; i < C; i++) {
        sum += consumed_sum[i];
        n += consumed_n[i];
    }
    for (int id = 0; id < P; id++)
        for (int i = 0; i < PER; i++)
            expect += id * 1000 + i;
    printf("stress: consumed %d items, sum %ld (expected %ld)\n", n, sum, expect);
    printf("monitor totals: puts=%ld gets=%ld (includes demo and %d poison items)\n", m.puts, m.gets, C);
    check(n == P * PER && sum == expect, "stress totals");
    check(m.puts == m.gets && m.count == 0, "balanced");
    return 0;
}
