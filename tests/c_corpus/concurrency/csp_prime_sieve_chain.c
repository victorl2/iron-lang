/*
 * title: CSP prime sieve as a chain of processes
 * topic: concurrency
 * covers: CSP, unbuffered rendezvous channels, dynamically growing process pipeline, close propagation
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* Rendezvous channel: send returns only after the receiver has taken the value. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int state; /* 0 empty, 1 value offered, 2 value taken */
    int closed;
    int value;
} Chan;

static void chan_init(Chan *c) {
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->state = 0;
    c->closed = 0;
}

static void chan_send(Chan *c, int v) {
    pthread_mutex_lock(&c->mu);
    while (c->state != 0)
        pthread_cond_wait(&c->cv, &c->mu);
    c->value = v;
    c->state = 1;
    pthread_cond_broadcast(&c->cv);
    while (c->state != 2)
        pthread_cond_wait(&c->cv, &c->mu);
    c->state = 0;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

static void chan_close(Chan *c) {
    pthread_mutex_lock(&c->mu);
    while (c->state != 0)
        pthread_cond_wait(&c->cv, &c->mu);
    c->closed = 1;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

/* returns 1 and stores a value, or 0 when the channel is closed and drained */
static int chan_recv(Chan *c, int *out) {
    pthread_mutex_lock(&c->mu);
    while (c->state != 1 && !c->closed)
        pthread_cond_wait(&c->cv, &c->mu);
    if (c->state == 1) {
        *out = c->value;
        c->state = 2;
        pthread_cond_broadcast(&c->cv);
        pthread_mutex_unlock(&c->mu);
        return 1;
    }
    pthread_mutex_unlock(&c->mu);
    return 0;
}

enum { LIMIT = 300, MAXSTAGES = 80 };

static int primes[MAXSTAGES], nprimes;
static pthread_t stage_thread[MAXSTAGES + 2];
static Chan chans[MAXSTAGES + 2];
static int nstages;
static pthread_mutex_t stage_mu = PTHREAD_MUTEX_INITIALIZER;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *generator(void *arg) {
    Chan *out = arg;
    for (int n = 2; n <= LIMIT; n++)
        chan_send(out, n);
    chan_close(out);
    return NULL;
}

/* Only the stage that owns the newest channel appends to primes[]; earlier stages never do. */
static void *filter(void *arg) {
    int idx = (int)(long)arg;
    Chan *in = &chans[idx];
    int p;
    if (!chan_recv(in, &p))
        return NULL;
    primes[nprimes++] = p; /* first value through a stage is prime; stages run one at a time here */
    Chan *out = NULL;
    int v;
    while (chan_recv(in, &v)) {
        if (v % p == 0)
            continue;
        if (!out) { /* lazily grow the pipeline */
            pthread_mutex_lock(&stage_mu);
            int next_idx = ++nstages;
            pthread_mutex_unlock(&stage_mu);
            check(next_idx < MAXSTAGES, "stage limit");
            chan_init(&chans[next_idx]);
            out = &chans[next_idx];
            check(pthread_create(&stage_thread[next_idx], NULL, filter, (void *)(long)next_idx) == 0, "spawn");
        }
        chan_send(out, v);
    }
    if (out)
        chan_close(out);
    return NULL;
}

int main(void) {
    chan_init(&chans[0]);
    nstages = 0;
    pthread_t gen;
    check(pthread_create(&gen, NULL, generator, &chans[0]) == 0, "gen");
    check(pthread_create(&stage_thread[0], NULL, filter, (void *)0L) == 0, "first filter");
    pthread_join(gen, NULL);
    /* stage i creates stage i+1 before it returns, so after joining i the count is final for i+1 */
    for (int i = 0;; i++) {
        pthread_join(stage_thread[i], NULL);
        pthread_mutex_lock(&stage_mu);
        int more = i + 1 <= nstages;
        pthread_mutex_unlock(&stage_mu);
        if (!more)
            break;
    }

    /* reference sieve */
    char comp[LIMIT + 1] = {0};
    int expect[LIMIT], en = 0;
    for (int i = 2; i <= LIMIT; i++) {
        if (comp[i])
            continue;
        expect[en++] = i;
        for (int j = i * i; j <= LIMIT; j += i)
            comp[j] = 1;
    }
    check(en == nprimes, "prime count");
    for (int i = 0; i < en; i++)
        check(primes[i] == expect[i], "prime order");
    printf("primes up to %d: %d found by a chain of %d filter processes\n", LIMIT, nprimes, nstages + 1);
    for (int i = 0; i < nprimes; i++)
        printf("%d%c", primes[i], (i % 15 == 14 || i == nprimes - 1) ? '\n' : ' ');
    return 0;
}
