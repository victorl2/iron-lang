/*
 * title: Parallel tree reduction of non-commutative associative operators
 * topic: concurrency
 * covers: monoid reduction, chunked partial results, pairwise combine rounds, order preservation, matrix and affine maps
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 6000, MAXCHUNKS = 32 };
#define P 1000003ULL

typedef struct {
    unsigned long long m[2][2];
} Mat;

typedef struct {
    unsigned long long a, b; /* x -> a*x + b (mod P) */
} Aff;

static Mat mats[N];
static Aff affs[N];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static Mat mat_id(void) {
    Mat r = {{{1, 0}, {0, 1}}};
    return r;
}

static Mat mat_mul(Mat x, Mat y) {
    Mat r;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            r.m[i][j] = (x.m[i][0] * y.m[0][j] + x.m[i][1] * y.m[1][j]) % P;
    return r;
}

static Aff aff_id(void) {
    Aff r = {1, 0};
    return r;
}

/* compose: apply x first, then y */
static Aff aff_then(Aff x, Aff y) {
    Aff r = {(y.a * x.a) % P, (y.a * x.b + y.b) % P};
    return r;
}

typedef struct {
    int lo, hi;
    Mat pm;
    Aff pa;
} Chunk;

static Chunk chunks[MAXCHUNKS];

static void *leaf(void *arg) {
    Chunk *c = arg;
    c->pm = mat_id();
    c->pa = aff_id();
    for (int i = c->lo; i < c->hi; i++) {
        c->pm = mat_mul(c->pm, mats[i]);
        c->pa = aff_then(c->pa, affs[i]);
    }
    return NULL;
}

/* Combine chunk[i] and chunk[i+stride] into chunk[i] (left operand first). */
typedef struct {
    Chunk *l, *r;
} Pair;

static void *merge(void *arg) {
    Pair *p = arg;
    p->l->pm = mat_mul(p->l->pm, p->r->pm);
    p->l->pa = aff_then(p->l->pa, p->r->pa);
    return NULL;
}

static int reduce(int nchunks, Mat *om, Aff *oa) {
    pthread_t th[MAXCHUNKS];
    for (int i = 0; i < nchunks; i++) {
        chunks[i].lo = (int)((long)N * i / nchunks);
        chunks[i].hi = (int)((long)N * (i + 1) / nchunks);
        check(pthread_create(&th[i], NULL, leaf, &chunks[i]) == 0, "leaf");
    }
    for (int i = 0; i < nchunks; i++)
        pthread_join(th[i], NULL);
    int levels = 0;
    for (int stride = 1; stride < nchunks; stride *= 2) {
        Pair pr[MAXCHUNKS];
        int np = 0;
        for (int i = 0; i + stride < nchunks; i += 2 * stride) {
            pr[np].l = &chunks[i];
            pr[np].r = &chunks[i + stride];
            check(pthread_create(&th[np], NULL, merge, &pr[np]) == 0, "merge");
            np++;
        }
        for (int i = 0; i < np; i++)
            pthread_join(th[i], NULL);
        levels++;
    }
    *om = chunks[0].pm;
    *oa = chunks[0].pa;
    return levels;
}

int main(void) {
    unsigned s = 8675309u;
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < 4; k++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            mats[i].m[k / 2][k % 2] = s % P;
        }
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        affs[i].a = 1 + s % (P - 1);
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        affs[i].b = s % P;
    }
    Mat sm = mat_id();
    Aff sa = aff_id();
    for (int i = 0; i < N; i++) {
        sm = mat_mul(sm, mats[i]);
        sa = aff_then(sa, affs[i]);
    }
    printf("sequential: matrix [%llu %llu; %llu %llu] affine (%llu, %llu)\n", sm.m[0][0], sm.m[0][1], sm.m[1][0],
           sm.m[1][1], sa.a, sa.b);
    int counts[] = {1, 2, 3, 5, 8, 13, 16, 32};
    for (size_t k = 0; k < sizeof counts / sizeof counts[0]; k++) {
        Mat pm;
        Aff pa;
        int levels = reduce(counts[k], &pm, &pa);
        int same = pm.m[0][0] == sm.m[0][0] && pm.m[0][1] == sm.m[0][1] && pm.m[1][0] == sm.m[1][0] &&
                   pm.m[1][1] == sm.m[1][1] && pa.a == sa.a && pa.b == sa.b;
        check(same, "parallel reduction equals sequential fold");
        printf("chunks %2d: combine levels %d, equal to sequential: yes\n", counts[k], levels);
    }
    /* Order matters: reversing the operands must change the answer. */
    Mat rev = mat_id();
    for (int i = N - 1; i >= 0; i--)
        rev = mat_mul(rev, mats[i]);
    check(rev.m[0][0] != sm.m[0][0] || rev.m[0][1] != sm.m[0][1], "operator is not commutative");
    printf("reversed product differs: yes\n");
    return 0;
}
