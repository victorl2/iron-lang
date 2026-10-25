/*
 * title: Parallel blocked matrix multiply
 * topic: concurrency
 * covers: tile decomposition, row-block ownership, modular integer arithmetic, associativity check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*ParFn)(void *ctx, int tid, int nt);
typedef struct {
    ParFn fn;
    void *ctx;
    int tid;
    int nt;
} ParJob;

static void *par_tramp(void *p) {
    ParJob *j = p;
    j->fn(j->ctx, j->tid, j->nt);
    return NULL;
}

/* Run fn on nt threads (nt <= 8) and join them all. */
static inline void par_run(int nt, ParFn fn, void *ctx) {
    pthread_t th[8];
    ParJob jobs[8];
    for (int i = 0; i < nt; i++) {
        jobs[i].fn = fn;
        jobs[i].ctx = ctx;
        jobs[i].tid = i;
        jobs[i].nt = nt;
        if (pthread_create(&th[i], NULL, par_tramp, &jobs[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    for (int i = 0; i < nt; i++)
        pthread_join(th[i], NULL);
}

/* Even split of [0,n) into nt contiguous ranges. */
static inline void par_range(int n, int tid, int nt, int *lo, int *hi) {
    *lo = (int)((long)n * tid / nt);
    *hi = (int)((long)n * (tid + 1) / nt);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline uint64_t sm64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

enum { N = 72, B = 8, NT = 6, MOD = 1000003 };

typedef struct {
    const int *a;
    const int *b;
    int *c;
} Mul;

/* Thread owns whole tile-rows of C; inside a tile-row it walks tiles. */
static void mul_worker(void *ctx, int tid, int nt) {
    Mul *m = ctx;
    int nb = N / B;
    int lo, hi;
    par_range(nb, tid, nt, &lo, &hi);
    for (int bi = lo; bi < hi; bi++)
        for (int bj = 0; bj < nb; bj++)
            for (int bk = 0; bk < nb; bk++)
                for (int i = bi * B; i < bi * B + B; i++)
                    for (int k = bk * B; k < bk * B + B; k++) {
                        long aik = m->a[i * N + k];
                        for (int j = bj * B; j < bj * B + B; j++)
                            m->c[i * N + j] =
                                (int)((m->c[i * N + j] + aik * m->b[k * N + j]) % MOD);
                    }
}

static void mul_seq(const int *a, const int *b, int *c) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            long s = 0;
            for (int k = 0; k < N; k++)
                s = (s + (long)a[i * N + k] * b[k * N + j]) % MOD;
            c[i * N + j] = (int)s;
        }
}

static long trace(const int *m) {
    long t = 0;
    for (int i = 0; i < N; i++)
        t += m[i * N + i];
    return t;
}

int main(void) {
    static int a[N * N], b[N * N], c[N * N], d[N * N], ab[N * N], ref[N * N], bc[N * N], l[N * N],
        r[N * N];
    uint64_t seed = 1234;
    for (int i = 0; i < N * N; i++) {
        a[i] = (int)(sm64(&seed) % 100u);
        b[i] = (int)(sm64(&seed) % 100u);
        c[i] = (int)(sm64(&seed) % 100u);
    }
    Mul m;
    m.a = a;
    m.b = b;
    m.c = ab;
    par_run(NT, mul_worker, &m);
    mul_seq(a, b, ref);
    check(memcmp(ab, ref, sizeof ab) == 0, "blocked parallel equals naive");
    printf("trace(AB)=%ld C00=%d C[N-1][N-1]=%d\n", trace(ab), ab[0], ab[N * N - 1]);

    /* associativity: (AB)C == A(BC) mod MOD */
    memset(l, 0, sizeof l);
    m.a = ab;
    m.b = c;
    m.c = l;
    par_run(NT, mul_worker, &m);
    memset(bc, 0, sizeof bc);
    m.a = b;
    m.b = c;
    m.c = bc;
    par_run(4, mul_worker, &m);
    memset(r, 0, sizeof r);
    m.a = a;
    m.b = bc;
    m.c = r;
    par_run(3, mul_worker, &m);
    check(memcmp(l, r, sizeof l) == 0, "associativity");
    printf("trace((AB)C)=%ld equal-to-A(BC)=yes\n", trace(l));

    /* identity */
    for (int i = 0; i < N * N; i++)
        d[i] = (i / N == i % N);
    memset(ref, 0, sizeof ref);
    m.a = a;
    m.b = d;
    m.c = ref;
    par_run(NT, mul_worker, &m);
    check(memcmp(ref, a, sizeof ref) == 0, "A*I == A");
    printf("identity ok\n");
    return 0;
}
