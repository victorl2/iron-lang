/*
 * title: Five-stage pipeline with bounded queues and backpressure bound
 * topic: concurrency
 * covers: chain of bounded queues, stage threads, in-flight bound, FIFO preservation, sentinel shutdown
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

enum { STAGES = 5, CAP = 2, ITEMS = 300 };
#define NQ (STAGES + 1)
#define DONE (-1L)

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t nf, ne;
    long buf[CAP];
    int head, cnt, high;
} Q;

static Q qs[NQ];
static pthread_mutex_t fl_mu = PTHREAD_MUTEX_INITIALIZER;
static long inflight, max_inflight;
static long outputs[ITEMS];
static int nout;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void put(Q *q, long v) {
    pthread_mutex_lock(&q->mu);
    while (q->cnt == CAP)
        pthread_cond_wait(&q->nf, &q->mu);
    q->buf[(q->head + q->cnt++) % CAP] = v;
    if (q->cnt > q->high)
        q->high = q->cnt;
    pthread_cond_signal(&q->ne);
    pthread_mutex_unlock(&q->mu);
}

static long get(Q *q) {
    pthread_mutex_lock(&q->mu);
    while (q->cnt == 0)
        pthread_cond_wait(&q->ne, &q->mu);
    long v = q->buf[q->head];
    q->head = (q->head + 1) % CAP;
    q->cnt--;
    pthread_cond_signal(&q->nf);
    pthread_mutex_unlock(&q->mu);
    return v;
}

static long xf(int stage, long v) {
    switch (stage) {
    case 0: return v * 3 + 1;
    case 1: return v ^ 0x55;
    case 2: return (v * v) % 10007;
    case 3: return v + stage * 100;
    default: return v * 2 - 5;
    }
}

static void *stage_thread(void *arg) {
    int s = (int)(long)arg;
    for (;;) {
        long v = get(&qs[s]);
        if (v == DONE) {
            put(&qs[s + 1], DONE);
            return NULL;
        }
        long r = xf(s, v);
        for (int i = 0; i < (s == 2 ? 30 : 0); i++)
            sched_yield(); /* a deliberately slow middle stage forces backpressure upstream */
        put(&qs[s + 1], r);
    }
}

static void *source(void *arg) {
    for (long i = 1; i <= ITEMS; i++) {
        pthread_mutex_lock(&fl_mu);
        inflight++;
        if (inflight > max_inflight)
            max_inflight = inflight;
        pthread_mutex_unlock(&fl_mu);
        put(&qs[0], i);
    }
    put(&qs[0], DONE);
    return NULL;
}

static void *sink(void *arg) {
    for (;;) {
        long v = get(&qs[STAGES]);
        if (v == DONE)
            return NULL;
        outputs[nout++] = v;
        pthread_mutex_lock(&fl_mu);
        inflight--;
        pthread_mutex_unlock(&fl_mu);
    }
}

int main(void) {
    for (int i = 0; i < NQ; i++) {
        pthread_mutex_init(&qs[i].mu, NULL);
        pthread_cond_init(&qs[i].nf, NULL);
        pthread_cond_init(&qs[i].ne, NULL);
    }
    pthread_t src, snk, st[STAGES];
    check(pthread_create(&snk, NULL, sink, NULL) == 0, "sink");
    for (long i = 0; i < STAGES; i++)
        check(pthread_create(&st[i], NULL, stage_thread, (void *)i) == 0, "stage");
    check(pthread_create(&src, NULL, source, NULL) == 0, "source");
    pthread_join(src, NULL);
    for (int i = 0; i < STAGES; i++)
        pthread_join(st[i], NULL);
    pthread_join(snk, NULL);

    check(nout == ITEMS, "all items reached the sink");
    long chk = 0;
    for (int i = 0; i < ITEMS; i++) {
        long v = i + 1;
        for (int s = 0; s < STAGES; s++)
            v = xf(s, v);
        check(outputs[i] == v, "FIFO order and value at position");
        chk = (chk * 31 + outputs[i]) % 1000000007L;
    }
    long bound = (long)NQ * CAP + STAGES + 2;
    check(max_inflight <= bound, "in-flight items bounded by queue capacities");
    for (int i = 0; i < NQ; i++) {
        check(qs[i].high <= CAP, "queue capacity respected");
        pthread_mutex_destroy(&qs[i].mu);
        pthread_cond_destroy(&qs[i].nf);
        pthread_cond_destroy(&qs[i].ne);
    }
    check(inflight == 0, "nothing left in flight");
    printf("stages %d queue capacity %d items %d\n", STAGES, CAP, ITEMS);
    printf("first outputs %ld %ld %ld %ld\n", outputs[0], outputs[1], outputs[2], outputs[3]);
    printf("last output %ld\n", outputs[ITEMS - 1]);
    printf("chain hash %ld\n", chk);
    printf("in-flight bound respected: yes\n");
    return 0;
}
