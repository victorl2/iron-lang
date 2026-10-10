/*
 * title: Disjoint sparse table for non-idempotent operations
 * topic: data_structures
 * covers: disjoint sparse table, highest differing bit, O(1) queries, non-commutative matrix product, prefix and suffix blocks from midpoints
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define P 1000000007ULL
#define LEVELS 8
#define SZ 256

typedef struct {
    unsigned long long m[2][2];
} Mat;

static Mat mul(Mat a, Mat b) {
    Mat r;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            unsigned long long s = 0;
            for (int k = 0; k < 2; k++)
                s = (s + a.m[i][k] * b.m[k][j]) % P;
            r.m[i][j] = s;
        }
    return r;
}


static Mat mt[LEVELS][SZ];
static long st[LEVELS][SZ];

static int high_bit(unsigned x) {
    int b = 0;
    while (x >>= 1)
        b++;
    return b;
}

/* n must be a power of two, n <= SZ */
static void build(const Mat *a, const long *v, int n) {
    int levels = high_bit((unsigned)n);
    for (int h = 0; h < levels; h++) {
        int half = 1 << h;
        for (int start = 0; start < n; start += 2 * half) {
            int mid = start + half;
            /* left half: suffix products ending at mid-1 */
            mt[h][mid - 1] = a[mid - 1];
            st[h][mid - 1] = v[mid - 1];
            for (int i = mid - 2; i >= start; i--) {
                mt[h][i] = mul(a[i], mt[h][i + 1]);
                st[h][i] = v[i] + st[h][i + 1];
            }
            /* right half: prefix products starting at mid */
            mt[h][mid] = a[mid];
            st[h][mid] = v[mid];
            for (int i = mid + 1; i < start + 2 * half; i++) {
                mt[h][i] = mul(mt[h][i - 1], a[i]);
                st[h][i] = st[h][i - 1] + v[i];
            }
        }
    }
}

static Mat query_mat(const Mat *a, int l, int r) {
    if (l == r)
        return a[l];
    int h = high_bit((unsigned)(l ^ r));
    return mul(mt[h][l], mt[h][r]);
}

static long query_sum(const long *v, int l, int r) {
    if (l == r)
        return v[l];
    int h = high_bit((unsigned)(l ^ r));
    return st[h][l] + st[h][r];
}

int main(void) {
    int sizes[] = {2, 4, 16, 64, 256};
    for (int si = 0; si < 5; si++) {
        int n = sizes[si];
        Mat a[SZ];
        long v[SZ];
        for (int i = 0; i < n; i++) {
            for (int x = 0; x < 2; x++)
                for (int y = 0; y < 2; y++)
                    a[i].m[x][y] = rnd(1000);
            v[i] = (long)rnd(2001) - 1000;
        }
        build(a, v, n);
        unsigned long long digest = 0;
        long sdigest = 0;
        int nq = 0;
        for (int l = 0; l < n; l++)
            for (int r = l; r < n; r++) {
                if (n > 16 && rnd(20) != 0)
                    continue;
                Mat want = a[l];
                long ws = v[l];
                for (int i = l + 1; i <= r; i++) {
                    want = mul(want, a[i]);
                    ws += v[i];
                }
                Mat got = query_mat(a, l, r);
                check(memcmp(&got, &want, sizeof got) == 0, "matrix product");
                check(query_sum(v, l, r) == ws, "sum");
                digest = digest * 31 + got.m[0][0] + got.m[1][1];
                sdigest += ws;
                nq++;
            }
        printf("n=%-3d queries=%-5d matrix digest=%llu sum digest=%ld\n", n, nq, digest, sdigest);
    }
    Mat f = {{{1, 1}, {1, 0}}};
    Mat fib[SZ];
    long ones[SZ];
    for (int i = 0; i < SZ; i++) {
        fib[i] = f;
        ones[i] = 1;
    }
    build(fib, ones, SZ);
    Mat fm = query_mat(fib, 0, 89);
    printf("fib(90) mod p = %llu\n", fm.m[0][1]);
    check(fm.m[0][1] == 2880067194370816120ULL % P, "fibonacci via range product");
    return 0;
}
