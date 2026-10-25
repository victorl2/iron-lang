/*
 * title: Multi-writer seqlock with atomic payload words
 * topic: concurrency
 * covers: sequence counter CAS to odd, release publish, reader validate loop, acquire fence, race-free payload via relaxed atomics
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Writers need no mutex: a writer claims the seqlock by CASing an even sequence s to s+1 (odd =
 * write in progress), writes the payload, then store-releases s+2. Two writers cannot both win
 * the CAS from the same even value, so they are serialized by the counter itself.
 * A reader loads s1 (acquire); if odd it retries; it reads the payload words, issues an acquire
 * fence, and re-reads the sequence; the read is valid only if s2 == s1.
 *
 * The payload words are relaxed atomics, so a torn read is merely detected and discarded,
 * never a data race. Invariant used as oracle: word[i] == base * (i + 1), so any mixture of two
 * writes breaks the invariant.
 *
 * Deterministic outputs: the final sequence is exactly 2 * total writes.
 */
enum { WRITERS = 3, READERS = 3, WRITES = 2500, W = 5 };

static atomic_uint seq;
static atomic_uint payload[W];
static atomic_int writers_done;
static long good_reads[READERS];
static long retries[READERS];
static long bad_reads[READERS];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void seq_write(unsigned base) {
    unsigned s = atomic_load_explicit(&seq, memory_order_relaxed);
    for (;;) {
        if (s & 1u) {
            sched_yield();
            s = atomic_load_explicit(&seq, memory_order_relaxed);
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(&seq, &s, s + 1, memory_order_acquire,
                                                  memory_order_relaxed))
            break;
    }
    for (int i = 0; i < W; i++)
        atomic_store_explicit(&payload[i], base * (unsigned)(i + 1), memory_order_relaxed);
    atomic_store_explicit(&seq, s + 2, memory_order_release);
}

/* returns 1 with a validated copy, 0 if the attempt must be retried */
static int seq_read(unsigned *out) {
    unsigned s1 = atomic_load_explicit(&seq, memory_order_acquire);
    if (s1 & 1u)
        return 0;
    for (int i = 0; i < W; i++)
        out[i] = atomic_load_explicit(&payload[i], memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    unsigned s2 = atomic_load_explicit(&seq, memory_order_relaxed);
    return s1 == s2;
}

static void *writer(void *p) {
    unsigned id = (unsigned)(size_t)p + 1;
    for (unsigned k = 1; k <= WRITES; k++)
        seq_write(id * 10000u + k);
    atomic_fetch_add(&writers_done, 1);
    return NULL;
}

static void *reader(void *p) {
    int id = (int)(size_t)p;
    while (atomic_load(&writers_done) < WRITERS) {
        unsigned v[W];
        if (!seq_read(v)) {
            retries[id]++;
            continue;
        }
        int ok = 1;
        for (int i = 1; i < W; i++)
            if (v[i] != v[0] * (unsigned)(i + 1))
                ok = 0;
        if (ok)
            good_reads[id]++;
        else
            bad_reads[id]++;
    }
    return NULL;
}

int main(void) {
    pthread_t w[WRITERS], r[READERS];
    for (int i = 0; i < READERS; i++)
        check(pthread_create(&r[i], NULL, reader, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < WRITERS; i++)
        check(pthread_create(&w[i], NULL, writer, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < WRITERS; i++)
        pthread_join(w[i], NULL);
    for (int i = 0; i < READERS; i++)
        pthread_join(r[i], NULL);
    long bad = 0;
    for (int i = 0; i < READERS; i++)
        bad += bad_reads[i];
    unsigned fin = atomic_load(&seq);
    check(bad == 0, "validated reads are never torn");
    check(fin == 2u * WRITERS * WRITES, "sequence counts every write");
    unsigned v[W];
    check(seq_read(v), "quiescent read validates");
    unsigned base = v[0];
    check(base % 10000u == WRITES && base / 10000u >= 1 && base / 10000u <= WRITERS, "last value is some writer's final");
    for (int i = 0; i < W; i++)
        check(v[i] == base * (unsigned)(i + 1), "final payload consistent");
    printf("writers=%d writes each=%d final sequence=%u (even: %s)\n", WRITERS, WRITES, fin,
           (fin & 1u) ? "no" : "yes");
    printf("torn validated reads: %ld\n", bad);
    printf("final payload word multiples consistent: yes, sequence/2 = %u\n", fin / 2);
    return 0;
}
