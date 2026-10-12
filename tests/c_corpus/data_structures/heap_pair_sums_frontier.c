/*
 * title: Heap frontier for k smallest sums and sorted-matrix selection
 * topic: data_structures
 * covers: lazy frontier expansion, k smallest pair sums, visited bitset, three-way sums, sorted matrix kth element, brute-force cross-check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0x5EA1ull;
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
    long sum;
    int i, j, k;
} St;

typedef struct {
    St *a;
    int n, cap;
    int peak;
} PQ;

static int less(St x, St y) {
    if (x.sum != y.sum)
        return x.sum < y.sum;
    if (x.i != y.i)
        return x.i < y.i;
    if (x.j != y.j)
        return x.j < y.j;
    return x.k < y.k;
}

static void push(PQ *q, St s) {
    if (q->n == q->cap) {
        q->cap = q->cap ? 2 * q->cap : 16;
        q->a = realloc(q->a, sizeof(St) * (size_t)q->cap);
    }
    int i = q->n++;
    while (i > 0 && less(s, q->a[(i - 1) / 2])) {
        q->a[i] = q->a[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    q->a[i] = s;
    if (q->n > q->peak)
        q->peak = q->n;
}

static St pop(PQ *q) {
    St top = q->a[0], x = q->a[--q->n];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= q->n)
            break;
        if (c + 1 < q->n && less(q->a[c + 1], q->a[c]))
            c++;
        if (!less(q->a[c], x))
            break;
        q->a[i] = q->a[c];
        i = c;
    }
    if (q->n > 0)
        q->a[i] = x;
    return top;
}

static int cmp_long(const void *x, const void *y) {
    long a = *(const long *)x, b = *(const long *)y;
    return (a > b) - (a < b);
}

static void sorted_ints(int *v, int n, unsigned range) {
    long cur = 0;
    for (int i = 0; i < n; i++) {
        cur += rng() % range;
        v[i] = (int)cur;
    }
}

int main(void) {
    enum { NA = 200, NB = 150, NC = 40 };
    static int A[NA], B[NB], C[NC];
    sorted_ints(A, NA, 20);
    sorted_ints(B, NB, 30);
    sorted_ints(C, NC, 100);

    /* two lists: k smallest a[i]+b[j] */
    {
        static unsigned char seen[NA][NB];
        static long all[NA * NB];
        int m = 0;
        for (int i = 0; i < NA; i++)
            for (int j = 0; j < NB; j++)
                all[m++] = (long)A[i] + B[j];
        qsort(all, (size_t)m, sizeof(long), cmp_long);
        PQ q = {0};
        push(&q, (St){(long)A[0] + B[0], 0, 0, 0});
        seen[0][0] = 1;
        int K = 500;
        long sum = 0, last = 0;
        for (int t = 0; t < K; t++) {
            St s = pop(&q);
            check(s.sum == all[t], "pair sums come out in sorted order");
            sum += s.sum;
            last = s.sum;
            if (s.i + 1 < NA && !seen[s.i + 1][s.j]) {
                seen[s.i + 1][s.j] = 1;
                push(&q, (St){(long)A[s.i + 1] + B[s.j], s.i + 1, s.j, 0});
            }
            if (s.j + 1 < NB && !seen[s.i][s.j + 1]) {
                seen[s.i][s.j + 1] = 1;
                push(&q, (St){(long)A[s.i] + B[s.j + 1], s.i, s.j + 1, 0});
            }
        }
        printf("pairs: K=%d sum=%ld kth=%ld frontier_peak=%d (grid has %d cells)\n", K, sum, last, q.peak, NA * NB);
        free(q.a);
    }

    /* three lists: k smallest a+b+c using a 3D visited set */
    {
        static unsigned char seen[NA][NB][NC];
        long *big = malloc(sizeof(long) * (size_t)NA * NB * NC);
        int m = 0;
        for (int i = 0; i < NA; i++)
            for (int j = 0; j < NB; j++)
                for (int k = 0; k < NC; k++)
                    big[m++] = (long)A[i] + B[j] + C[k];
        qsort(big, (size_t)m, sizeof(long), cmp_long);
        PQ q = {0};
        push(&q, (St){(long)A[0] + B[0] + C[0], 0, 0, 0});
        seen[0][0][0] = 1;
        int K = 1000;
        long sum = 0, last = 0;
        for (int t = 0; t < K; t++) {
            St s = pop(&q);
            check(s.sum == big[t], "triple sums come out in sorted order");
            sum += s.sum;
            last = s.sum;
            int di[3] = {1, 0, 0}, dj[3] = {0, 1, 0}, dk[3] = {0, 0, 1};
            for (int d = 0; d < 3; d++) {
                int i = s.i + di[d], j = s.j + dj[d], k = s.k + dk[d];
                if (i < NA && j < NB && k < NC && !seen[i][j][k]) {
                    seen[i][j][k] = 1;
                    push(&q, (St){(long)A[i] + B[j] + C[k], i, j, k});
                }
            }
        }
        printf("triples: K=%d sum=%ld kth=%ld frontier_peak=%d\n", K, sum, last, q.peak);
        free(q.a);
        free(big);
    }

    /* kth smallest in a row- and column-sorted matrix, by frontier of row heads */
    {
        enum { R = 60, Cc = 45 };
        static int mat[R][Cc];
        for (int i = 0; i < R; i++)
            for (int j = 0; j < Cc; j++) {
                int up = i ? mat[i - 1][j] : 0, left = j ? mat[i][j - 1] : 0;
                mat[i][j] = (up > left ? up : left) + 1 + (int)(rng() % 9);
            }
        static long flat[R * Cc];
        int m = 0;
        for (int i = 0; i < R; i++)
            for (int j = 0; j < Cc; j++)
                flat[m++] = mat[i][j];
        qsort(flat, (size_t)m, sizeof(long), cmp_long);
        int ks[] = {1, 2, 100, 1000, R * Cc};
        for (size_t t = 0; t < sizeof ks / sizeof ks[0]; t++) {
            PQ q = {0};
            for (int i = 0; i < R && i < ks[t]; i++)
                push(&q, (St){mat[i][0], i, 0, 0});
            St s = {0, 0, 0, 0};
            for (int c = 0; c < ks[t]; c++) {
                s = pop(&q);
                if (s.j + 1 < Cc)
                    push(&q, (St){mat[s.i][s.j + 1], s.i, s.j + 1, 0});
            }
            check(s.sum == flat[ks[t] - 1], "kth element equals sorted rank");
            printf("matrix k=%-5d value=%ld\n", ks[t], s.sum);
            free(q.a);
        }
    }
    return 0;
}
