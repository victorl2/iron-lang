/*
 * title: Parallel Karatsuba polynomial multiplication with thread-per-branch
 * topic: concurrency
 * covers: divide and conquer with depth-limited threads, wraparound uint64 coefficients, schoolbook cutoff, degree checks
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define CUTOFF 16

typedef struct {
    const uint64_t *a, *b;
    int n;
    uint64_t *out; /* 2n - 1 coefficients (zeroed by caller) */
    int depth;
} Job;

static void school(const uint64_t *a, const uint64_t *b, int n, uint64_t *out) {
    for (int i = 0; i < 2 * n - 1; i++)
        out[i] = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            out[i + j] += a[i] * b[j];
}

static void *job_main(void *p);

/* Length n is a power of two. Coefficients wrap mod 2^64 (unsigned), which is
 * well-defined and makes Karatsuba exact. */
static void kara(const uint64_t *a, const uint64_t *b, int n, uint64_t *out, int depth) {
    if (n <= CUTOFF) {
        school(a, b, n, out);
        return;
    }
    int h = n / 2;
    uint64_t *sa = malloc((size_t)h * 8), *sb = malloc((size_t)h * 8);
    uint64_t *z0 = malloc((size_t)(2 * h) * 8), *z1 = malloc((size_t)(2 * h) * 8),
             *z2 = malloc((size_t)(2 * h) * 8);
    check(sa && sb && z0 && z1 && z2, "alloc");
    for (int i = 0; i < h; i++) {
        sa[i] = a[i] + a[i + h];
        sb[i] = b[i] + b[i + h];
    }
    Job j0 = {a, b, h, z0, depth - 1};
    Job j1 = {sa, sb, h, z1, depth - 1};
    Job j2 = {a + h, b + h, h, z2, depth - 1};
    if (depth > 0) {
        pthread_t t0, t1;
        if (pthread_create(&t0, NULL, job_main, &j0) != 0 || pthread_create(&t1, NULL, job_main, &j1) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
        job_main(&j2);
        pthread_join(t0, NULL);
        pthread_join(t1, NULL);
    } else {
        job_main(&j0);
        job_main(&j1);
        job_main(&j2);
    }
    for (int i = 0; i < 2 * n - 1; i++)
        out[i] = 0;
    for (int i = 0; i < 2 * h - 1; i++) {
        out[i] += z0[i];
        out[i + n] += z2[i];
        out[i + h] += z1[i] - z0[i] - z2[i];
    }
    free(sa);
    free(sb);
    free(z0);
    free(z1);
    free(z2);
}

static void *job_main(void *p) {
    Job *j = p;
    kara(j->a, j->b, j->n, j->out, j->depth);
    return NULL;
}

int main(void) {
    uint64_t seed = 2718281828ULL;
    int sizes[] = {16, 32, 64, 256, 1024};
    for (int t = 0; t < 5; t++) {
        int n = sizes[t];
        uint64_t *a = malloc((size_t)n * 8), *b = malloc((size_t)n * 8);
        uint64_t *out = calloc((size_t)(2 * n), 8), *ref = calloc((size_t)(2 * n), 8);
        check(a && b && out && ref, "alloc");
        for (int i = 0; i < n; i++) {
            a[i] = sm64(&seed);
            b[i] = sm64(&seed);
        }
        school(a, b, n, ref);
        for (int depth = 0; depth <= 2; depth++) { /* depth 2 means at most 6 extra threads */
            memset(out, 0, (size_t)(2 * n) * 8);
            kara(a, b, n, out, depth);
            check(memcmp(out, ref, (size_t)(2 * n - 1) * 8) == 0, "karatsuba equals schoolbook");
        }
        uint64_t h = 0;
        for (int i = 0; i < 2 * n - 1; i++)
            h = h * 6364136223846793005ULL + out[i] + 1442695040888963407ULL;
        printf("n=%4d coeffs=%4d c[0]=%016llx c[n-1]=%016llx hash=%016llx\n", n, 2 * n - 1,
               (unsigned long long)out[0], (unsigned long long)out[n - 1], (unsigned long long)h);
        free(a);
        free(b);
        free(out);
        free(ref);
    }
    /* small exact case: (1 + x)^16 squared = (1 + x)^32 has binomial coefficients */
    uint64_t p[16], q[32];
    for (int i = 0; i < 16; i++)
        p[i] = 0;
    /* (1+x)^15 coefficients: C(15,i) */
    uint64_t c = 1;
    for (int i = 0; i < 16; i++) {
        p[i] = c;
        c = c * (uint64_t)(15 - i) / (uint64_t)(i + 1);
    }
    school(p, p, 16, q);
    /* (1+x)^30: C(30,15) = 155117520 */
    check(q[15] == 155117520ULL && q[0] == 1 && q[30] == 1, "binomial identity");
    printf("C(30,15) = %llu via polynomial square\n", (unsigned long long)q[15]);
    return 0;
}
