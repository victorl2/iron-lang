/*
 * title: External-style k-way merge with a min-heap of run cursors
 * topic: algorithms
 * covers: external sorting, chunked run generation, k-way merge, min-heap of cursors, block buffered reads, tie-breaking by run index
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned key;
    unsigned id;
} Rec;

static unsigned st = 8080808u;
static unsigned rng(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* A "disk run": sorted data that can only be read in fixed-size blocks. */
typedef struct {
    Rec *data;
    int len;
    int pos;     /* next unread record on "disk" */
    Rec block[8];
    int bcount, bpos;
    long block_reads;
} Run;

static void refill(Run *r) {
    int take = r->len - r->pos < 8 ? r->len - r->pos : 8;
    memcpy(r->block, r->data + r->pos, sizeof(Rec) * (size_t)take);
    r->pos += take;
    r->bcount = take;
    r->bpos = 0;
    if (take)
        r->block_reads++;
}

static int has_next(Run *r) {
    if (r->bpos == r->bcount)
        refill(r);
    return r->bcount > 0;
}

typedef struct {
    Rec rec;
    int run;
} HeapItem;

static int hless(const HeapItem *a, const HeapItem *b) {
    if (a->rec.key != b->rec.key)
        return a->rec.key < b->rec.key;
    return a->run < b->run; /* earlier run first: keeps the merge stable across runs */
}

static void sift_down(HeapItem *h, int n, int i) {
    for (;;) {
        int l = 2 * i + 1, m = i;
        if (l < n && hless(&h[l], &h[m]))
            m = l;
        if (l + 1 < n && hless(&h[l + 1], &h[m]))
            m = l + 1;
        if (m == i)
            return;
        HeapItem t = h[i];
        h[i] = h[m];
        h[m] = t;
        i = m;
    }
}

static int cmp_rec(const void *a, const void *b) {
    const Rec *x = a, *y = b;
    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    return (x->id > y->id) - (x->id < y->id); /* id is unique: total order */
}

int main(void) {
    enum { N = 5000, MEM = 700 };
    Rec *all = malloc(sizeof(Rec) * N);
    Rec *out = malloc(sizeof(Rec) * N);
    check(all && out, "alloc");
    for (unsigned i = 0; i < N; i++) {
        all[i].key = rng() % 2000;
        all[i].id = i;
    }
    /* Phase 1: sort memory-sized chunks into runs (chunks sorted in place: run i = all[i*MEM ..]) */
    int nruns = (N + MEM - 1) / MEM;
    Run *runs = calloc((size_t)nruns, sizeof(Run));
    check(runs != NULL, "alloc");
    Rec *sorted_input = malloc(sizeof(Rec) * N);
    check(sorted_input != NULL, "alloc");
    memcpy(sorted_input, all, sizeof(Rec) * N);
    for (int r = 0; r < nruns; r++) {
        int lo = r * MEM, len = N - lo < MEM ? N - lo : MEM;
        qsort(sorted_input + lo, (size_t)len, sizeof(Rec), cmp_rec);
        runs[r].data = sorted_input + lo;
        runs[r].len = len;
    }
    /* Phase 2: k-way merge, one record at a time, with block-buffered cursors. */
    HeapItem *heap = malloc(sizeof(HeapItem) * (size_t)nruns);
    check(heap != NULL, "alloc");
    int hn = 0;
    for (int r = 0; r < nruns; r++)
        if (has_next(&runs[r])) {
            heap[hn].rec = runs[r].block[runs[r].bpos++];
            heap[hn].run = r;
            hn++;
        }
    for (int i = hn / 2 - 1; i >= 0; i--)
        sift_down(heap, hn, i);
    int outn = 0;
    while (hn > 0) {
        out[outn++] = heap[0].rec;
        int r = heap[0].run;
        if (has_next(&runs[r])) {
            heap[0].rec = runs[r].block[runs[r].bpos++];
        } else {
            heap[0] = heap[--hn];
        }
        sift_down(heap, hn, 0);
    }
    check(outn == N, "all records emitted");
    /* Verify: sorted by key; ties resolved by run order, and within a run by id order => same as (key,id). */
    for (int i = 1; i < N; i++)
        check(cmp_rec(&out[i - 1], &out[i]) < 0, "globally ordered by (key,id)");
    long reads = 0;
    for (int r = 0; r < nruns; r++)
        reads += runs[r].block_reads;
    printf("records=%d run_size=%d runs=%d block_reads=%ld\n", N, MEM, nruns, reads);
    printf("head:");
    for (int i = 0; i < 6; i++)
        printf(" %u/%u", out[i].key, out[i].id);
    printf("\ntail:");
    for (int i = N - 4; i < N; i++)
        printf(" %u/%u", out[i].key, out[i].id);
    printf("\n");
    free(all);
    free(out);
    free(runs);
    free(sorted_input);
    free(heap);
    return 0;
}
