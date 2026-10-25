/*
 * title: Parallel map with dynamic chunk claiming and ordered output
 * topic: concurrency
 * covers: atomic chunk counter, ordered output slots, function pointer map, collatz steps
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

#include <stdatomic.h>

typedef uint32_t (*MapFn)(uint32_t);

static uint32_t f_collatz(uint32_t x) {
    uint32_t steps = 0;
    uint64_t v = x + 1u;
    while (v != 1) {
        v = (v & 1) ? 3 * v + 1 : v / 2;
        steps++;
    }
    return steps;
}

static uint32_t f_digitsum(uint32_t x) {
    uint32_t s = 0;
    while (x) {
        s += x % 10;
        x /= 10;
    }
    return s;
}

static uint32_t f_mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

typedef struct {
    const uint32_t *in;
    uint32_t *out;
    int n;
    int chunk;
    MapFn fn;
    atomic_int next;
    int claimed[8]; /* chunks claimed per thread (varies, not printed) */
} Job;

static void worker(void *ctx, int tid, int nt) {
    Job *j = ctx;
    (void)nt;
    for (;;) {
        int c = atomic_fetch_add(&j->next, 1);
        int lo = c * j->chunk;
        if (lo >= j->n)
            break;
        int hi = lo + j->chunk;
        if (hi > j->n)
            hi = j->n;
        for (int i = lo; i < hi; i++)
            j->out[i] = j->fn(j->in[i]);
        j->claimed[tid]++;
    }
}

int main(void) {
    enum { N = 3000 };
    static uint32_t in[N], par[N], ref[N];
    uint64_t seed = 99;
    for (int i = 0; i < N; i++)
        in[i] = (uint32_t)(sm64(&seed) % 50000u);
    MapFn fns[3] = {f_collatz, f_digitsum, f_mix};
    const char *names[3] = {"collatz", "digitsum", "mix"};
    int chunks[3] = {1, 17, 256};
    for (int f = 0; f < 3; f++) {
        for (int i = 0; i < N; i++)
            ref[i] = fns[f](in[i]);
        for (int t = 0; t < 3; t++) {
            int nt = 2 + t * 3;
            Job j;
            memset(&j, 0, sizeof j);
            j.in = in;
            j.out = par;
            j.n = N;
            j.chunk = chunks[t];
            j.fn = fns[f];
            atomic_init(&j.next, 0);
            memset(par, 0xAB, sizeof par);
            par_run(nt, worker, &j);
            int total = 0;
            for (int k = 0; k < nt; k++)
                total += j.claimed[k];
            check(total == (N + chunks[t] - 1) / chunks[t], "every chunk claimed once");
            check(memcmp(par, ref, sizeof par) == 0, "ordered output equals sequential");
        }
        uint64_t h = 1469598103934665603ULL;
        uint32_t mx = 0;
        for (int i = 0; i < N; i++) {
            h = (h ^ ref[i]) * 1099511628211ULL;
            if (ref[i] > mx)
                mx = ref[i];
        }
        printf("%s: max=%u hash=%016llx first=%u,%u,%u\n", names[f], (unsigned)mx,
               (unsigned long long)h, (unsigned)ref[0], (unsigned)ref[1], (unsigned)ref[2]);
    }
    return 0;
}
