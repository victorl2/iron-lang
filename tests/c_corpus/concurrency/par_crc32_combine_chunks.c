/*
 * title: Parallel CRC-32 of chunks merged with crc32_combine
 * topic: concurrency
 * covers: GF(2) matrix squaring, chunk CRC combine, table-driven CRC, known check value
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

enum { NT = 6 };

static uint32_t table[256];

static void init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
}

static uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 255u] ^ (crc >> 8);
    return ~crc;
}

/* zlib-style combine using GF(2) 32x32 matrices */
static uint32_t gf2_times(const uint32_t *mat, uint32_t vec) {
    uint32_t sum = 0;
    while (vec) {
        if (vec & 1u)
            sum ^= *mat;
        vec >>= 1;
        mat++;
    }
    return sum;
}

static void gf2_square(uint32_t *sq, const uint32_t *mat) {
    for (int n = 0; n < 32; n++)
        sq[n] = gf2_times(mat, mat[n]);
}

static uint32_t crc32_combine(uint32_t crc1, uint32_t crc2, uint64_t len2) {
    uint32_t even[32], odd[32];
    if (len2 == 0)
        return crc1;
    odd[0] = 0xEDB88320u;
    uint32_t row = 1;
    for (int n = 1; n < 32; n++) {
        odd[n] = row;
        row <<= 1;
    }
    gf2_square(even, odd);
    gf2_square(odd, even);
    do {
        gf2_square(even, odd);
        if (len2 & 1u)
            crc1 = gf2_times(even, crc1);
        len2 >>= 1;
        if (len2 == 0)
            break;
        gf2_square(odd, even);
        if (len2 & 1u)
            crc1 = gf2_times(odd, crc1);
        len2 >>= 1;
    } while (len2);
    return crc1 ^ crc2;
}

typedef struct {
    const unsigned char *data;
    size_t n;
    uint32_t crc[8];
    size_t len[8];
} Job;

static void worker(void *ctx, int tid, int nt) {
    Job *j = ctx;
    size_t lo = j->n * (size_t)tid / (size_t)nt;
    size_t hi = j->n * (size_t)(tid + 1) / (size_t)nt;
    j->crc[tid] = crc32_update(0, j->data + lo, hi - lo);
    j->len[tid] = hi - lo;
}

int main(void) {
    init_table();
    const char *check_str = "123456789";
    uint32_t known = crc32_update(0, (const unsigned char *)check_str, 9);
    check(known == 0xCBF43926u, "standard CRC-32 check value");
    printf("crc32(\"123456789\") = %08x\n", (unsigned)known);
    /* combine of two halves of the check string */
    uint32_t a = crc32_update(0, (const unsigned char *)check_str, 4);
    uint32_t b = crc32_update(0, (const unsigned char *)check_str + 4, 5);
    check(crc32_combine(a, b, 5) == known, "combine on check string");

    static unsigned char buf[200003];
    uint64_t seed = 0xC0FFEE;
    size_t sizes[] = {0, 1, 7, 100, 4096, 65537, 200003};
    for (int t = 0; t < 7; t++) {
        size_t n = sizes[t];
        for (size_t i = 0; i < n; i++)
            buf[i] = (unsigned char)sm64(&seed);
        uint32_t whole = crc32_update(0, buf, n);
        printf("n=%6zu whole=%08x", n, (unsigned)whole);
        for (int nt = 1; nt <= 8; nt++) {
            Job j;
            j.data = buf;
            j.n = n;
            par_run(nt, worker, &j);
            uint32_t acc = 0;
            for (int k = 0; k < nt; k++)
                acc = crc32_combine(acc, j.crc[k], j.len[k]);
            check(acc == whole, "combined equals whole");
        }
        printf(" combined-ok(1..8 threads)\n");
    }
    return 0;
}
