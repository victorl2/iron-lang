/*
 * title: Per-thread PRNG streams via splitmix seeding
 * topic: concurrency
 * covers: independent PRNG state per thread, splitmix64 seeding, Monte Carlo merge, reproducibility
 * deps: libc, pthread
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 6, DRAWS = 20000 };

typedef struct {
    uint64_t s[4];
} Xoshiro;

static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint64_t rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t xo_next(Xoshiro *g) {
    uint64_t *s = g->s;
    uint64_t result = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

static void xo_seed(Xoshiro *g, uint64_t seed) {
    for (int i = 0; i < 4; i++)
        g->s[i] = splitmix(&seed);
}

typedef struct {
    int id;
    uint64_t seed;
    long inside;
    uint64_t digest;
    uint64_t first;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void run_stream(Arg *a) {
    Xoshiro g;
    xo_seed(&g, a->seed);
    a->inside = 0;
    a->digest = 0;
    for (int i = 0; i < DRAWS; i++) {
        uint64_t rx = xo_next(&g);
        uint64_t ry = xo_next(&g);
        if (i == 0)
            a->first = rx;
        /* 31-bit coordinates, integer-only quarter-circle test */
        uint64_t x = rx >> 33, y = ry >> 33;
        if (x * x + y * y < (1ull << 62))
            a->inside++;
        a->digest = a->digest * 31u + (rx ^ ry);
    }
}

static void *worker(void *p) {
    run_stream(p);
    return NULL;
}

int main(void) {
    Arg par[T], seq[T];
    pthread_t th[T];
    uint64_t master = 20240607ull;
    for (int i = 0; i < T; i++) {
        par[i].id = i;
        par[i].seed = splitmix(&master); /* seeds drawn on the main thread, in order */
        seq[i] = par[i];
        check(pthread_create(&th[i], NULL, worker, &par[i]) == 0, "create");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    for (int i = 0; i < T; i++)
        run_stream(&seq[i]);

    long inside = 0;
    for (int i = 0; i < T; i++) {
        check(par[i].inside == seq[i].inside, "inside matches sequential");
        check(par[i].digest == seq[i].digest, "digest matches sequential");
        printf("stream %d: first=%016" PRIx64 " inside=%ld digest=%016" PRIx64 "\n", i, par[i].first,
               par[i].inside, par[i].digest);
        inside += par[i].inside;
    }
    for (int i = 0; i < T; i++)
        for (int j = i + 1; j < T; j++)
            check(par[i].first != par[j].first, "streams differ");
    double pi = 4.0 * (double)inside / (double)((long)T * DRAWS);
    check(pi > 3.05 && pi < 3.25, "pi estimate plausible");
    printf("pi estimate ~ %.1f\n", pi);
    printf("total inside=%ld of %ld\n", inside, (long)T * DRAWS);
    return 0;
}
