/*
 * title: d-ary heap arity sweep
 * topic: data_structures
 * covers: d-ary heap, index arithmetic, arity trade-off, comparison counting, model cross-check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 424242;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    int *a;
    int n, d;
    long cmp, moves;
} DHeap;

static void push(DHeap *h, int v) {
    int i = h->n++;
    while (i > 0) {
        int p = (i - 1) / h->d;
        h->cmp++;
        if (h->a[p] <= v)
            break;
        h->a[i] = h->a[p];
        h->moves++;
        i = p;
    }
    h->a[i] = v;
}

static int pop(DHeap *h) {
    int top = h->a[0], x = h->a[--h->n], i = 0;
    for (;;) {
        int first = i * h->d + 1;
        if (first >= h->n)
            break;
        int best = first, last = first + h->d;
        if (last > h->n)
            last = h->n;
        for (int c = first + 1; c < last; c++) {
            h->cmp++;
            if (h->a[c] < h->a[best])
                best = c;
        }
        h->cmp++;
        if (h->a[best] >= x)
            break;
        h->a[i] = h->a[best];
        h->moves++;
        i = best;
    }
    if (h->n > 0)
        h->a[i] = x;
    return top;
}

static void invariant(const DHeap *h) {
    for (int i = 1; i < h->n; i++)
        check(h->a[(i - 1) / h->d] <= h->a[i], "d-ary heap order");
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    enum { N = 3000 };
    static int data[N], sorted[N], mix[N];
    for (int i = 0; i < N; i++)
        data[i] = (int)(rng() % 50000);
    for (int i = 0; i < N; i++)
        sorted[i] = data[i];
    qsort(sorted, N, sizeof(int), cmp_int);

    printf("%-3s %-9s %-9s %-7s\n", "d", "cmp", "moves", "height");
    for (int d = 2; d <= 8; d++) {
        DHeap h = {malloc(sizeof(int) * N), 0, d, 0, 0};
        for (int i = 0; i < N; i++) {
            push(&h, data[i]);
            if (i % 250 == 0)
                invariant(&h);
        }
        int height = 0;
        for (long span = 1, cap = 1; cap < N; span *= d, cap += span)
            height++;
        for (int i = 0; i < N; i++) {
            check(pop(&h) == sorted[i], "pops come out sorted");
            if (i % 250 == 0)
                invariant(&h);
        }
        printf("%-3d %-9ld %-9ld %-7d\n", d, h.cmp, h.moves, height);
        free(h.a);
    }

    /* interleaved workload against a brute-force model, d = 5 */
    DHeap h = {malloc(sizeof(int) * N), 0, 5, 0, 0};
    int mn = 0;
    long sum = 0;
    for (int op = 0; op < 6000; op++) {
        if (mn == 0 || (mn < N && rng() % 2)) {
            int v = (int)(rng() % 500);
            mix[mn++] = v;
            push(&h, v);
        } else {
            int bi = 0;
            for (int i = 1; i < mn; i++)
                if (mix[i] < mix[bi])
                    bi = i;
            int want = mix[bi];
            mix[bi] = mix[--mn];
            int got = pop(&h);
            check(got == want, "interleaved pop equals model");
            sum += got;
        }
        invariant(&h);
    }
    printf("interleaved d=5 final_size=%d pop_sum=%ld\n", h.n, sum);
    free(h.a);
    return 0;
}
