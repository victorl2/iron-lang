/*
 * title: Batching queue with size limit, flush markers and close
 * topic: concurrency
 * covers: batcher thread, size-triggered and marker-triggered flush, batch worker pool, result slots by batch index
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { BATCH = 7, ITEMS = 200, MAXB = 128, NW = 3 };
#define FLUSH (-1L)
#define END (-2L)

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t nf, ne;
    long *buf;
    int cap, head, cnt;
} Q;

static Q inq, batchq;

typedef struct {
    long items[BATCH];
    int n;
    const char *why;
    long sum, xr;
} Batch;

static Batch batches[MAXB];
static int nbatches;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void q_init(Q *q, int cap) {
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->nf, NULL);
    pthread_cond_init(&q->ne, NULL);
    q->buf = malloc(sizeof(long) * (size_t)cap);
    check(q->buf != NULL, "alloc");
    q->cap = cap;
    q->head = q->cnt = 0;
}

static void q_free(Q *q) {
    free(q->buf);
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->nf);
    pthread_cond_destroy(&q->ne);
}

static void q_put(Q *q, long v) {
    pthread_mutex_lock(&q->mu);
    while (q->cnt == q->cap)
        pthread_cond_wait(&q->nf, &q->mu);
    q->buf[(q->head + q->cnt++) % q->cap] = v;
    pthread_cond_signal(&q->ne);
    pthread_mutex_unlock(&q->mu);
}

static long q_get(Q *q) {
    pthread_mutex_lock(&q->mu);
    while (q->cnt == 0)
        pthread_cond_wait(&q->ne, &q->mu);
    long v = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->cnt--;
    pthread_cond_signal(&q->nf);
    pthread_mutex_unlock(&q->mu);
    return v;
}

static int nflush_at[ITEMS];

static void *producer(void *arg) {
    for (long i = 0; i < ITEMS; i++) {
        q_put(&inq, i * 3 + 1);
        if (nflush_at[i])
            q_put(&inq, FLUSH);
    }
    q_put(&inq, END);
    return NULL;
}

static void emit(Batch *cur, const char *why) {
    if (cur->n == 0)
        return; /* flushing an empty batch is a no-op */
    cur->why = why;
    check(nbatches < MAXB, "batch space");
    batches[nbatches] = *cur;
    q_put(&batchq, nbatches);
    nbatches++;
    cur->n = 0;
}

static void *batcher(void *arg) {
    Batch cur = {{0}, 0, NULL, 0, 0};
    for (;;) {
        long v = q_get(&inq);
        if (v == END) {
            emit(&cur, "close");
            break;
        }
        if (v == FLUSH) {
            emit(&cur, "marker");
            continue;
        }
        cur.items[cur.n++] = v;
        if (cur.n == BATCH)
            emit(&cur, "full");
    }
    for (int i = 0; i < NW; i++)
        q_put(&batchq, END);
    return NULL;
}

static void *batch_worker(void *arg) {
    for (;;) {
        long b = q_get(&batchq);
        if (b == END)
            return NULL;
        Batch *x = &batches[b]; /* slot written before its index was enqueued */
        long s = 0, xr = 0;
        for (int i = 0; i < x->n; i++) {
            s += x->items[i];
            xr ^= x->items[i];
        }
        x->sum = s;
        x->xr = xr;
    }
}

int main(void) {
    unsigned s = 5150u;
    for (int i = 0; i < ITEMS; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        nflush_at[i] = (s % 9u == 0);
    }
    q_init(&inq, 4);
    q_init(&batchq, 4);
    pthread_t pt, bt, wt[NW];
    /* batchq has to hold every batch index until workers start, so start workers first */
    for (int i = 0; i < NW; i++)
        check(pthread_create(&wt[i], NULL, batch_worker, NULL) == 0, "worker");
    check(pthread_create(&bt, NULL, batcher, NULL) == 0, "batcher");
    check(pthread_create(&pt, NULL, producer, NULL) == 0, "producer");
    pthread_join(pt, NULL);
    pthread_join(bt, NULL);
    for (int i = 0; i < NW; i++)
        pthread_join(wt[i], NULL);

    /* Reference: replay the same stream sequentially. */
    long ref_items[ITEMS], nref = 0;
    int ref_sizes[MAXB], nref_b = 0, cur = 0;
    for (int i = 0; i < ITEMS; i++) {
        ref_items[nref++] = i * 3 + 1;
        cur++;
        if (cur == BATCH) {
            ref_sizes[nref_b++] = cur;
            cur = 0;
        }
        if (nflush_at[i] && cur > 0) {
            ref_sizes[nref_b++] = cur;
            cur = 0;
        }
    }
    if (cur > 0)
        ref_sizes[nref_b++] = cur;
    check(nref_b == nbatches, "batch count");
    long seen = 0, total = 0;
    int full = 0, marker = 0, close_b = 0;
    for (int b = 0; b < nbatches; b++) {
        check(batches[b].n == ref_sizes[b], "batch size");
        long s2 = 0;
        for (int i = 0; i < batches[b].n; i++) {
            check(batches[b].items[i] == ref_items[seen++], "batch content in stream order");
            s2 += batches[b].items[i];
        }
        check(batches[b].sum == s2, "worker computed sum");
        total += batches[b].sum;
        if (batches[b].why[0] == 'f')
            full++;
        else if (batches[b].why[0] == 'm')
            marker++;
        else
            close_b++;
    }
    check(seen == ITEMS, "every item batched once");
    for (int b = 0; b < 12; b++)
        printf("batch %2d size %d sum %4ld xor %4ld via %s\n", b, batches[b].n, batches[b].sum, batches[b].xr,
               batches[b].why);
    printf("batches %d (full %d, marker %d, close %d) items %ld total %ld\n", nbatches, full, marker, close_b, seen,
           total);
    q_free(&inq);
    q_free(&batchq);
    return 0;
}
