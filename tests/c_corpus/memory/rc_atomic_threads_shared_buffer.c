/*
 * title: Atomic refcount across threads
 * topic: memory
 * covers: C11 atomics, acquire/release ordering on last release, shared ownership across threads
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    atomic_int rc;
    int len;
    unsigned char data[64];
} Buf;

static atomic_int g_frees;
static atomic_int g_bad;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Buf *buf_new(int seed) {
    Buf *b = malloc(sizeof *b);
    check(b != NULL, "alloc");
    atomic_init(&b->rc, 1);
    b->len = 64;
    for (int i = 0; i < 64; i++)
        b->data[i] = (unsigned char)(seed * 7 + i);
    return b;
}

static Buf *buf_retain(Buf *b) {
    atomic_fetch_add_explicit(&b->rc, 1, memory_order_relaxed);
    return b;
}

static void buf_release(Buf *b) {
    if (atomic_fetch_sub_explicit(&b->rc, 1, memory_order_release) == 1) {
        atomic_thread_fence(memory_order_acquire);
        memset(b->data, 0xDD, sizeof b->data);
        free(b);
        atomic_fetch_add(&g_frees, 1);
    }
}

#define NBUF 8
#define NTHREADS 4
#define ITERS 2000

static Buf *g_bufs[NBUF];
static long g_sums[NTHREADS];

static unsigned step(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void *worker(void *arg) {
    int id = (int)(size_t)arg;
    unsigned s = 0x9E3779B9u * (unsigned)(id + 1);
    long sum = 0;
    for (int i = 0; i < ITERS; i++) {
        unsigned r = step(&s);
        Buf *b = buf_retain(g_bufs[r % NBUF]);
        /* a retained buffer must never be poisoned */
        if (b->data[0] == 0xDD && b->data[1] == 0xDD)
            atomic_fetch_add(&g_bad, 1);
        Buf *b2 = buf_retain(b);
        sum += b->data[i % 64];
        buf_release(b);
        buf_release(b2);
    }
    g_sums[id] = sum;
    return NULL;
}

int main(void) {
    for (int i = 0; i < NBUF; i++)
        g_bufs[i] = buf_new(i);
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(th[i], NULL);

    /* recompute sums single-threaded */
    for (int t = 0; t < NTHREADS; t++) {
        unsigned s = 0x9E3779B9u * (unsigned)(t + 1);
        long sum = 0;
        for (int i = 0; i < ITERS; i++) {
            unsigned r = step(&s);
            sum += g_bufs[r % NBUF]->data[i % 64];
        }
        check(sum == g_sums[t], "sum mismatch");
        printf("thread %d sum %ld\n", t, g_sums[t]);
    }
    for (int i = 0; i < NBUF; i++) {
        int rc = atomic_load(&g_bufs[i]->rc);
        check(rc == 1, "rc back to 1");
    }
    printf("bad reads: %d\n", atomic_load(&g_bad));
    check(atomic_load(&g_bad) == 0, "no poisoned reads");
    for (int i = 0; i < NBUF; i++)
        buf_release(g_bufs[i]);
    printf("frees: %d\n", atomic_load(&g_frees));
    check(atomic_load(&g_frees) == NBUF, "all freed");
    return 0;
}
