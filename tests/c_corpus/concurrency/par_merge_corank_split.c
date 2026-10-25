/*
 * title: Parallel merge of two sorted arrays using co-ranking
 * topic: concurrency
 * covers: co-rank binary search, output-range partition, stable merge of records, duplicate keys, empty inputs
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

typedef struct {
    int key;
    int src; /* 0 = from A, 1 = from B */
    int pos;
} Rec;

typedef struct {
    const Rec *a;
    int na;
    const Rec *b;
    int nb;
    Rec *out;
} Mg;

/* Find i (elements taken from A) such that the first k outputs of the stable
 * merge consist of a[0..i) and b[0..k-i). Ties favour A. */
static int corank(int k, const Rec *a, int na, const Rec *b, int nb) {
    int lo = k > nb ? k - nb : 0;
    int hi = k < na ? k : na;
    for (;;) {
        int i = lo + (hi - lo) / 2;
        int j = k - i;
        if (i > 0 && j < nb && a[i - 1].key > b[j].key)
            hi = i - 1; /* took too many from A: b[j] must come first */
        else if (j > 0 && i < na && b[j - 1].key >= a[i].key)
            lo = i + 1; /* took too few from A: a[i] precedes b[j-1] on ties */
        else
            return i;
    }
}

static void seq_merge(const Rec *a, int na, const Rec *b, int nb, Rec *out) {
    int i = 0, j = 0, k = 0;
    while (i < na && j < nb)
        out[k++] = (b[j].key < a[i].key) ? b[j++] : a[i++];
    while (i < na)
        out[k++] = a[i++];
    while (j < nb)
        out[k++] = b[j++];
}

static void worker(void *ctx, int tid, int nt) {
    Mg *m = ctx;
    int total = m->na + m->nb;
    int k0 = (int)((long)total * tid / nt), k1 = (int)((long)total * (tid + 1) / nt);
    int i0 = corank(k0, m->a, m->na, m->b, m->nb), j0 = k0 - i0;
    int i1 = corank(k1, m->a, m->na, m->b, m->nb), j1 = k1 - i1;
    seq_merge(m->a + i0, i1 - i0, m->b + j0, j1 - j0, m->out + k0);
}

static void fill(Rec *r, int n, int src, int range, uint64_t *seed) {
    int key = 0;
    for (int i = 0; i < n; i++) {
        key += (int)(sm64(seed) % (unsigned)range);
        r[i].key = key;
        r[i].src = src;
        r[i].pos = i;
    }
}

int main(void) {
    uint64_t seed = 2718;
    int cases[][3] = {{0, 0, 3}, {0, 500, 3}, {500, 0, 3}, {1, 1, 2}, {1000, 1000, 1},
                      {1000, 1000, 50}, {3000, 17, 4}, {17, 3000, 4}, {4096, 4099, 2}};
    for (int c = 0; c < 9; c++) {
        int na = cases[c][0], nb = cases[c][1], range = cases[c][2];
        Rec *a = malloc((size_t)(na + 1) * sizeof(Rec));
        Rec *b = malloc((size_t)(nb + 1) * sizeof(Rec));
        Rec *out = malloc((size_t)(na + nb + 1) * sizeof(Rec));
        Rec *ref = malloc((size_t)(na + nb + 1) * sizeof(Rec));
        check(a && b && out && ref, "alloc");
        fill(a, na, 0, range, &seed);
        fill(b, nb, 1, range, &seed);
        seq_merge(a, na, b, nb, ref);
        int ties = 0;
        for (int nt = 1; nt <= 8; nt++) {
            Mg m = {a, na, b, nb, out};
            memset(out, 0, (size_t)(na + nb + 1) * sizeof(Rec));
            par_run(nt, worker, &m);
            check(memcmp(out, ref, (size_t)(na + nb) * sizeof(Rec)) == 0, "parallel merge equals sequential");
        }
        for (int i = 1; i < na + nb; i++) {
            check(ref[i - 1].key <= ref[i].key, "sorted");
            if (ref[i - 1].key == ref[i].key) {
                ties++;
                /* stability: A before B, and original order within a source */
                check(ref[i - 1].src < ref[i].src || (ref[i - 1].src == ref[i].src && ref[i - 1].pos < ref[i].pos),
                      "stable ties");
            }
        }
        printf("|A|=%4d |B|=%4d range=%2d ties=%4d", na, nb, range, ties);
        if (na + nb)
            printf(" first=(%d,%c) last=(%d,%c)", ref[0].key, ref[0].src ? 'B' : 'A', ref[na + nb - 1].key,
                   ref[na + nb - 1].src ? 'B' : 'A');
        printf("\n");
        free(a);
        free(b);
        free(out);
        free(ref);
    }
    return 0;
}
