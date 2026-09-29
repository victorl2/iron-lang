/*
 * title: CSP guarded alternation server
 * topic: concurrency
 * covers: CSP alt/select, guards, rendezvous channels, server process owning a bounded buffer
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* All channels of the network share one lock and one condition so that alt can wait on several. */
static pthread_mutex_t net_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t net_cv = PTHREAD_COND_INITIALIZER;

typedef struct {
    int state; /* 0 empty, 1 offered, 2 taken */
    long value;
} Chan;

static void send_on(Chan *c, long v) {
    pthread_mutex_lock(&net_mu);
    while (c->state != 0)
        pthread_cond_wait(&net_cv, &net_mu);
    c->value = v;
    c->state = 1;
    pthread_cond_broadcast(&net_cv);
    while (c->state != 2)
        pthread_cond_wait(&net_cv, &net_mu);
    c->state = 0;
    pthread_cond_broadcast(&net_cv);
    pthread_mutex_unlock(&net_mu);
}

static long recv_on(Chan *c) {
    pthread_mutex_lock(&net_mu);
    while (c->state != 1)
        pthread_cond_wait(&net_cv, &net_mu);
    long v = c->value;
    c->state = 2;
    pthread_cond_broadcast(&net_cv);
    pthread_mutex_unlock(&net_mu);
    return v;
}

/* alt: wait until one of the enabled channels has an offer; lowest index wins. */
static int alt(Chan **chans, const int *enabled, int n, long *out) {
    pthread_mutex_lock(&net_mu);
    for (;;) {
        for (int i = 0; i < n; i++)
            if (enabled[i] && chans[i]->state == 1) {
                *out = chans[i]->value;
                chans[i]->state = 2;
                pthread_cond_broadcast(&net_cv);
                pthread_mutex_unlock(&net_mu);
                return i;
            }
        pthread_cond_wait(&net_cv, &net_mu);
    }
}

enum { PRODUCERS = 3, CONSUMERS = 3, ITEMS = 120, CAP = 4 };

static Chan put_ch, get_ch, quit_ch;
static Chan reply_ch[CONSUMERS];

static long served_puts, served_gets;
static int max_fill;
static int guard_violations;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *server(void *arg) {
    (void)arg;
    long buf[CAP];
    int head = 0, count = 0;
    Chan *chans[3] = {&put_ch, &get_ch, &quit_ch};
    for (;;) {
        int enabled[3] = {count < CAP, count > 0, 1};
        long v;
        int which = alt(chans, enabled, 3, &v);
        if (which == 0) {
            if (count >= CAP)
                guard_violations++;
            buf[(head + count) % CAP] = v;
            count++;
            served_puts++;
            if (count > max_fill)
                max_fill = count;
        } else if (which == 1) {
            if (count <= 0)
                guard_violations++;
            long item = buf[head];
            head = (head + 1) % CAP;
            count--;
            served_gets++;
            send_on(&reply_ch[v], item); /* v carries the consumer id */
        } else {
            break;
        }
    }
    return NULL;
}

static void *producer(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < ITEMS; i++)
        send_on(&put_ch, id * 1000 + i);
    return NULL;
}

static long consumed_sum[CONSUMERS], consumed_n[CONSUMERS];

static void *consumer(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < ITEMS; i++) {
        send_on(&get_ch, id);
        long item = recv_on(&reply_ch[id]);
        consumed_sum[id] += item;
        consumed_n[id]++;
    }
    return NULL;
}

int main(void) {
    pthread_t sv, pt[PRODUCERS], ct[CONSUMERS];
    check(pthread_create(&sv, NULL, server, NULL) == 0, "server");
    for (long i = 0; i < PRODUCERS; i++)
        check(pthread_create(&pt[i], NULL, producer, (void *)i) == 0, "producer");
    for (long i = 0; i < CONSUMERS; i++)
        check(pthread_create(&ct[i], NULL, consumer, (void *)i) == 0, "consumer");
    for (int i = 0; i < PRODUCERS; i++)
        pthread_join(pt[i], NULL);
    for (int i = 0; i < CONSUMERS; i++)
        pthread_join(ct[i], NULL);
    send_on(&quit_ch, 0);
    pthread_join(sv, NULL);

    long total = 0, expect = 0, n = 0;
    for (int i = 0; i < CONSUMERS; i++) {
        total += consumed_sum[i];
        n += consumed_n[i];
    }
    for (long p = 0; p < PRODUCERS; p++)
        for (int i = 0; i < ITEMS; i++)
            expect += p * 1000 + i;
    printf("puts served=%ld gets served=%ld\n", served_puts, served_gets);
    printf("items consumed=%ld sum=%ld\n", n, total);
    printf("buffer capacity %d never exceeded: %s\n", CAP, max_fill <= CAP ? "yes" : "no");
    check(served_puts == PRODUCERS * ITEMS && served_gets == CONSUMERS * ITEMS, "served");
    check(total == expect, "sum of items");
    check(guard_violations == 0 && max_fill <= CAP, "guards held");
    return 0;
}
