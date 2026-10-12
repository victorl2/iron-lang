/*
 * title: Parallel 0/1 knapsack DP splitting the capacity axis
 * topic: concurrency
 * covers: DP rows, per-item barrier via join, double buffering, item reconstruction, brute force check
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

enum { ITEMS = 40, CAP = 500, NT = 6 };

typedef struct {
    const int *prev;
    int *cur;
    int w, v;
} Row;

/* dp[i][c] = max(dp[i-1][c], dp[i-1][c-w] + v); each thread owns a capacity range. */
static void row_worker(void *ctx, int tid, int nt) {
    Row *r = ctx;
    int lo, hi;
    par_range(CAP + 1, tid, nt, &lo, &hi);
    for (int c = lo; c < hi; c++) {
        int best = r->prev[c];
        if (c >= r->w && r->prev[c - r->w] + r->v > best)
            best = r->prev[c - r->w] + r->v;
        r->cur[c] = best;
    }
}

static int seq_knap(const int *w, const int *v, int n, int cap) {
    int dp[CAP + 1];
    memset(dp, 0, sizeof dp);
    for (int i = 0; i < n; i++)
        for (int c = cap; c >= w[i]; c--)
            if (dp[c - w[i]] + v[i] > dp[c])
                dp[c] = dp[c - w[i]] + v[i];
    return dp[cap];
}

/* exhaustive check on a small prefix of the items */
static int brute(const int *w, const int *v, int n, int cap) {
    int best = 0;
    for (unsigned mask = 0; mask < (1u << n); mask++) {
        int tw = 0, tv = 0;
        for (int i = 0; i < n; i++)
            if (mask >> i & 1u) {
                tw += w[i];
                tv += v[i];
            }
        if (tw <= cap && tv > best)
            best = tv;
    }
    return best;
}

int main(void) {
    static int table[ITEMS + 1][CAP + 1];
    int w[ITEMS], v[ITEMS];
    uint64_t seed = 90210;
    for (int i = 0; i < ITEMS; i++) {
        w[i] = 5 + (int)(sm64(&seed) % 60u);
        v[i] = 10 + (int)(sm64(&seed) % 200u);
    }
    for (int i = 0; i < ITEMS; i++) {
        Row r = {table[i], table[i + 1], w[i], v[i]};
        par_run(NT, row_worker, &r);
    }
    int best = table[ITEMS][CAP];
    check(best == seq_knap(w, v, ITEMS, CAP), "matches sequential 1D dp");
    /* reconstruct */
    int c = CAP, tw = 0, tv = 0, chosen = 0;
    unsigned long long picks = 0;
    for (int i = ITEMS; i >= 1; i--)
        if (table[i][c] != table[i - 1][c]) {
            picks |= 1ULL << (i - 1);
            c -= w[i - 1];
            tw += w[i - 1];
            tv += v[i - 1];
            chosen++;
        }
    check(tv == best && tw <= CAP && c >= 0, "reconstruction consistent");
    printf("items=%d cap=%d best=%d used_weight=%d chosen=%d\n", ITEMS, CAP, best, tw, chosen);
    printf("pick mask=%010llx\n", picks);
    for (int cap = 50; cap <= CAP; cap *= 3)
        printf("cap %3d -> %d\n", cap, table[ITEMS][cap]);
    /* brute force on first 16 items, several capacities, via a fresh parallel table */
    for (int cap = 60; cap <= 300; cap += 80) {
        static int t2[17][CAP + 1];
        memset(t2, 0, sizeof t2);
        for (int i = 0; i < 16; i++) {
            Row r = {t2[i], t2[i + 1], w[i], v[i]};
            par_run(3, row_worker, &r);
        }
        int bf = brute(w, v, 16, cap);
        check(t2[16][cap] == bf, "brute force agrees");
        printf("brute(16 items, cap %d)=%d\n", cap, bf);
    }
    return 0;
}
