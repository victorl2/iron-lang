/*
 * title: In-place heapsort with heap invariant checks
 * topic: algorithms
 * covers: binary heap, sift down, Floyd heap construction, invariant verification, generic element size
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 1u << 20;
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

typedef int (*Cmp)(const void *, const void *);

static void swap_bytes(unsigned char *a, unsigned char *b, size_t sz) {
    unsigned char t;
    while (sz--) {
        t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift_down(unsigned char *base, size_t sz, size_t n, size_t i, Cmp cmp) {
    for (;;) {
        size_t l = 2 * i + 1;
        if (l >= n)
            return;
        size_t big = l;
        if (l + 1 < n && cmp(base + (l + 1) * sz, base + l * sz) > 0)
            big = l + 1;
        if (cmp(base + big * sz, base + i * sz) <= 0)
            return;
        swap_bytes(base + big * sz, base + i * sz, sz);
        i = big;
    }
}

static int is_max_heap(const unsigned char *base, size_t sz, size_t n, Cmp cmp) {
    for (size_t i = 1; i < n; i++)
        if (cmp(base + ((i - 1) / 2) * sz, base + i * sz) < 0)
            return 0;
    return 1;
}

static void heap_sort(void *v, size_t n, size_t sz, Cmp cmp) {
    unsigned char *base = v;
    if (n < 2)
        return;
    for (size_t i = n / 2; i-- > 0;)
        sift_down(base, sz, n, i, cmp);
    check(is_max_heap(base, sz, n, cmp), "heap after build");
    for (size_t e = n - 1; e > 0; e--) {
        swap_bytes(base, base + e * sz, sz);
        sift_down(base, sz, e, 0, cmp);
    }
}

static int cmp_byte(const void *a, const void *b) {
    return (int)*(const unsigned char *)a - (int)*(const unsigned char *)b;
}

typedef struct {
    double weight;
    int id;
    char label[6];
} Thing;

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int cmp_thing(const void *a, const void *b) {
    const Thing *x = a, *y = b;
    if (x->weight != y->weight)
        return x->weight < y->weight ? -1 : 1;
    return (x->id > y->id) - (x->id < y->id);
}

int main(void) {
    enum { N = 1500 };
    static int a[N];
    for (int i = 0; i < N; i++)
        a[i] = (int)(rng() % 10000) - 5000;
    heap_sort(a, N, sizeof a[0], cmp_int);
    for (int i = 1; i < N; i++)
        check(a[i - 1] <= a[i], "ints sorted");
    printf("ints: min=%d max=%d median=%d\n", a[0], a[N - 1], a[N / 2]);

    Thing t[40];
    for (int i = 0; i < 40; i++) {
        t[i].weight = (double)(rng() % 64) / 4.0;
        t[i].id = i;
        snprintf(t[i].label, sizeof t[i].label, "T%02d", i);
    }
    heap_sort(t, 40, sizeof t[0], cmp_thing);
    for (int i = 1; i < 40; i++)
        check(cmp_thing(&t[i - 1], &t[i]) < 0, "things strictly ordered");
    printf("things:");
    for (int i = 0; i < 10; i++)
        printf(" %s(%.2f)", t[i].label, t[i].weight);
    printf("\n");

    unsigned char bytes[] = {9, 3, 250, 0, 128, 77, 3, 200};
    heap_sort(bytes, sizeof bytes, 1, cmp_byte);
    printf("bytes:");
    for (unsigned i = 0; i < sizeof bytes; i++) {
        if (i)
            check(bytes[i - 1] <= bytes[i], "bytes sorted");
        printf(" %u", (unsigned)bytes[i]);
    }
    printf("\n");
    return 0;
}
