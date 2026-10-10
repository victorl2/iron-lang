/*
 * title: Fail-the-Nth-allocation sweep over a string table
 * topic: memory
 * covers: failure injection, allocator wrapper, exhaustive failure points, leak accounting, error codes
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- failing allocator ---- */
static long alloc_calls, fail_at, live_blocks, live_bytes;
typedef struct { size_t n; } Hdr;
static void *xmalloc(size_t n) {
    alloc_calls++;
    if (fail_at && alloc_calls == fail_at) return NULL;
    Hdr *h = malloc(32 + n);
    if (!h) return NULL;
    h->n = n;
    live_blocks++; live_bytes += (long)n;
    return (unsigned char *)h + 32;
}
static void xfree(void *p) {
    if (!p) return;
    Hdr *h = (Hdr *)((unsigned char *)p - 32);
    live_blocks--; live_bytes -= (long)h->n;
    free(h);
}

enum { OK = 0, E_NOMEM = 1, E_DUP = 2 };

typedef struct Entry { char *key; int *vals; int nvals; struct Entry *next; } Entry;
typedef struct { Entry **buckets; int nb; int count; } Table;

static unsigned hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static void table_free(Table *t) {
    if (!t->buckets) return;
    for (int i = 0; i < t->nb; i++) {
        Entry *e = t->buckets[i];
        while (e) { Entry *n = e->next; xfree(e->key); xfree(e->vals); xfree(e); e = n; }
    }
    xfree(t->buckets);
    t->buckets = NULL; t->count = 0;
}

static int table_init(Table *t, int nb) {
    t->buckets = xmalloc((size_t)nb * sizeof(Entry *));
    if (!t->buckets) return E_NOMEM;
    memset(t->buckets, 0, (size_t)nb * sizeof(Entry *));
    t->nb = nb; t->count = 0;
    return OK;
}

/* Insert with three separate allocations; each failure path frees exactly what it made. */
static int table_add(Table *t, const char *key, int nvals, int seed) {
    unsigned b = hash(key) % (unsigned)t->nb;
    for (Entry *e = t->buckets[b]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return E_DUP;
    Entry *e = xmalloc(sizeof *e);
    if (!e) return E_NOMEM;
    e->key = xmalloc(strlen(key) + 1);
    if (!e->key) { xfree(e); return E_NOMEM; }
    strcpy(e->key, key);
    e->vals = xmalloc((size_t)nvals * sizeof(int));
    if (!e->vals) { xfree(e->key); xfree(e); return E_NOMEM; }
    for (int i = 0; i < nvals; i++) e->vals[i] = seed + i;
    e->nvals = nvals;
    e->next = t->buckets[b];
    t->buckets[b] = e;
    t->count++;
    return OK;
}

static const char *WORDS[] = {"alpha", "beta", "gamma", "delta", "alpha", "epsilon", "zeta", "beta", "eta", "theta"};

/* The whole workload: returns OK / E_NOMEM, and leaves nothing allocated either way. */
static int workload(long *sum_out) {
    Table t = {0};
    int rc = table_init(&t, 4);
    if (rc) return rc;
    int dups = 0;
    for (int i = 0; i < 10; i++) {
        rc = table_add(&t, WORDS[i], i + 1, i * 10);
        if (rc == E_DUP) { dups++; rc = OK; }
        if (rc) { table_free(&t); return rc; }
    }
    long sum = dups;
    for (int i = 0; i < t.nb; i++)
        for (Entry *e = t.buckets[i]; e; e = e->next)
            for (int k = 0; k < e->nvals; k++) sum += e->vals[k];
    *sum_out = sum + t.count * 1000;
    table_free(&t);
    return OK;
}

int main(void) {
    long ref = 0;
    fail_at = 0; alloc_calls = 0;
    if (workload(&ref) != OK || live_blocks != 0) { fprintf(stderr, "baseline broken\n"); return 1; }
    long total = alloc_calls;
    printf("baseline: %ld allocations, result %ld\n", total, ref);

    int nomem = 0, stage_hist[4] = {0};
    for (long n = 1; n <= total + 3; n++) {
        long before_calls;
        fail_at = n; alloc_calls = 0;
        long out = -1;
        int rc = workload(&out);
        before_calls = alloc_calls;
        if (live_blocks != 0 || live_bytes != 0) { fprintf(stderr, "leak at N=%ld: %ld blocks\n", n, live_blocks); return 1; }
        if (n <= total) {
            if (rc != E_NOMEM || out != -1) { fprintf(stderr, "wrong rc at N=%ld\n", n); return 1; }
            nomem++;
            /* how far did it get before the injected failure? */
            stage_hist[(before_calls - 1) * 4 / total]++;
        } else if (rc != OK || out != ref) { fprintf(stderr, "beyond-range run wrong\n"); return 1; }
    }
    printf("failure points tried: %d, all returned NOMEM with zero leaks\n", nomem);
    for (int i = 0; i < 4; i++) printf("quartile %d: %d failures\n", i + 1, stage_hist[i]);
    fail_at = 0;
    return 0;
}
