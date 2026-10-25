/*
 * title: Ownership transfer through a thread-safe queue
 * topic: memory
 * covers: producer/consumer message passing, ownership handoff (send moves, receive owns), mutex+condvar queue, per-message ownership audit
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPROD 3
#define NCONS 2
#define PER_PROD 200
#define QCAP 8

typedef struct {
    int producer;
    int seq;
    int owner; /* 0 = queue/in flight, otherwise consumer id + 1 */
    unsigned checksum;
    int payload[4];
} Msg;

static Msg *ring[QCAP];
static int head, tail, count;
static int producers_done;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;

static long consumed_sum[NCONS];
static int consumed_count[NCONS];
static int ownership_violations[NCONS];
static int per_producer_seen[NPROD];
static int order_violation[NCONS];

static unsigned checksum_of(const Msg *m) {
    unsigned h = 2166136261u;
    h = (h ^ (unsigned)m->producer) * 16777619u;
    h = (h ^ (unsigned)m->seq) * 16777619u;
    for (int i = 0; i < 4; i++)
        h = (h ^ (unsigned)m->payload[i]) * 16777619u;
    return h;
}

/* send takes ownership: the caller must not touch the message afterwards. */
static void q_send(Msg *m) {
    pthread_mutex_lock(&mu);
    while (count == QCAP)
        pthread_cond_wait(&not_full, &mu);
    ring[tail] = m;
    tail = (tail + 1) % QCAP;
    count++;
    pthread_cond_signal(&not_empty);
    pthread_mutex_unlock(&mu);
}

static Msg *q_recv(void) {
    pthread_mutex_lock(&mu);
    while (count == 0 && producers_done < NPROD)
        pthread_cond_wait(&not_empty, &mu);
    Msg *m = NULL;
    if (count > 0) {
        m = ring[head];
        ring[head] = NULL;
        head = (head + 1) % QCAP;
        count--;
        pthread_cond_signal(&not_full);
    }
    pthread_mutex_unlock(&mu);
    return m;
}

static void *producer(void *arg) {
    int id = (int)(size_t)arg;
    unsigned s = 777u + (unsigned)id * 1000003u;
    for (int i = 0; i < PER_PROD; i++) {
        Msg *m = malloc(sizeof *m);
        if (!m)
            abort();
        m->producer = id;
        m->seq = i;
        m->owner = 0;
        for (int k = 0; k < 4; k++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            m->payload[k] = (int)(s % 1000);
        }
        m->checksum = checksum_of(m);
        q_send(m); /* m is no longer ours */
    }
    pthread_mutex_lock(&mu);
    producers_done++;
    pthread_cond_broadcast(&not_empty);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void *consumer(void *arg) {
    int id = (int)(size_t)arg;
    int last_seq[NPROD];
    for (int i = 0; i < NPROD; i++)
        last_seq[i] = -1;
    Msg *m;
    while ((m = q_recv()) != NULL) {
        if (m->owner != 0)
            ownership_violations[id]++; /* somebody else already owns it */
        m->owner = id + 1;
        if (checksum_of(m) != m->checksum)
            ownership_violations[id]++;
        if (m->seq <= last_seq[m->producer])
            order_violation[id]++; /* per-producer FIFO order must be preserved */
        last_seq[m->producer] = m->seq;
        consumed_sum[id] += m->payload[0] + m->payload[3];
        consumed_count[id]++;
        free(m);
    }
    return NULL;
}

int main(void) {
    pthread_t p[NPROD], c[NCONS];
    for (int i = 0; i < NCONS; i++)
        pthread_create(&c[i], NULL, consumer, (void *)(size_t)i);
    for (int i = 0; i < NPROD; i++)
        pthread_create(&p[i], NULL, producer, (void *)(size_t)i);
    for (int i = 0; i < NPROD; i++)
        pthread_join(p[i], NULL);
    for (int i = 0; i < NCONS; i++)
        pthread_join(c[i], NULL);

    int total = 0, violations = 0, order_bad = 0;
    long sum = 0;
    for (int i = 0; i < NCONS; i++) {
        total += consumed_count[i];
        sum += consumed_sum[i];
        violations += ownership_violations[i];
        order_bad += order_violation[i];
    }
    (void)per_producer_seen;

    /* recompute the expected checksum sum single-threaded */
    long expect = 0;
    for (int id = 0; id < NPROD; id++) {
        unsigned s = 777u + (unsigned)id * 1000003u;
        for (int i = 0; i < PER_PROD; i++) {
            int pl[4];
            for (int k = 0; k < 4; k++) {
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                pl[k] = (int)(s % 1000);
            }
            expect += pl[0] + pl[3];
        }
    }
    printf("messages produced: %d, consumed: %d\n", NPROD * PER_PROD, total);
    printf("payload sum: %ld (expected %ld)\n", sum, expect);
    printf("ownership violations: %d, order violations: %d\n", violations, order_bad);
    printf("queue empty at end: %d\n", count == 0);
    if (total != NPROD * PER_PROD || sum != expect || violations || order_bad || count != 0) {
        fprintf(stderr, "check failed: transfer\n");
        return 1;
    }
    return 0;
}
