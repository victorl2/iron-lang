/*
 * title: Iterative bottom-up merge sort with ping-pong buffers
 * topic: algorithms
 * covers: bottom-up merge sort, buffer swapping, width doubling, uint64 keys, stability with composite keys
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t st = 12345;
static uint64_t rng(void) {
    st ^= st >> 12;
    st ^= st << 25;
    st ^= st >> 27;
    return st * 0x2545F4914F6CDD1DULL;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    uint32_t key;
    uint32_t id;
} Rec;

static void merge_pass(const Rec *src, Rec *dst, size_t n, size_t width) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
        size_t mid = lo + width < n ? lo + width : n;
        size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
        size_t i = lo, j = mid, k = lo;
        while (i < mid && j < hi) {
            if (src[j].key < src[i].key)
                dst[k++] = src[j++];
            else
                dst[k++] = src[i++];
        }
        while (i < mid)
            dst[k++] = src[i++];
        while (j < hi)
            dst[k++] = src[j++];
    }
}

/* Returns pointer to the buffer holding the result and the number of passes. */
static Rec *sort(Rec *a, Rec *b, size_t n, int *passes) {
    Rec *src = a, *dst = b;
    *passes = 0;
    for (size_t w = 1; w < n; w *= 2) {
        merge_pass(src, dst, n, w);
        Rec *t = src;
        src = dst;
        dst = t;
        (*passes)++;
    }
    return src;
}

int main(void) {
    static const size_t sizes[] = {0, 1, 2, 3, 7, 8, 9, 100, 1023, 1024, 1025, 5000};
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        size_t n = sizes[s];
        Rec *a = malloc(sizeof(Rec) * (n + 1));
        Rec *b = malloc(sizeof(Rec) * (n + 1));
        check(a && b, "alloc");
        for (size_t i = 0; i < n; i++) {
            a[i].key = (uint32_t)(rng() % (n / 4 + 1));
            a[i].id = (uint32_t)i;
        }
        int passes;
        Rec *r = sort(a, b, n, &passes);
        uint64_t sum = 0;
        for (size_t i = 0; i < n; i++) {
            if (i) {
                check(r[i - 1].key <= r[i].key, "sorted");
                if (r[i - 1].key == r[i].key)
                    check(r[i - 1].id < r[i].id, "stable");
            }
            sum += (uint64_t)r[i].key * (i + 1) + r[i].id;
        }
        printf("n=%-5zu passes=%2d result_in=%s checksum=%llu\n", n, passes, r == a ? "a" : "b",
               (unsigned long long)sum);
        free(a);
        free(b);
    }
    return 0;
}
