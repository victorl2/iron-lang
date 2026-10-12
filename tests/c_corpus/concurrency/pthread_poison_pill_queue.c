/*
 * title: Multi-producer queue drained by consumers using poison pills
 * topic: concurrency
 * covers: unbounded linked queue, poison pills, one pill per consumer, per-producer ordering check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { P = 3, C = 3, PER = 400 };

typedef struct Msg {
    int producer;
    int seq; /* -1 marks a poison pill */
    struct Msg *next;
} Msg;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Msg *head, *tail;
} Queue;

static Queue q = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL};

typedef struct {
    int id;
    long got;
    int last_seq[P]; /* per producer: FIFO from a single producer must be preserved for a consumer */
    int order_violations;
    long seq_sum;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void enqueue(int producer, int seq) {
    Msg *m = malloc(sizeof *m);
    check(m != NULL, "malloc");
    m->producer = producer;
    m->seq = seq;
    m->next = NULL;
    pthread_mutex_lock(&q.mu);
    if (q.tail)
        q.tail->next = m;
    else
        q.head = m;
    q.tail = m;
    pthread_cond_signal(&q.cv);
    pthread_mutex_unlock(&q.mu);
}

static void *producer(void *p) {
    Arg *a = p;
    for (int i = 0; i < PER; i++)
        enqueue(a->id, i);
    return NULL;
}

static void *consumer(void *p) {
    Arg *a = p;
    for (int i = 0; i < P; i++)
        a->last_seq[i] = -1;
    for (;;) {
        pthread_mutex_lock(&q.mu);
        while (!q.head)
            pthread_cond_wait(&q.cv, &q.mu);
        Msg *m = q.head;
        q.head = m->next;
        if (!q.head)
            q.tail = NULL;
        pthread_mutex_unlock(&q.mu);
        if (m->seq < 0) {
            free(m);
            return NULL; /* pill: this consumer is done */
        }
        if (m->seq <= a->last_seq[m->producer])
            a->order_violations++;
        a->last_seq[m->producer] = m->seq;
        a->got++;
        a->seq_sum += m->seq;
        free(m);
    }
}

int main(void) {
    Arg pa[P], ca[C];
    pthread_t pt[P], ct[C];
    for (int i = 0; i < C; i++) {
        ca[i] = (Arg){i, 0, {0}, 0, 0};
        check(pthread_create(&ct[i], NULL, consumer, &ca[i]) == 0, "create c");
    }
    for (int i = 0; i < P; i++) {
        pa[i] = (Arg){i, 0, {0}, 0, 0};
        check(pthread_create(&pt[i], NULL, producer, &pa[i]) == 0, "create p");
    }
    for (int i = 0; i < P; i++)
        pthread_join(pt[i], NULL);
    for (int i = 0; i < C; i++)
        enqueue(-1, -1); /* one pill per consumer, queued after all real work */
    long got = 0, seq_sum = 0;
    int bad = 0;
    for (int i = 0; i < C; i++) {
        pthread_join(ct[i], NULL);
        got += ca[i].got;
        seq_sum += ca[i].seq_sum;
        bad += ca[i].order_violations;
    }
    check(got == (long)P * PER, "all messages consumed");
    check(seq_sum == (long)P * PER * (PER - 1) / 2, "sequence numbers sum");
    check(bad == 0, "per-producer order preserved within each consumer");
    check(q.head == NULL, "queue empty");
    printf("messages=%ld seq_sum=%ld\n", got, seq_sum);
    printf("order violations=%d\n", bad);
    printf("consumers terminated by pills: %d\n", C);
    return 0;
}
