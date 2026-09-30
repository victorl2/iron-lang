/*
 * title: Seqlock with torn-read detector
 * topic: concurrency
 * covers: seqlock, sequence counter parity, C11 fences, optimistic readers, torn snapshot detection, writer mutex
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { K = 8, WRITERS = 2, UPDATES = 600, READERS = 3, READS = 1500 };

typedef struct {
    atomic_uint seq;
    atomic_uint word[K];
    pthread_mutex_t writers;
} Seq;

static Seq sl = {0, {0}, PTHREAD_MUTEX_INITIALIZER};

/* Invariant of every consistent snapshot: word[k] == word[0] * (k + 1). */
static int consistent(const unsigned *w) {
    for (int k = 1; k < K; k++)
        if (w[k] != w[0] * (unsigned)(k + 1))
            return 0;
    return 1;
}

/* Writers are serialized by a mutex; the seqlock protects readers from them. New value = old + 1. */
static void sl_increment(Seq *s, int yield_mid) {
    pthread_mutex_lock(&s->writers);
    unsigned q = atomic_load_explicit(&s->seq, memory_order_relaxed);
    unsigned v = atomic_load_explicit(&s->word[0], memory_order_relaxed) + 1;
    atomic_store_explicit(&s->seq, q + 1, memory_order_relaxed); /* odd: write in progress */
    atomic_thread_fence(memory_order_release);
    for (int k = 0; k < K; k++) {
        atomic_store_explicit(&s->word[k], v * (unsigned)(k + 1), memory_order_relaxed);
        if (k == K / 2 && yield_mid)
            sched_yield();
    }
    atomic_store_explicit(&s->seq, q + 2, memory_order_release);
    pthread_mutex_unlock(&s->writers);
}

static void sl_read(Seq *s, unsigned *out) {
    for (;;) {
        unsigned q1 = atomic_load_explicit(&s->seq, memory_order_acquire);
        if (q1 & 1u) {
            sched_yield();
            continue;
        }
        for (int k = 0; k < K; k++)
            out[k] = atomic_load_explicit(&s->word[k], memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s->seq, memory_order_relaxed) == q1)
            return;
    }
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static atomic_int torn_validated;
static pthread_mutex_t stat_mu = PTHREAD_MUTEX_INITIALIZER;
static long validated_total;
static unsigned max_seen[READERS];
static int monotonic_bad[READERS];

static void *writer(void *p) {
    int id = (int)(intptr_t)p;
    for (int i = 0; i < UPDATES; i++)
        sl_increment(&sl, (i + id) % 2 == 0);
    return NULL;
}

static void *reader(void *p) {
    int id = (int)(intptr_t)p;
    unsigned last = 0;
    for (int i = 0; i < READS; i++) {
        unsigned snap[K];
        sl_read(&sl, snap);
        if (!consistent(snap))
            atomic_fetch_add(&torn_validated, 1);
        if (snap[0] < last)
            monotonic_bad[id]++;
        last = snap[0];
        if (i % 50 == 0)
            sched_yield();
    }
    max_seen[id] = last;
    pthread_mutex_lock(&stat_mu);
    validated_total += READS;
    pthread_mutex_unlock(&stat_mu);
    return NULL;
}

int main(void) {
    /* Detector self-test on hand-made snapshots. */
    unsigned good[K], bad[K];
    for (int k = 0; k < K; k++)
        good[k] = bad[k] = 7u * (unsigned)(k + 1);
    bad[5] = 7u * 5u; /* half-updated: one word from the previous generation */
    printf("detector: good snapshot %s, spliced snapshot %s\n", consistent(good) ? "accepted" : "rejected",
           consistent(bad) ? "accepted" : "rejected");
    check(consistent(good) && !consistent(bad), "detector self-test");

    sl_increment(&sl, 0); /* value 1, seq 2 */
    pthread_t th[WRITERS + READERS];
    for (int i = 0; i < WRITERS; i++)
        pthread_create(&th[i], NULL, writer, (void *)(intptr_t)i);
    for (int i = 0; i < READERS; i++)
        pthread_create(&th[WRITERS + i], NULL, reader, (void *)(intptr_t)i);
    for (int i = 0; i < WRITERS + READERS; i++)
        pthread_join(th[i], NULL);

    unsigned fin[K];
    sl_read(&sl, fin);
    unsigned final_seq = atomic_load(&sl.seq);
    int mono = 0;
    for (int i = 0; i < READERS; i++)
        mono += monotonic_bad[i];
    printf("validated reads: %ld, torn snapshots accepted: %d\n", validated_total, atomic_load(&torn_validated));
    printf("final value %u, words consistent %s\n", fin[0], consistent(fin) ? "yes" : "no");
    printf("sequence counter %u (even: %s), expected %u\n", final_seq, (final_seq & 1u) ? "no" : "yes",
           2u + 2u * (unsigned)(WRITERS * UPDATES));
    printf("non-monotonic snapshots %d\n", mono);
    check(atomic_load(&torn_validated) == 0, "torn read accepted");
    check(fin[0] == (unsigned)(WRITERS * UPDATES + 1), "final value");
    check(final_seq == 2u + 2u * (unsigned)(WRITERS * UPDATES), "sequence");
    check(mono == 0, "monotonic");
    return 0;
}
