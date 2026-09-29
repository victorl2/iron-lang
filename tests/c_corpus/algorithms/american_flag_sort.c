/*
 * title: American flag sort, in-place MSD radix on bytes
 * topic: algorithms
 * covers: in-place radix sort, cycle-leader permutation, bucket offsets, 64-bit keys, recursion per byte
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t st = 0xDEADBEEFCAFEF00DULL;
static uint64_t rng(void) {
    st ^= st << 13;
    st ^= st >> 7;
    st ^= st << 17;
    return st;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long swaps, buckets_used;

static void flag_sort(uint64_t *a, size_t n, int byte) {
    if (n < 2 || byte < 0)
        return;
    if (n <= 12) {
        for (size_t i = 1; i < n; i++) {
            uint64_t x = a[i];
            size_t j = i;
            while (j > 0 && a[j - 1] > x) {
                a[j] = a[j - 1];
                j--;
            }
            a[j] = x;
        }
        return;
    }
    int shift = 8 * byte;
    size_t count[256] = {0}, start[256], next[256], end[256];
    for (size_t i = 0; i < n; i++)
        count[(a[i] >> shift) & 0xff]++;
    size_t sum = 0;
    for (int b = 0; b < 256; b++) {
        start[b] = next[b] = sum;
        sum += count[b];
        end[b] = sum;
        if (count[b])
            buckets_used++;
    }
    /* permute in place: for each bucket, keep swapping the misplaced element into its home bucket */
    for (int b = 0; b < 256; b++) {
        while (next[b] < end[b]) {
            uint64_t v = a[next[b]];
            int home = (int)((v >> shift) & 0xff);
            if (home == b) {
                next[b]++;
            } else {
                uint64_t t = a[next[home]];
                a[next[home]++] = v;
                a[next[b]] = t;
                swaps++;
            }
        }
    }
    for (int b = 0; b < 256; b++)
        flag_sort(a + start[b], count[b], byte - 1);
}

int main(void) {
    static const size_t sizes[] = {0, 1, 13, 100, 3000, 30000};
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        size_t n = sizes[s];
        uint64_t *a = malloc(sizeof(uint64_t) * (n + 1));
        check(a != NULL, "alloc");
        uint64_t sum = 0, x = 0;
        for (size_t i = 0; i < n; i++) {
            a[i] = rng();
            if (s == 4)
                a[i] &= 0x0000ff00ff00ff00ULL; /* clustered high bytes */
            sum += a[i];
            x ^= a[i];
        }
        swaps = 0;
        buckets_used = 0;
        flag_sort(a, n, 7);
        uint64_t sum2 = 0, x2 = 0;
        for (size_t i = 0; i < n; i++) {
            if (i)
                check(a[i - 1] <= a[i], "sorted");
            sum2 += a[i];
            x2 ^= a[i];
        }
        check(sum == sum2 && x == x2, "permutation");
        printf("n=%-6zu swaps=%-7ld buckets=%-6ld first=%016llx last=%016llx\n", n, swaps, buckets_used,
               n ? (unsigned long long)a[0] : 0ULL, n ? (unsigned long long)a[n - 1] : 0ULL);
        free(a);
    }
    return 0;
}
