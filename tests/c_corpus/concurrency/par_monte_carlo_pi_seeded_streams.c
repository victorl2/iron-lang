/*
 * title: Parallel Monte Carlo pi with per-thread seeded streams
 * topic: concurrency
 * covers: jump-free stream splitting by seed mix, integer hit counts, thread-count independence of total via fixed sample blocks
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*ParFn)(void *ctx, int tid, int nt);
typedef struct {
    ParFn fn;
    void *ctx;
    int tid;
    int nt;
} ParJob;

static void *par_tramp(void *p) {
    ParJob *j = p;
    j->fn(j->ctx, j->tid, j->nt);
    return NULL;
}

/* Run fn on nt threads (nt <= 8) and join them all. */
static inline void par_run(int nt, ParFn fn, void *ctx) {
    pthread_t th[8];
    ParJob jobs[8];
    for (int i = 0; i < nt; i++) {
        jobs[i].fn = fn;
        jobs[i].ctx = ctx;
        jobs[i].tid = i;
        jobs[i].nt = nt;
        if (pthread_create(&th[i], NULL, par_tramp, &jobs[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    for (int i = 0; i < nt; i++)
        pthread_join(th[i], NULL);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline uint64_t sm64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

enum { BLOCKS = 32, PER_BLOCK = 20000 };

typedef struct {
    uint64_t master;
    long hits[BLOCKS];
} Mc;

/* Every block has its own stream derived from (master, block index), so the
 * total is independent of how blocks are assigned to threads. */
static long run_block(uint64_t master, int b) {
    uint64_t s = master ^ ((uint64_t)(b + 1) * 0xD1B54A32D192ED03ULL);
    (void)sm64(&s);
    long hits = 0;
    for (int i = 0; i < PER_BLOCK; i++) {
        uint64_t r = sm64(&s);
        uint32_t x = (uint32_t)(r & 0xFFFFu), y = (uint32_t)((r >> 16) & 0xFFFFu);
        /* unit quarter circle in a 65536 grid */
        if ((uint64_t)x * x + (uint64_t)y * y < 65536ULL * 65536ULL)
            hits++;
    }
    return hits;
}

static void worker(void *ctx, int tid, int nt) {
    Mc *m = ctx;
    for (int b = tid; b < BLOCKS; b += nt)
        m->hits[b] = run_block(m->master, b);
}

int main(void) {
    static Mc m;
    m.master = 0x243F6A8885A308D3ULL;
    long ref[BLOCKS], reftot = 0;
    for (int b = 0; b < BLOCKS; b++) {
        ref[b] = run_block(m.master, b);
        reftot += ref[b];
    }
    for (int nt = 1; nt <= 8; nt++) {
        memset(m.hits, 0, sizeof m.hits);
        par_run(nt, worker, &m);
        long tot = 0;
        for (int b = 0; b < BLOCKS; b++) {
            check(m.hits[b] == ref[b], "block result independent of thread count");
            tot += m.hits[b];
        }
        check(tot == reftot, "total independent of thread count");
    }
    long n = (long)BLOCKS * PER_BLOCK;
    /* pi ~ 4*hits/n ; report with 4 digits and the integer numerator */
    double est = 4.0 * (double)reftot / (double)n;
    printf("samples=%ld hits=%ld\n", n, reftot);
    printf("pi estimate: %.4f\n", est);
    check(est > 3.12 && est < 3.16, "estimate near pi");
    for (int b = 0; b < 6; b++)
        printf("block %d hits %ld\n", b, ref[b]);
    /* streams for different blocks differ */
    int distinct = 1;
    for (int b = 1; b < BLOCKS; b++)
        if (ref[b] != ref[0])
            distinct++;
    printf("blocks with distinct counts vs block 0: %d of %d\n", distinct - 1, BLOCKS - 1);
    return 0;
}
