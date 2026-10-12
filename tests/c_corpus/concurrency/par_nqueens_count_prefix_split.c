/*
 * title: Parallel N-queens counting split by placement prefixes
 * topic: concurrency
 * covers: bitmask backtracking, prefix task enumeration, dynamic claiming, known solution counts
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

#include <stdatomic.h>

enum { MAXTASK = 4096 };

typedef struct {
    int n;
    int ntask;
    unsigned cols[MAXTASK], d1[MAXTASK], d2[MAXTASK];
    long long result[MAXTASK];
    atomic_int next;
} Nq;

static long long solve(int n, int row, unsigned cols, unsigned d1, unsigned d2) {
    unsigned full = (1u << n) - 1u;
    if (cols == full)
        return 1;
    long long cnt = 0;
    unsigned avail = full & ~(cols | d1 | d2);
    while (avail) {
        unsigned bit = avail & (0u - avail);
        avail ^= bit;
        cnt += solve(n, row + 1, cols | bit, ((d1 | bit) << 1) & full, (d2 | bit) >> 1);
    }
    return cnt;
}

/* enumerate legal placements of the first `depth` rows */
static void gen_prefix(Nq *q, int depth, int row, unsigned cols, unsigned d1, unsigned d2) {
    unsigned full = (1u << q->n) - 1u;
    if (row == depth) {
        check(q->ntask < MAXTASK, "task table");
        q->cols[q->ntask] = cols;
        q->d1[q->ntask] = d1;
        q->d2[q->ntask] = d2;
        q->ntask++;
        return;
    }
    unsigned avail = full & ~(cols | d1 | d2);
    while (avail) {
        unsigned bit = avail & (0u - avail);
        avail ^= bit;
        gen_prefix(q, depth, row + 1, cols | bit, ((d1 | bit) << 1) & full, (d2 | bit) >> 1);
    }
}

static void worker(void *ctx, int tid, int nt) {
    Nq *q = ctx;
    (void)tid;
    (void)nt;
    for (;;) {
        int t = atomic_fetch_add(&q->next, 1);
        if (t >= q->ntask)
            break;
        /* rows already placed = popcount(cols) */
        int placed = 0;
        for (unsigned c = q->cols[t]; c; c &= c - 1)
            placed++;
        q->result[t] = solve(q->n, placed, q->cols[t], q->d1[t], q->d2[t]);
    }
}

int main(void) {
    static const long long known[] = {1, 1, 0, 0, 2, 10, 4, 40, 92, 352, 724, 2680};
    static Nq q;
    for (int n = 1; n <= 11; n++) {
        int depth = n >= 6 ? 3 : 1;
        q.n = n;
        q.ntask = 0;
        atomic_init(&q.next, 0);
        gen_prefix(&q, depth, 0, 0, 0, 0);
        par_run(8, worker, &q);
        long long total = 0;
        for (int t = 0; t < q.ntask; t++)
            total += q.result[t];
        long long ref = solve(n, 0, 0, 0, 0);
        check(total == ref, "parallel equals sequential");
        check(total == known[n], "known count");
        printf("n=%2d tasks=%3d solutions=%lld\n", n, q.ntask, total);
    }
    return 0;
}
