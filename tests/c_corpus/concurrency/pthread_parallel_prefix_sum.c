/*
 * title: Blocked parallel prefix sum
 * topic: concurrency
 * covers: two-pass scan, block sums, phase sequencing by join, per-thread ranges, unsigned wraparound
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 10007, T = 6 };

static uint32_t in[N], out[N], seqout[N];
static uint32_t block_total[T], block_offset[T];

typedef struct {
    int id;
    int lo, hi;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *pass1(void *p) {
    Arg *a = p;
    uint32_t run = 0;
    for (int i = a->lo; i < a->hi; i++) {
        run += in[i];
        out[i] = run; /* local inclusive scan */
    }
    block_total[a->id] = run;
    return NULL;
}

static void *pass2(void *p) {
    Arg *a = p;
    uint32_t off = block_offset[a->id];
    for (int i = a->lo; i < a->hi; i++)
        out[i] += off;
    return NULL;
}

static void run_phase(void *(*fn)(void *), Arg *args) {
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, fn, &args[i]) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
}

int main(void) {
    uint32_t s = 42u;
    for (int i = 0; i < N; i++) {
        s = s * 1664525u + 1013904223u;
        in[i] = (s >> 12) % 1000u;
    }
    uint32_t run = 0;
    for (int i = 0; i < N; i++) {
        run += in[i];
        seqout[i] = run;
    }
    Arg args[T];
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        args[i].lo = (int)((long)N * i / T);
        args[i].hi = (int)((long)N * (i + 1) / T);
    }
    run_phase(pass1, args);
    uint32_t acc = 0;
    for (int i = 0; i < T; i++) {
        block_offset[i] = acc;
        acc += block_total[i];
    }
    run_phase(pass2, args);
    int mismatches = 0;
    for (int i = 0; i < N; i++)
        if (out[i] != seqout[i])
            mismatches++;
    check(mismatches == 0, "matches sequential scan");
    check(out[N - 1] == acc, "last element equals total");
    for (int i = 0; i < T; i++)
        printf("block %d [%d,%d) total=%u offset=%u\n", i, args[i].lo, args[i].hi,
               (unsigned)block_total[i], (unsigned)block_offset[i]);
    printf("total=%u out[0]=%u out[%d]=%u\n", (unsigned)acc, (unsigned)out[0], N / 2,
           (unsigned)out[N / 2]);
    return 0;
}
