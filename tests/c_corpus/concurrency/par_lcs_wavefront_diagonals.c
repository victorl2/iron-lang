/*
 * title: Parallel LCS by anti-diagonal wavefront
 * topic: concurrency
 * covers: wavefront DP, per-diagonal parallel cells, traceback, 2D table, sequential reference
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

enum { NT = 6 };

typedef struct {
    const char *a, *b;
    int n, m;
    int *t; /* (n+1) x (m+1) */
    int d;  /* current anti-diagonal i + j == d */
} Wf;

static void diag_worker(void *ctx, int tid, int nt) {
    Wf *w = ctx;
    int ilo = w->d - w->m > 1 ? w->d - w->m : 1;
    int ihi = w->d - 1 < w->n ? w->d - 1 : w->n;
    if (ihi < ilo)
        return;
    int cnt = ihi - ilo + 1;
    int lo, hi;
    par_range(cnt, tid, nt, &lo, &hi);
    int W = w->m + 1;
    for (int i = ilo + lo; i < ilo + hi; i++) {
        int j = w->d - i;
        int v;
        if (w->a[i - 1] == w->b[j - 1])
            v = w->t[(i - 1) * W + j - 1] + 1;
        else {
            int up = w->t[(i - 1) * W + j], left = w->t[i * W + j - 1];
            v = up > left ? up : left;
        }
        w->t[i * W + j] = v;
    }
}

static int lcs_parallel(const char *a, const char *b, int n, int m, int *t, int *diags) {
    Wf w = {a, b, n, m, t, 0};
    memset(t, 0, (size_t)(n + 1) * (size_t)(m + 1) * sizeof(int));
    int count = 0;
    for (int d = 2; d <= n + m; d++) {
        w.d = d;
        par_run(NT, diag_worker, &w);
        count++;
    }
    *diags = count;
    return t[n * (m + 1) + m];
}

static int lcs_seq(const char *a, const char *b, int n, int m) {
    int *prev = calloc((size_t)m + 1, sizeof(int)), *cur = calloc((size_t)m + 1, sizeof(int));
    check(prev && cur, "alloc");
    for (int i = 1; i <= n; i++) {
        for (int j = 1; j <= m; j++) {
            if (a[i - 1] == b[j - 1])
                cur[j] = prev[j - 1] + 1;
            else
                cur[j] = prev[j] > cur[j - 1] ? prev[j] : cur[j - 1];
        }
        int *tmp = prev;
        prev = cur;
        cur = tmp;
    }
    int r = prev[m];
    free(prev);
    free(cur);
    return r;
}

int main(void) {
    uint64_t seed = 555;
    int shapes[][3] = {{1, 1, 2}, {10, 15, 2}, {40, 40, 4}, {120, 90, 3}, {200, 200, 26}, {7, 300, 2}};
    for (int s = 0; s < 6; s++) {
        int n = shapes[s][0], m = shapes[s][1], alpha = shapes[s][2];
        char *a = malloc((size_t)n + 1), *b = malloc((size_t)m + 1);
        int *t = malloc((size_t)(n + 1) * (size_t)(m + 1) * sizeof(int));
        check(a && b && t, "alloc");
        for (int i = 0; i < n; i++)
            a[i] = (char)('a' + sm64(&seed) % (unsigned)alpha);
        for (int i = 0; i < m; i++)
            b[i] = (char)('a' + sm64(&seed) % (unsigned)alpha);
        int diags;
        int len = lcs_parallel(a, b, n, m, t, &diags);
        check(len == lcs_seq(a, b, n, m), "matches rolling-row sequential");
        /* traceback (prefer diagonal, then up) and verify it is a common subsequence */
        char *sub = malloc((size_t)len + 1);
        check(sub != NULL, "alloc");
        int i = n, j = m, k = len;
        sub[k] = 0;
        while (i > 0 && j > 0) {
            if (a[i - 1] == b[j - 1]) {
                sub[--k] = a[i - 1];
                i--;
                j--;
            } else if (t[(i - 1) * (m + 1) + j] >= t[i * (m + 1) + j - 1])
                i--;
            else
                j--;
        }
        check(k == 0, "traceback consumed the whole length");
        int p = 0;
        for (int x = 0; x < n && p < len; x++)
            if (a[x] == sub[p])
                p++;
        int q = 0;
        for (int x = 0; x < m && q < len; x++)
            if (b[x] == sub[q])
                q++;
        check(p == len && q == len, "traceback is a common subsequence");
        printf("n=%3d m=%3d alphabet=%2d diagonals=%3d lcs=%3d", n, m, alpha, diags, len);
        if (len <= 24)
            printf(" seq=%s", sub);
        printf("\n");
        free(a);
        free(b);
        free(t);
        free(sub);
    }
    return 0;
}
