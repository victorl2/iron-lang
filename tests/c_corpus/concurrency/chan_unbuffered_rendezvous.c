/*
 * title: Unbuffered rendezvous channel with completion ordering proof
 * topic: concurrency
 * covers: synchronous channel, sender blocks until taken, logical clock happens-before, try_send, request/response
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    long slot;
    int has_item;
    long issued, taken;
    int receivers_waiting;
    long clock;
} Chan;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void chan_init(Chan *c) {
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->slot = 0;
    c->has_item = 0;
    c->issued = c->taken = 0;
    c->receivers_waiting = 0;
    c->clock = 0;
}

static void chan_destroy(Chan *c) {
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
}

/* Blocks until a receiver took the item. Returns the logical time at which send returned. */
static long chan_send(Chan *c, long v) {
    pthread_mutex_lock(&c->mu);
    while (c->has_item)
        pthread_cond_wait(&c->cv, &c->mu);
    c->slot = v;
    c->has_item = 1;
    long ticket = ++c->issued;
    pthread_cond_broadcast(&c->cv);
    while (c->taken < ticket)
        pthread_cond_wait(&c->cv, &c->mu);
    long t = ++c->clock;
    pthread_mutex_unlock(&c->mu);
    return t;
}

static long chan_recv(Chan *c, long *taken_at) {
    pthread_mutex_lock(&c->mu);
    c->receivers_waiting++;
    pthread_cond_broadcast(&c->cv);
    while (!c->has_item)
        pthread_cond_wait(&c->cv, &c->mu);
    c->receivers_waiting--;
    long v = c->slot;
    c->has_item = 0;
    c->taken++;
    *taken_at = ++c->clock;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return v;
}

/* Succeeds only if a receiver is already waiting and no other item is pending. */
static int chan_try_send(Chan *c, long v) {
    pthread_mutex_lock(&c->mu);
    if (c->receivers_waiting == 0 || c->has_item) {
        pthread_mutex_unlock(&c->mu);
        return 0;
    }
    c->slot = v;
    c->has_item = 1;
    c->issued++;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return 1;
}

enum { NS = 3, PER = 100, NR = 2 };
static Chan ch;
static long sent_ret[NS * 1000 + PER];  /* logical time when send returned, by value */
static long taken_time[NS * 1000 + PER];
static long recv_count[NR], recv_sum[NR];

static void *sender(void *arg) {
    long id = (long)arg;
    for (long i = 0; i < PER; i++)
        sent_ret[id * 1000 + i] = chan_send(&ch, id * 1000 + i);
    return NULL;
}

static void *receiver(void *arg) {
    long id = (long)arg;
    for (;;) {
        long at;
        long v = chan_recv(&ch, &at);
        if (v < 0)
            return NULL;
        taken_time[v] = at;
        recv_count[id]++;
        recv_sum[id] += v;
    }
}

static Chan req, resp;

static void *server(void *arg) {
    for (;;) {
        long at;
        long n = chan_recv(&req, &at);
        if (n == 0)
            return NULL;
        chan_send(&resp, n * n + 1);
    }
}

static void *late_receiver(void *arg) {
    long at;
    long *out = arg;
    *out = chan_recv(&ch, &at);
    return NULL;
}

int main(void) {
    chan_init(&ch);
    chan_init(&req);
    chan_init(&resp);

    /* try_send with no receiver fails; with a parked receiver it succeeds. */
    check(!chan_try_send(&ch, 5), "try_send without receiver");
    pthread_t lr;
    long got = 0;
    check(pthread_create(&lr, NULL, late_receiver, &got) == 0, "late receiver");
    for (;;) {
        pthread_mutex_lock(&ch.mu);
        int w = ch.receivers_waiting;
        pthread_mutex_unlock(&ch.mu);
        if (w)
            break;
        sched_yield();
    }
    int ok = chan_try_send(&ch, 4242);
    pthread_join(lr, NULL);
    check(ok && got == 4242, "try_send reached the parked receiver");
    printf("try_send with no receiver: failed; with parked receiver: delivered %ld\n", got);

    pthread_t st[NS], rt[NR];
    for (long i = 0; i < NR; i++)
        check(pthread_create(&rt[i], NULL, receiver, (void *)i) == 0, "receiver");
    for (long i = 0; i < NS; i++)
        check(pthread_create(&st[i], NULL, sender, (void *)i) == 0, "sender");
    for (int i = 0; i < NS; i++)
        pthread_join(st[i], NULL);
    for (int i = 0; i < NR; i++)
        chan_send(&ch, -1); /* one stop value per receiver */
    for (int i = 0; i < NR; i++)
        pthread_join(rt[i], NULL);

    long total = 0, sum = 0, expect = 0;
    for (long id = 0; id < NS; id++)
        for (long i = 0; i < PER; i++) {
            long v = id * 1000 + i;
            check(taken_time[v] > 0 && taken_time[v] < sent_ret[v], "item was taken before its send returned");
            expect += v;
        }
    for (int i = 0; i < NR; i++) {
        total += recv_count[i];
        sum += recv_sum[i];
    }
    check(total == NS * PER && sum == expect, "all items delivered exactly once");
    printf("rendezvous: %ld items, sum %ld, every send returned after its receive\n", total, sum);

    pthread_t sv;
    check(pthread_create(&sv, NULL, server, NULL) == 0, "server");
    long acc = 0;
    for (long n = 1; n <= 40; n++) {
        chan_send(&req, n);
        long at;
        long r = chan_recv(&resp, &at);
        check(r == n * n + 1, "response value");
        acc += r;
    }
    chan_send(&req, 0);
    pthread_join(sv, NULL);
    printf("request/response: 40 calls, sum of replies %ld\n", acc);
    chan_destroy(&ch);
    chan_destroy(&req);
    chan_destroy(&resp);
    return 0;
}
