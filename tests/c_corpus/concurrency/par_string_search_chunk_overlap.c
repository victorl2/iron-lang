/*
 * title: Parallel substring search across chunks with boundary overlap
 * topic: concurrency
 * covers: chunk overlap of m-1 bytes, ownership by match start, KMP per chunk, matches straddling boundaries
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
    const unsigned char *text;
    int n;
    const unsigned char *pat;
    int m;
    int *found[8];
    int nfound[8];
    int cap[8];
} Srch;

static void kmp_table(const unsigned char *p, int m, int *fail) {
    fail[0] = 0;
    for (int i = 1, k = 0; i < m; i++) {
        while (k && p[i] != p[k])
            k = fail[k - 1];
        if (p[i] == p[k])
            k++;
        fail[i] = k;
    }
}

/* Thread owns match START positions in [lo, hi); it reads m-1 bytes past hi. */
static void search_worker(void *ctx, int tid, int nt) {
    Srch *s = ctx;
    int lo, hi;
    par_range(s->n, tid, nt, &lo, &hi);
    int end = hi + s->m - 1;
    if (end > s->n)
        end = s->n;
    int *fail = malloc((size_t)s->m * sizeof(int));
    check(fail != NULL, "alloc");
    kmp_table(s->pat, s->m, fail);
    s->cap[tid] = 16;
    s->found[tid] = malloc(16 * sizeof(int));
    s->nfound[tid] = 0;
    int k = 0;
    for (int i = lo; i < end; i++) {
        while (k && s->text[i] != s->pat[k])
            k = fail[k - 1];
        if (s->text[i] == s->pat[k])
            k++;
        if (k == s->m) {
            int start = i - s->m + 1;
            if (start >= lo && start < hi) {
                if (s->nfound[tid] == s->cap[tid]) {
                    s->cap[tid] *= 2;
                    s->found[tid] = realloc(s->found[tid], (size_t)s->cap[tid] * sizeof(int));
                }
                s->found[tid][s->nfound[tid]++] = start;
            }
            k = fail[k - 1];
        }
    }
    free(fail);
}

int main(void) {
    enum { N = 30000 };
    static unsigned char text[N];
    uint64_t seed = 271828;
    /* small alphabet so overlapping matches are common */
    for (int i = 0; i < N; i++)
        text[i] = (unsigned char)('a' + sm64(&seed) % 3u);
    const char *pats[] = {"abcab", "aaaa", "a", "cbacbacba", "abababab"};
    for (int p = 0; p < 5; p++) {
        Srch s;
        int plen = (int)strlen(pats[p]);
        if (plen > 1)
            for (int t = 1; t < NT; t++) {
                int lo, hi;
                par_range(N, t, NT, &lo, &hi);
                /* plant a copy that starts just before each chunk boundary */
                memcpy(text + lo - plen / 2, pats[p], (size_t)plen);
            }
        memset(&s, 0, sizeof s);
        s.text = text;
        s.n = N;
        s.pat = (const unsigned char *)pats[p];
        s.m = (int)strlen(pats[p]);
        par_run(NT, search_worker, &s);
        /* naive reference */
        int *ref = malloc(N * sizeof(int));
        int nref = 0;
        for (int i = 0; i + s.m <= N; i++)
            if (memcmp(text + i, pats[p], (size_t)s.m) == 0)
                ref[nref++] = i;
        int total = 0, straddle = 0;
        for (int t = 0; t < NT; t++) {
            for (int k = 0; k < s.nfound[t]; k++) {
                check(total < nref && s.found[t][k] == ref[total], "ordered match list");
                total++;
                int hi;
                int lo;
                par_range(N, t, NT, &lo, &hi);
                if (s.found[t][k] + s.m > hi)
                    straddle++;
            }
            free(s.found[t]);
        }
        check(total == nref, "match count");
        printf("pattern %-10s matches=%5d straddling boundaries=%d first=%d\n", pats[p], total,
               straddle, nref ? ref[0] : -1);
        free(ref);
    }
    return 0;
}
