/*
 * title: Parallel odd-even transposition sort
 * topic: concurrency
 * covers: compare-exchange phases, thread-owned pair ranges, N rounds, early-out detection
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

/* Even split of [0,n) into nt contiguous ranges. */
static inline void par_range(int n, int tid, int nt, int *lo, int *hi) {
    *lo = (int)((long)n * tid / nt);
    *hi = (int)((long)n * (tid + 1) / nt);
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

enum { N = 257, NT = 6 };

typedef struct {
    int *a;
    int parity;
    int swaps[8];
} Phase;

/* Each thread handles a contiguous range of pair indices; pairs are disjoint
 * within a phase so no locking is needed. */
static void phase_worker(void *ctx, int tid, int nt) {
    Phase *p = ctx;
    int npairs = (N - p->parity) / 2;
    int lo, hi;
    par_range(npairs, tid, nt, &lo, &hi);
    int sw = 0;
    for (int k = lo; k < hi; k++) {
        int i = p->parity + 2 * k;
        if (p->a[i] > p->a[i + 1]) {
            int t = p->a[i];
            p->a[i] = p->a[i + 1];
            p->a[i + 1] = t;
            sw++;
        }
    }
    p->swaps[tid] = sw;
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

static void seq_oets(int *a, int *rounds) {
    int r = 0, quiet = 0;
    while (quiet < 2) {
        int sw = 0;
        for (int i = r & 1; i + 1 < N; i += 2)
            if (a[i] > a[i + 1]) {
                int t = a[i];
                a[i] = a[i + 1];
                a[i + 1] = t;
                sw++;
            }
        quiet = sw ? 0 : quiet + 1;
        r++;
    }
    *rounds = r;
}

int main(void) {
    uint64_t seed = 8080;
    for (int shape = 0; shape < 4; shape++) {
        int a[N], b[N], ref[N];
        for (int i = 0; i < N; i++) {
            uint64_t r = sm64(&seed);
            int v = (int)(r % 1000u);
            if (shape == 1)
                v = N - i; /* reversed */
            if (shape == 2)
                v = i; /* sorted */
            if (shape == 3)
                v = (int)(r % 3u); /* many ties */
            a[i] = b[i] = ref[i] = v;
        }
        qsort(ref, N, sizeof(int), cmp_int);
        int seq_rounds;
        seq_oets(b, &seq_rounds);
        check(memcmp(b, ref, sizeof b) == 0, "sequential oets sorted");

        Phase p;
        p.a = a;
        int rounds = 0, quiet = 0, total_swaps = 0;
        while (quiet < 2) {
            p.parity = rounds & 1;
            par_run(NT, phase_worker, &p);
            int sw = 0;
            for (int t = 0; t < NT; t++)
                sw += p.swaps[t];
            total_swaps += sw;
            quiet = sw ? 0 : quiet + 1;
            rounds++;
            check(rounds <= N + 2, "terminates within N rounds");
        }
        check(memcmp(a, ref, sizeof a) == 0, "parallel oets sorted");
        check(rounds == seq_rounds, "same round count as sequential");
        static const char *names[] = {"random", "reversed", "sorted", "ties"};
        printf("%-8s rounds=%d swaps=%d first=%d last=%d\n", names[shape], rounds, total_swaps, a[0],
               a[N - 1]);
    }
    return 0;
}
