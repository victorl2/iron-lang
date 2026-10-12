/*
 * title: Fan-out by key affinity and fan-in through a result queue
 * topic: concurrency
 * covers: dispatcher, per-worker queues, key partitioning, single collector, per-key aggregation, close propagation
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { K = 4, KEYS = 23, EVENTS = 4000, CAP = 16 };

typedef struct {
    int key;
    long amount;
} Event;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t nf, ne;
    Event *buf;
    int cap, head, cnt, closed;
} Chan;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void chan_init(Chan *c, int cap) {
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->nf, NULL);
    pthread_cond_init(&c->ne, NULL);
    c->buf = malloc(sizeof(Event) * (size_t)cap);
    check(c->buf != NULL, "alloc");
    c->cap = cap;
    c->head = c->cnt = c->closed = 0;
}

static void chan_free(Chan *c) {
    free(c->buf);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->nf);
    pthread_cond_destroy(&c->ne);
}

static void chan_send(Chan *c, Event e) {
    pthread_mutex_lock(&c->mu);
    while (c->cnt == c->cap)
        pthread_cond_wait(&c->nf, &c->mu);
    c->buf[(c->head + c->cnt++) % c->cap] = e;
    pthread_cond_signal(&c->ne);
    pthread_mutex_unlock(&c->mu);
}

static int chan_recv(Chan *c, Event *e) {
    pthread_mutex_lock(&c->mu);
    while (c->cnt == 0 && !c->closed)
        pthread_cond_wait(&c->ne, &c->mu);
    if (c->cnt == 0) {
        pthread_mutex_unlock(&c->mu);
        return 0;
    }
    *e = c->buf[c->head];
    c->head = (c->head + 1) % c->cap;
    c->cnt--;
    pthread_cond_signal(&c->nf);
    pthread_mutex_unlock(&c->mu);
    return 1;
}

static void chan_close(Chan *c) {
    pthread_mutex_lock(&c->mu);
    c->closed = 1;
    pthread_cond_broadcast(&c->ne);
    pthread_mutex_unlock(&c->mu);
}

static Chan in[K], results;
static Event events[EVENTS];

typedef struct {
    int id;
    long count[KEYS], sum[KEYS], maxv[KEYS];
    int foreign;
} WorkerState;

static WorkerState ws[K];

/* The result message reuses Event: key = worker id, amount = number of distinct keys owned. */
static void *worker(void *arg) {
    WorkerState *w = arg;
    Event e;
    while (chan_recv(&in[w->id], &e)) {
        if (e.key % K != w->id)
            w->foreign++;
        w->count[e.key]++;
        w->sum[e.key] += e.amount;
        if (e.amount > w->maxv[e.key])
            w->maxv[e.key] = e.amount;
    }
    int owned = 0;
    for (int k = 0; k < KEYS; k++)
        if (w->count[k])
            owned++;
    Event done = {w->id, owned};
    chan_send(&results, done);
    return NULL;
}

static void *dispatcher(void *arg) {
    for (int i = 0; i < EVENTS; i++)
        chan_send(&in[events[i].key % K], events[i]);
    for (int i = 0; i < K; i++)
        chan_close(&in[i]);
    return NULL;
}

int main(void) {
    unsigned s = 20240607u;
    long ref_count[KEYS] = {0}, ref_sum[KEYS] = {0};
    for (int i = 0; i < EVENTS; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        events[i].key = (int)(s % KEYS);
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        events[i].amount = (long)(s % 1000u);
        ref_count[events[i].key]++;
        ref_sum[events[i].key] += events[i].amount;
    }
    for (int i = 0; i < K; i++) {
        chan_init(&in[i], CAP);
        ws[i].id = i;
    }
    chan_init(&results, K);
    pthread_t wt[K], dt;
    for (int i = 0; i < K; i++)
        check(pthread_create(&wt[i], NULL, worker, &ws[i]) == 0, "create worker");
    check(pthread_create(&dt, NULL, dispatcher, NULL) == 0, "create dispatcher");

    /* fan-in: collect one summary per worker; order of arrival varies, so index by worker id */
    long owned_by[K] = {0};
    for (int i = 0; i < K; i++) {
        Event e;
        check(chan_recv(&results, &e), "summary");
        owned_by[e.key] = e.amount;
    }
    pthread_join(dt, NULL);
    for (int i = 0; i < K; i++)
        pthread_join(wt[i], NULL);

    long total_count = 0, total_sum = 0;
    for (int k = 0; k < KEYS; k++) {
        int w = k % K;
        check(ws[w].count[k] == ref_count[k], "count per key");
        check(ws[w].sum[k] == ref_sum[k], "sum per key");
        for (int o = 0; o < K; o++)
            if (o != w)
                check(ws[o].count[k] == 0, "key handled by exactly one worker");
        total_count += ws[w].count[k];
        total_sum += ws[w].sum[k];
    }
    for (int i = 0; i < K; i++)
        check(ws[i].foreign == 0, "no foreign keys");
    check(total_count == EVENTS, "event count");
    printf("events %d keys %d workers %d\n", EVENTS, KEYS, K);
    for (int i = 0; i < K; i++)
        printf("worker %d owns %ld keys\n", i, owned_by[i]);
    for (int k = 0; k < 8; k++)
        printf("key %2d count %ld sum %ld max %ld\n", k, ref_count[k], ws[k % K].sum[k], ws[k % K].maxv[k]);
    printf("total count %ld total sum %ld\n", total_count, total_sum);
    for (int i = 0; i < K; i++)
        chan_free(&in[i]);
    chan_free(&results);
    return 0;
}
