/*
 * title: Parallel word frequency with hash-partitioned owner shards
 * topic: concurrency
 * covers: two-phase shuffle, per-owner tables, FNV hash routing, deterministic sorted output
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

enum { NT = 4, SHARDS = 4, NW = 6000, TAB = 512 };

typedef struct {
    char word[12];
    int count;
    int used;
} Ent;

typedef struct {
    Ent tab[TAB];
    int distinct;
} Shard;

typedef struct {
    const char (*words)[12];
    /* route[src][dst] lists word indices the source thread sends to shard dst */
    int *route[NT][SHARDS];
    int nroute[NT][SHARDS];
    Shard shard[SHARDS];
} Ctx;

static unsigned fnv(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void route_worker(void *ctx, int tid, int nt) {
    Ctx *c = ctx;
    int lo, hi;
    par_range(NW, tid, nt, &lo, &hi);
    for (int d = 0; d < SHARDS; d++) {
        c->route[tid][d] = malloc((size_t)(hi - lo + 1) * sizeof(int));
        c->nroute[tid][d] = 0;
    }
    for (int i = lo; i < hi; i++) {
        int d = (int)(fnv(c->words[i]) % SHARDS);
        c->route[tid][d][c->nroute[tid][d]++] = i;
    }
}

/* Each shard is owned by one thread: no locks; sources are read in fixed order. */
static void count_worker(void *ctx, int tid, int nt) {
    Ctx *c = ctx;
    Shard *sh = &c->shard[tid];
    (void)nt;
    for (int src = 0; src < NT; src++)
        for (int k = 0; k < c->nroute[src][tid]; k++) {
            const char *w = c->words[c->route[src][tid][k]];
            unsigned h = fnv(w) / SHARDS % TAB;
            for (;;) {
                Ent *e = &sh->tab[h];
                if (!e->used) {
                    e->used = 1;
                    strcpy(e->word, w);
                    e->count = 1;
                    sh->distinct++;
                    break;
                }
                if (strcmp(e->word, w) == 0) {
                    e->count++;
                    break;
                }
                h = (h + 1) % TAB;
            }
        }
}

typedef struct {
    char word[12];
    int count;
} Out;

static int cmp_out(const void *x, const void *y) {
    const Out *a = x, *b = y;
    if (a->count != b->count)
        return b->count - a->count;
    return strcmp(a->word, b->word);
}

int main(void) {
    static const char *syl[] = {"ka", "lo", "mi", "ne", "ru", "sa", "to", "vi", "za", "be"};
    static char words[NW][12];
    uint64_t seed = 5150;
    for (int i = 0; i < NW; i++) {
        /* zipf-like: pick a word id with a skewed distribution */
        uint64_t r = sm64(&seed);
        unsigned a = (unsigned)(r % 100u), b = (unsigned)((r >> 16) % 100u);
        unsigned id = (a * b) / 60u; /* 0..165, heavy toward small */
        snprintf(words[i], sizeof words[i], "%s%s%u", syl[id % 10], syl[(id / 10) % 10], id / 100u);
    }
    static Ctx c;
    c.words = (const char(*)[12])words;
    par_run(NT, route_worker, &c);
    par_run(SHARDS, count_worker, &c);
    for (int s = 0; s < NT; s++)
        for (int d = 0; d < SHARDS; d++)
            free(c.route[s][d]);
    /* gather */
    static Out out[SHARDS * TAB];
    int nout = 0;
    long total = 0;
    for (int s = 0; s < SHARDS; s++)
        for (int i = 0; i < TAB; i++)
            if (c.shard[s].tab[i].used) {
                strcpy(out[nout].word, c.shard[s].tab[i].word);
                out[nout].count = c.shard[s].tab[i].count;
                total += out[nout].count;
                nout++;
            }
    check(total == NW, "every word counted once");
    qsort(out, (size_t)nout, sizeof out[0], cmp_out);
    /* reference: quadratic count using the sorted order as the key list */
    for (int i = 0; i < nout; i++) {
        int n = 0;
        for (int k = 0; k < NW; k++)
            if (strcmp(words[k], out[i].word) == 0)
                n++;
        check(n == out[i].count, "count equals brute force");
    }
    printf("words=%d distinct=%d\n", NW, nout);
    for (int s = 0; s < SHARDS; s++)
        printf("shard %d distinct=%d\n", s, c.shard[s].distinct);
    for (int i = 0; i < 10; i++)
        printf("%2d. %-6s %d\n", i + 1, out[i].word, out[i].count);
    printf("least frequent: %s %d\n", out[nout - 1].word, out[nout - 1].count);
    return 0;
}
