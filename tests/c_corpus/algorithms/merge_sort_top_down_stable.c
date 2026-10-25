/*
 * title: Top-down merge sort with one auxiliary buffer
 * topic: algorithms
 * covers: merge sort, stability, insertion sort cutoff, sentinel-free merge, comparison counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned key;
    unsigned seq;
} Rec;

static unsigned long long st = 0x9E3779B97F4A7C15ULL;
static unsigned rng(void) {
    unsigned long long z = (st += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (unsigned)((z ^ (z >> 31)) >> 16);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long cmps;

static int less(const Rec *a, const Rec *b) {
    cmps++;
    return a->key < b->key;
}

static void insertion(Rec *a, int n) {
    for (int i = 1; i < n; i++) {
        Rec x = a[i];
        int j = i - 1;
        while (j >= 0 && less(&x, &a[j])) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
}

static void msort(Rec *a, Rec *tmp, int n, int cutoff) {
    if (n <= cutoff) {
        insertion(a, n);
        return;
    }
    int mid = n / 2;
    msort(a, tmp, mid, cutoff);
    msort(a + mid, tmp, n - mid, cutoff);
    /* skip merge if already ordered */
    if (!less(&a[mid], &a[mid - 1]))
        return;
    memcpy(tmp, a, sizeof(Rec) * (size_t)mid);
    int i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        if (less(&a[j], &tmp[i]))
            a[k++] = a[j++];
        else
            a[k++] = tmp[i++];
    }
    while (i < mid)
        a[k++] = tmp[i++];
}

int main(void) {
    enum { N = 3000 };
    static Rec base[N], a[N];
    Rec *tmp = malloc(sizeof(Rec) * (N / 2 + 1));
    check(tmp != NULL, "alloc");
    for (int i = 0; i < N; i++) {
        base[i].key = rng() % 100;
        base[i].seq = (unsigned)i;
    }
    static const int cutoffs[] = {1, 4, 8, 16, 32};
    for (int c = 0; c < 5; c++) {
        memcpy(a, base, sizeof a);
        cmps = 0;
        msort(a, tmp, N, cutoffs[c]);
        for (int i = 1; i < N; i++) {
            check(a[i - 1].key <= a[i].key, "sorted");
            if (a[i - 1].key == a[i].key)
                check(a[i - 1].seq < a[i].seq, "stable");
        }
        printf("cutoff=%2d comparisons=%ld\n", cutoffs[c], cmps);
    }
    /* already sorted input: only n/cutoff-ish comparisons thanks to the skip test */
    for (int i = 0; i < N; i++) {
        a[i].key = (unsigned)i;
        a[i].seq = (unsigned)i;
    }
    cmps = 0;
    msort(a, tmp, N, 8);
    printf("presorted comparisons=%ld\n", cmps);
    /* checksum of key/seq pairs in final order for the random case */
    memcpy(a, base, sizeof a);
    msort(a, tmp, N, 8);
    unsigned h = 2166136261u;
    for (int i = 0; i < N; i++) {
        h = (h ^ a[i].key) * 16777619u;
        h = (h ^ a[i].seq) * 16777619u;
    }
    printf("fnv=%08x min=%u max=%u\n", h, a[0].key, a[N - 1].key);
    free(tmp);
    return 0;
}
