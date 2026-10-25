/*
 * title: Partial sort and top-k with a bounded heap
 * topic: algorithms
 * covers: partial sort, top-k selection, bounded max-heap, streaming input, heap sort of the prefix, tie-broken total order
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned score;
    unsigned id;
} Entry;

static unsigned long long st = 0xABCDEF0123456789ULL;
static unsigned rng(void) {
    st ^= st << 13;
    st ^= st >> 7;
    st ^= st << 17;
    return (unsigned)(st >> 20);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Total order: smaller score first, then smaller id. */
static int before(const Entry *a, const Entry *b) {
    return a->score != b->score ? a->score < b->score : a->id < b->id;
}

static void sift_down(Entry *h, int n, int i) {
    for (;;) {
        int l = 2 * i + 1, m = i;
        if (l < n && before(&h[m], &h[l]))
            m = l;
        if (l + 1 < n && before(&h[m], &h[l + 1]))
            m = l + 1;
        if (m == i)
            return;
        Entry t = h[i];
        h[i] = h[m];
        h[m] = t;
        i = m;
    }
}

/* Max-heap of the k best (smallest) seen so far: root is the worst of the best. */
static long replaced;

static void topk_stream(const Entry *in, int n, Entry *heap, int k) {
    int hn = 0;
    replaced = 0;
    for (int i = 0; i < n; i++) {
        if (hn < k) {
            heap[hn++] = in[i];
            if (hn == k)
                for (int j = k / 2 - 1; j >= 0; j--)
                    sift_down(heap, k, j);
        } else if (before(&in[i], &heap[0])) {
            heap[0] = in[i];
            sift_down(heap, k, 0);
            replaced++;
        }
    }
}

static void heap_sort_asc(Entry *h, int n) {
    for (int e = n - 1; e > 0; e--) {
        Entry t = h[0];
        h[0] = h[e];
        h[e] = t;
        sift_down(h, e, 0);
    }
}

/* std::partial_sort style: first k of a sorted in place, rest unspecified but a permutation. */
static void partial_sort(Entry *a, int n, int k) {
    for (int j = k / 2 - 1; j >= 0; j--)
        sift_down(a, k, j);
    for (int i = k; i < n; i++)
        if (before(&a[i], &a[0])) {
            Entry t = a[0];
            a[0] = a[i];
            a[i] = t;
            sift_down(a, k, 0);
        }
    heap_sort_asc(a, k);
}

static int cmp_entry(const void *x, const void *y) {
    const Entry *a = x, *b = y;
    return before(a, b) ? -1 : (before(b, a) ? 1 : 0);
}

int main(void) {
    enum { N = 10000 };
    static Entry in[N], ref[N], work[N];
    for (unsigned i = 0; i < N; i++) {
        in[i].score = rng() % 5000;
        in[i].id = i;
    }
    memcpy(ref, in, sizeof in);
    qsort(ref, N, sizeof(Entry), cmp_entry);

    static const int ks[] = {1, 5, 20, 100, 1000};
    for (int t = 0; t < 5; t++) {
        int k = ks[t];
        Entry *heap = malloc(sizeof(Entry) * (size_t)k);
        check(heap != NULL, "alloc");
        topk_stream(in, N, heap, k);
        long rep = replaced;
        heap_sort_asc(heap, k);
        for (int i = 0; i < k; i++)
            check(heap[i].score == ref[i].score && heap[i].id == ref[i].id, "top-k equals prefix of full sort");
        memcpy(work, in, sizeof in);
        partial_sort(work, N, k);
        for (int i = 0; i < k; i++)
            check(work[i].id == ref[i].id, "partial_sort prefix");
        /* rest is a permutation: sum of ids preserved */
        unsigned long long sum = 0;
        for (int i = 0; i < N; i++)
            sum += work[i].id;
        check(sum == (unsigned long long)N * (N - 1) / 2, "permutation");
        printf("k=%-5d replacements=%-6ld best=%u/%u kth=%u/%u\n", k, rep, heap[0].score, heap[0].id,
               heap[k - 1].score, heap[k - 1].id);
        free(heap);
    }
    return 0;
}
