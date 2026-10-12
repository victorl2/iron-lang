/*
 * title: Go-style buffered channel with close, ok-flag receive and range loop
 * topic: concurrency
 * covers: buffered channel, close semantics, recv ok flag, send on closed error, blocked sender woken by close, len and cap
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum { SEND_OK, SEND_FULL, SEND_CLOSED } SendRes;
typedef enum { CLOSE_OK, CLOSE_ALREADY } CloseRes;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int *buf;
    int cap, head, cnt, closed;
    int blocked_senders;
} Chan;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static const char *send_name(SendRes r) {
    return r == SEND_OK ? "ok" : r == SEND_FULL ? "full" : "closed";
}

static Chan *chan_make(int cap) {
    Chan *c = calloc(1, sizeof *c);
    check(c != NULL, "alloc");
    c->buf = malloc(sizeof(int) * (size_t)cap);
    check(c->buf != NULL, "alloc");
    c->cap = cap;
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    return c;
}

static void chan_drop(Chan *c) {
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    free(c->buf);
    free(c);
}

static int chan_len(Chan *c) {
    pthread_mutex_lock(&c->mu);
    int n = c->cnt;
    pthread_mutex_unlock(&c->mu);
    return n;
}

static SendRes chan_send(Chan *c, int v) {
    pthread_mutex_lock(&c->mu);
    while (c->cnt == c->cap && !c->closed) {
        c->blocked_senders++;
        pthread_cond_broadcast(&c->cv);
        pthread_cond_wait(&c->cv, &c->mu);
        c->blocked_senders--;
    }
    if (c->closed) {
        pthread_mutex_unlock(&c->mu);
        return SEND_CLOSED;
    }
    c->buf[(c->head + c->cnt++) % c->cap] = v;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return SEND_OK;
}

static SendRes chan_try_send(Chan *c, int v) {
    pthread_mutex_lock(&c->mu);
    SendRes r = SEND_OK;
    if (c->closed)
        r = SEND_CLOSED;
    else if (c->cnt == c->cap)
        r = SEND_FULL;
    else {
        c->buf[(c->head + c->cnt++) % c->cap] = v;
        pthread_cond_broadcast(&c->cv);
    }
    pthread_mutex_unlock(&c->mu);
    return r;
}

/* Returns 1 with *v set, or 0 (ok == false) once the channel is closed and empty. */
static int chan_recv(Chan *c, int *v) {
    pthread_mutex_lock(&c->mu);
    while (c->cnt == 0 && !c->closed)
        pthread_cond_wait(&c->cv, &c->mu);
    if (c->cnt == 0) {
        *v = 0;
        pthread_mutex_unlock(&c->mu);
        return 0;
    }
    *v = c->buf[c->head];
    c->head = (c->head + 1) % c->cap;
    c->cnt--;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return 1;
}

static CloseRes chan_close(Chan *c) {
    pthread_mutex_lock(&c->mu);
    CloseRes r = c->closed ? CLOSE_ALREADY : CLOSE_OK;
    c->closed = 1;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return r;
}

typedef struct {
    Chan *c;
    SendRes res;
} BlockedSend;

static void *blocked_sender(void *arg) {
    BlockedSend *b = arg;
    b->res = chan_send(b->c, 99);
    return NULL;
}

typedef struct {
    Chan *c;
    int id;
    int n;
} Producer;

static void *producer(void *arg) {
    Producer *p = arg;
    for (int i = 0; i < p->n; i++)
        check(chan_send(p->c, p->id * 1000 + i) == SEND_OK, "send while open");
    return NULL;
}

static void *closer(void *arg) {
    /* closes after all producers have finished (they are joined by this thread) */
    void **a = arg;
    Chan *c = a[0];
    pthread_t *pt = a[1];
    int n = *(int *)a[2];
    for (int i = 0; i < n; i++)
        pthread_join(pt[i], NULL);
    chan_close(c);
    return NULL;
}

typedef struct {
    Chan *c;
    long n, sum;
} Consumer;

static void *consumer(void *arg) {
    Consumer *k = arg;
    int v;
    while (chan_recv(k->c, &v)) { /* for v := range ch */
        k->n++;
        k->sum += v;
    }
    return NULL;
}

int main(void) {
    Chan *c = chan_make(4);
    printf("len %d cap %d\n", chan_len(c), c->cap);
    for (int i = 1; i <= 4; i++)
        check(chan_try_send(c, i * 10) == SEND_OK, "fill");
    SendRes full = chan_try_send(c, 50);
    printf("fifth send: %s, len %d\n", send_name(full), chan_len(c));

    /* A sender blocked on the full channel is released with an error by close. */
    BlockedSend bs = {c, SEND_OK};
    pthread_t bt;
    check(pthread_create(&bt, NULL, blocked_sender, &bs) == 0, "blocked sender");
    pthread_mutex_lock(&c->mu);
    while (c->blocked_senders == 0)
        pthread_cond_wait(&c->cv, &c->mu);
    pthread_mutex_unlock(&c->mu);
    CloseRes c1 = chan_close(c);
    pthread_join(bt, NULL);
    CloseRes c2 = chan_close(c);
    printf("close: %s, second close: %s\n", c1 == CLOSE_OK ? "ok" : "already", c2 == CLOSE_OK ? "ok" : "already");
    printf("blocked sender result: %s\n", send_name(bs.res));
    check(bs.res == SEND_CLOSED && c1 == CLOSE_OK && c2 == CLOSE_ALREADY, "close semantics");
    SendRes after = chan_try_send(c, 1);
    printf("send after close: %s\n", send_name(after));
    printf("drain buffered values after close:");
    int v, ok;
    while ((ok = chan_recv(c, &v)))
        printf(" %d", v);
    printf(" | last recv ok=%d value=%d\n", ok, v);
    chan_drop(c);

    /* Many producers, many consumers, closed by a coordinator. */
    enum { NP = 3, NC = 3, PER = 400 };
    Chan *d = chan_make(5);
    pthread_t pt[NP], ct[NC], cl;
    Producer pr[NP];
    Consumer co[NC] = {{d, 0, 0}, {d, 0, 0}, {d, 0, 0}};
    int np = NP;
    void *args[3] = {d, pt, &np};
    for (int i = 0; i < NC; i++)
        check(pthread_create(&ct[i], NULL, consumer, &co[i]) == 0, "consumer");
    for (int i = 0; i < NP; i++) {
        pr[i].c = d;
        pr[i].id = i + 1;
        pr[i].n = PER;
        check(pthread_create(&pt[i], NULL, producer, &pr[i]) == 0, "producer");
    }
    check(pthread_create(&cl, NULL, closer, args) == 0, "closer");
    pthread_join(cl, NULL);
    for (int i = 0; i < NC; i++)
        pthread_join(ct[i], NULL);
    long n = 0, sum = 0, expect = 0;
    for (int i = 0; i < NC; i++) {
        n += co[i].n;
        sum += co[i].sum;
    }
    for (int p = 1; p <= NP; p++)
        for (int i = 0; i < PER; i++)
            expect += p * 1000 + i;
    check(n == NP * PER && sum == expect, "range loops saw everything");
    printf("range loops: %ld values, sum %ld\n", n, sum);
    chan_drop(d);
    return 0;
}
