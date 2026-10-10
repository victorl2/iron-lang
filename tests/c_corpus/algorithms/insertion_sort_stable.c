/*
 * title: Stable insertion sort on records
 * topic: algorithms
 * covers: insertion sort, stability, comparison counting, invariant checks
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int key;
    int seq; /* original position, used to verify stability */
} Rec;

static unsigned rng_state = 12345u;

static unsigned rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static long insertion_sort(Rec *a, int n) {
    long cmps = 0;
    for (int i = 1; i < n; i++) {
        Rec x = a[i];
        int j = i - 1;
        while (j >= 0) {
            cmps++;
            if (a[j].key <= x.key)
                break;
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
    return cmps;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

int main(void) {
    enum { N = 200 };
    Rec a[N];
    for (int i = 0; i < N; i++) {
        a[i].key = (int)(rng() % 20);
        a[i].seq = i;
    }
    long cmps = insertion_sort(a, N);
    for (int i = 1; i < N; i++) {
        check(a[i - 1].key <= a[i].key, "sorted");
        if (a[i - 1].key == a[i].key)
            check(a[i - 1].seq < a[i].seq, "stable");
    }
    Rec sorted[5] = {{1, 0}, {2, 1}, {3, 2}, {4, 3}, {5, 4}};
    check(insertion_sort(sorted, 5) == 4, "best case is n-1 comparisons");

    printf("n=%d comparisons=%ld\n", N, cmps);
    printf("first:");
    for (int i = 0; i < 8; i++)
        printf(" %d/%d", a[i].key, a[i].seq);
    printf("\nlast:");
    for (int i = N - 8; i < N; i++)
        printf(" %d/%d", a[i].key, a[i].seq);
    printf("\n");
    return 0;
}
