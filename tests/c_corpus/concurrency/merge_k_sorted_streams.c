/*
 * title: K-way merge of sorted streams arriving through bounded queues
 * topic: concurrency
 * covers: producer per stream, bounded queues, heap merge on the consumer, tie-break by stream, end markers
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { K = 5, CAP = 3, MAXLEN = 400 };
#define END (-1L)

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t nf, ne;
    long buf[CAP];
    int head, cnt;
} Q;

static Q qs[K];
static int lens[K];
static long streams[K][MAXLEN];

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

static void *producer(void *arg) {
    int s = (int)(long)arg;
    for (int i = 0; i < lens[s]; i++)
        put(&qs[s], streams[s][i]);
    put(&qs[s], END);
    return NULL;
}

typedef struct {
    long v;
    int s;
} Head;

static int hless(Head a, Head b) {
    return a.v != b.v ? a.v < b.v : a.s < b.s;
}

static Head heap[K];
static int hn;

static void hpush(Head h) {
    int i = hn++;
    while (i > 0 && hless(h, heap[(i - 1) / 2])) {
        heap[i] = heap[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    heap[i] = h;
}

static Head hpop(void) {
    Head top = heap[0], last = heap[--hn];
    int i = 0;
    while (2 * i + 1 < hn) {
        int c = 2 * i + 1;
        if (c + 1 < hn && hless(heap[c + 1], heap[c]))
            c++;
        if (!hless(heap[c], last))
            break;
        heap[i] = heap[c];
        i = c;
    }
    if (hn > 0)
        heap[i] = last;
    return top;
}

typedef struct {
    long v;
    int s, idx;
} Rec;

static int rec_cmp(const void *a, const void *b) {
    const Rec *x = a, *y = b;
    if (x->v != y->v)
        return x->v < y->v ? -1 : 1;
    if (x->s != y->s)
        return x->s < y->s ? -1 : 1;
    return x->idx < y->idx ? -1 : x->idx > y->idx;
}

int main(void) {
    unsigned st = 13579u;
    int total = 0;
    for (int s = 0; s < K; s++) {
        st ^= st << 13;
        st ^= st >> 17;
        st ^= st << 5;
        lens[s] = 60 + (int)(st % 300u);
        long cur = 0;
        for (int i = 0; i < lens[s]; i++) {
            st ^= st << 13;
            st ^= st >> 17;
            st ^= st << 5;
            cur += (long)(st % 6u); /* small steps so equal values appear across streams */
            streams[s][i] = cur;
        }
        total += lens[s];
        pthread_mutex_init(&qs[s].mu, NULL);
        pthread_cond_init(&qs[s].nf, NULL);
        pthread_cond_init(&qs[s].ne, NULL);
    }
    pthread_t th[K];
    for (long s = 0; s < K; s++)
        check(pthread_create(&th[s], NULL, producer, (void *)s) == 0, "create");

    static long merged[K * MAXLEN];
    static int from[K * MAXLEN];
    int n = 0;
    for (int s = 0; s < K; s++) {
        long v = get(&qs[s]);
        if (v != END) {
            Head h = {v, s};
            hpush(h);
        }
    }
    while (hn > 0) {
        Head h = hpop();
        merged[n] = h.v;
        from[n++] = h.s;
        long v = get(&qs[h.s]);
        if (v != END) {
            Head nh = {v, h.s};
            hpush(nh);
        }
    }
    for (int s = 0; s < K; s++)
        pthread_join(th[s], NULL);

    static Rec recs[K * MAXLEN];
    int r = 0;
    for (int s = 0; s < K; s++)
        for (int i = 0; i < lens[s]; i++) {
            recs[r].v = streams[s][i];
            recs[r].s = s;
            recs[r].idx = i;
            r++;
        }
    qsort(recs, (size_t)r, sizeof(Rec), rec_cmp);
    check(n == total && r == total, "count");
    long chk = 0;
    for (int i = 0; i < n; i++) {
        check(merged[i] == recs[i].v && from[i] == recs[i].s, "matches full sort with stream tie-break");
        if (i > 0)
            check(merged[i - 1] <= merged[i], "sorted");
        chk = (chk * 131 + merged[i] * 7 + from[i]) % 1000000007L;
    }
    int first_counts[K] = {0};
    for (int i = 0; i < 60; i++)
        first_counts[from[i]]++;
    printf("streams %d, lengths", K);
    for (int s = 0; s < K; s++)
        printf(" %d", lens[s]);
    printf("\nmerged %d values, min %ld max %ld\n", n, merged[0], merged[n - 1]);
    printf("first 10 (value:stream):");
    for (int i = 0; i < 10; i++)
        printf(" %ld:%d", merged[i], from[i]);
    printf("\nstream share of first 60:");
    for (int s = 0; s < K; s++)
        printf(" %d", first_counts[s]);
    printf("\nmerge hash %ld\n", chk);
    for (int s = 0; s < K; s++) {
        pthread_mutex_destroy(&qs[s].mu);
        pthread_cond_destroy(&qs[s].nf);
        pthread_cond_destroy(&qs[s].ne);
    }
    return 0;
}
