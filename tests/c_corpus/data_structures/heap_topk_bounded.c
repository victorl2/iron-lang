/*
 * title: Bounded heaps for streaming top-k and uniform sampling
 * topic: data_structures
 * covers: bounded min-heap, streaming top-k, replacement counting, random-tag reservoir sample, total-order tie breaking, edge cases k=0 and k>n
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0x70B4ull;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    unsigned score;
    int id;
} Rec;

/* strict total order: higher score first, then smaller id */
static int better(Rec a, Rec b) { return a.score > b.score || (a.score == b.score && a.id < b.id); }

typedef struct {
    Rec *h; /* min-heap under "better": the root is the worst kept record */
    int n, k;
    long replaced;
} TopK;

static void down(TopK *t, int i) {
    Rec x = t->h[i];
    for (;;) {
        int c = 2 * i + 1;
        if (c >= t->n)
            break;
        if (c + 1 < t->n && better(t->h[c], t->h[c + 1]))
            c++; /* pick the worse child */
        if (!better(x, t->h[c]))
            break;
        t->h[i] = t->h[c];
        i = c;
    }
    t->h[i] = x;
}

static void offer(TopK *t, Rec r) {
    if (t->k == 0)
        return;
    if (t->n < t->k) {
        int i = t->n++;
        while (i > 0) {
            int p = (i - 1) / 2;
            if (!better(t->h[p], r))
                break;
            t->h[i] = t->h[p];
            i = p;
        }
        t->h[i] = r;
    } else if (better(r, t->h[0])) {
        t->h[0] = r;
        down(t, 0);
        t->replaced++;
    }
}

static int cmp_rec(const void *x, const void *y) {
    Rec a = *(const Rec *)x, b = *(const Rec *)y;
    if (better(a, b))
        return -1;
    if (better(b, a))
        return 1;
    return 0;
}

static int run_case(int n, int k, unsigned range, long *replaced_out, unsigned *best_out) {
    Rec *all = malloc(sizeof(Rec) * (size_t)(n + 1));
    TopK t = {malloc(sizeof(Rec) * (size_t)(k + 1)), 0, k, 0};
    for (int i = 0; i < n; i++) {
        all[i].score = rng() % range;
        all[i].id = i;
        offer(&t, all[i]);
    }
    qsort(all, (size_t)n, sizeof(Rec), cmp_rec);
    qsort(t.h, (size_t)t.n, sizeof(Rec), cmp_rec);
    int expect = k < n ? k : n;
    check(t.n == expect, "kept count");
    for (int i = 0; i < t.n; i++)
        check(t.h[i].id == all[i].id && t.h[i].score == all[i].score, "top-k equals sorted prefix");
    *replaced_out = t.replaced;
    *best_out = t.n ? t.h[0].score : 0;
    free(all);
    free(t.h);
    return t.n;
}

/* uniform sample without replacement: keep the k items with the smallest random tags */
static void sample_case(int n, int k, unsigned *checksum, int *first_id) {
    typedef struct {
        unsigned tag;
        int id;
    } Tg;
    Tg *heap = malloc(sizeof(Tg) * (size_t)k), *all = malloc(sizeof(Tg) * (size_t)n);
    int hn = 0;
    for (int i = 0; i < n; i++) {
        Tg g = {rng(), i};
        all[i] = g;
        /* max-heap on tag: root is the largest tag kept */
        if (hn < k) {
            int j = hn++;
            while (j > 0 && heap[(j - 1) / 2].tag < g.tag) {
                heap[j] = heap[(j - 1) / 2];
                j = (j - 1) / 2;
            }
            heap[j] = g;
        } else if (g.tag < heap[0].tag) {
            int j = 0;
            for (;;) {
                int c = 2 * j + 1;
                if (c >= hn)
                    break;
                if (c + 1 < hn && heap[c + 1].tag > heap[c].tag)
                    c++;
                if (heap[c].tag <= g.tag)
                    break;
                heap[j] = heap[c];
                j = c;
            }
            heap[j] = g;
        }
    }
    /* reference: partial selection by repeated minimum */
    unsigned mask_ref = 0, mask_got = 0;
    int used[2000] = {0};
    for (int r = 0; r < k; r++) {
        int bi = -1;
        for (int i = 0; i < n; i++)
            if (!used[i] && (bi < 0 || all[i].tag < all[bi].tag))
                bi = i;
        used[bi] = 1;
        mask_ref ^= (unsigned)bi * 2654435761u;
    }
    int lo = n;
    for (int i = 0; i < hn; i++) {
        mask_got ^= (unsigned)heap[i].id * 2654435761u;
        check(used[heap[i].id], "sample member is among the k smallest tags");
        if (heap[i].id < lo)
            lo = heap[i].id;
    }
    check(mask_ref == mask_got && hn == k, "sample equals reference selection");
    *checksum = mask_got;
    *first_id = lo;
    free(heap);
    free(all);
}

int main(void) {
    struct {
        int n, k;
        unsigned range;
    } cases[] = {{1000, 10, 100000}, {5000, 25, 1000000}, {2000, 100, 50}, {30, 50, 1000}, {500, 1, 1000}, {400, 0, 1000}, {3000, 3000, 7}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        long rep;
        unsigned best;
        int kept = run_case(cases[i].n, cases[i].k, cases[i].range, &rep, &best);
        printf("n=%-5d k=%-5d range=%-8u kept=%-5d replacements=%-5ld best=%u\n", cases[i].n, cases[i].k, cases[i].range, kept, rep, best);
    }
    int sizes[3][2] = {{1500, 20}, {1000, 1}, {700, 700}};
    for (int i = 0; i < 3; i++) {
        unsigned ck;
        int lo;
        sample_case(sizes[i][0], sizes[i][1], &ck, &lo);
        printf("sample n=%d k=%d checksum=%u min_id=%d\n", sizes[i][0], sizes[i][1], ck, lo);
    }
    return 0;
}
