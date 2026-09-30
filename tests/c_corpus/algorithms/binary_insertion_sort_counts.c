/*
 * title: Binary insertion sort, fewer comparisons but same moves
 * topic: algorithms
 * covers: binary insertion sort, upper bound search for stability, memmove shifting, information-theoretic lower bound
 * deps: libc, libm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int key;
    int seq;
} Rec;

static unsigned st = 24680u;
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

static long cmps, moves;

static void linear(Rec *a, int n) {
    for (int i = 1; i < n; i++) {
        Rec x = a[i];
        int j = i - 1;
        while (j >= 0) {
            cmps++;
            if (a[j].key <= x.key)
                break;
            a[j + 1] = a[j];
            moves++;
            j--;
        }
        a[j + 1] = x;
    }
}

static void binary(Rec *a, int n) {
    for (int i = 1; i < n; i++) {
        Rec x = a[i];
        int lo = 0, hi = i; /* find first index with key > x.key: upper bound, keeps stability */
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            cmps++;
            if (x.key < a[mid].key)
                hi = mid;
            else
                lo = mid + 1;
        }
        if (lo < i) {
            memmove(&a[lo + 1], &a[lo], sizeof(Rec) * (size_t)(i - lo));
            moves += i - lo;
        }
        a[lo] = x;
    }
}

int main(void) {
    static const int sizes[] = {16, 128, 1024};
    for (int s = 0; s < 3; s++) {
        int n = sizes[s];
        Rec *base = malloc(sizeof(Rec) * (size_t)n), *a = malloc(sizeof(Rec) * (size_t)n);
        Rec *b = malloc(sizeof(Rec) * (size_t)n);
        check(base && a && b, "alloc");
        for (int shape = 0; shape < 3; shape++) {
            for (int i = 0; i < n; i++) {
                base[i].key = shape == 0 ? (int)(rng() % (unsigned)(n * 2)) : shape == 1 ? n - i : (int)(rng() % 8);
                base[i].seq = i;
            }
            memcpy(a, base, sizeof(Rec) * (size_t)n);
            memcpy(b, base, sizeof(Rec) * (size_t)n);
            cmps = moves = 0;
            linear(a, n);
            long lc = cmps, lm = moves;
            cmps = moves = 0;
            binary(b, n);
            long bc = cmps, bm = moves;
            for (int i = 0; i < n; i++) {
                check(a[i].key == b[i].key && a[i].seq == b[i].seq, "identical results");
                if (i && a[i - 1].key == a[i].key)
                    check(a[i - 1].seq < a[i].seq, "stable");
            }
            check(lm == bm, "same number of element moves");
            double lb = 0;
            for (int i = 2; i <= n; i++)
                lb += log2((double)i);
            printf("n=%-5d %-9s linear cmps=%-8ld binary cmps=%-7ld moves=%-8ld lower bound~%.0f\n", n,
                   shape == 0 ? "random" : shape == 1 ? "reversed" : "few keys", lc, bc, bm, ceil(lb));
        }
        free(base);
        free(a);
        free(b);
    }
    return 0;
}
