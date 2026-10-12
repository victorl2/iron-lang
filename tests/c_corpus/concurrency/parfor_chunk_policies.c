/*
 * title: Parallel for with static, cyclic, dynamic and guided chunking
 * topic: concurrency
 * covers: loop scheduling policies, shared iteration counter, guided chunk shrinking, result equality across policies
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 10000, T = 4, MAXCH = 4096 };

typedef enum { STATIC_BLOCK, STATIC_CYCLIC, DYNAMIC, GUIDED } Policy;
static const char *pname[] = {"static-block", "static-cyclic", "dynamic", "guided"};

static unsigned out[N];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int next_iter;
static int chunk_sizes[MAXCH], nchunks;
static Policy policy;
static int dyn_chunk = 37;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Cost varies wildly by index so that static splits are unbalanced. */
static unsigned body(int i) {
    unsigned h = (unsigned)i * 2654435761u + 12345u;
    int reps = 1 + (i % 29) * (i % 7);
    for (int k = 0; k < reps; k++)
        h = (h ^ (h >> 13)) * 0x85ebca6bu + (unsigned)k;
    return h;
}

static int claim(int *lo, int *hi) {
    pthread_mutex_lock(&mu);
    int remaining = N - next_iter;
    if (remaining <= 0) {
        pthread_mutex_unlock(&mu);
        return 0;
    }
    int sz;
    if (policy == DYNAMIC) {
        sz = dyn_chunk;
    } else {
        sz = remaining / (2 * T);
        if (sz < 8)
            sz = 8;
    }
    if (sz > remaining)
        sz = remaining;
    *lo = next_iter;
    *hi = next_iter + sz;
    next_iter += sz;
    check(nchunks < MAXCH, "chunk log");
    chunk_sizes[nchunks++] = sz;
    pthread_mutex_unlock(&mu);
    return 1;
}

static void *worker(void *arg) {
    int t = (int)(long)arg;
    if (policy == STATIC_BLOCK) {
        int per = (N + T - 1) / T;
        int lo = t * per, hi = lo + per > N ? N : lo + per;
        for (int i = lo; i < hi; i++)
            out[i] = body(i);
    } else if (policy == STATIC_CYCLIC) {
        for (int i = t; i < N; i += T)
            out[i] = body(i);
    } else {
        int lo, hi;
        while (claim(&lo, &hi))
            for (int i = lo; i < hi; i++)
                out[i] = body(i);
    }
    return NULL;
}

static unsigned digest(void) {
    unsigned h = 2166136261u;
    for (int i = 0; i < N; i++) {
        h ^= out[i];
        h *= 16777619u;
    }
    return h;
}

int main(void) {
    unsigned ref = 0;
    {
        unsigned h = 2166136261u;
        for (int i = 0; i < N; i++) {
            h ^= body(i);
            h *= 16777619u;
        }
        ref = h;
    }
    for (int p = 0; p < 4; p++) {
        policy = (Policy)p;
        next_iter = 0;
        nchunks = 0;
        for (int i = 0; i < N; i++)
            out[i] = 0;
        pthread_t th[T];
        for (long t = 0; t < T; t++)
            check(pthread_create(&th[t], NULL, worker, (void *)t) == 0, "create");
        for (int t = 0; t < T; t++)
            pthread_join(th[t], NULL);
        unsigned d = digest();
        check(d == ref, "digest equals sequential");
        printf("%-13s digest %08x", pname[p], d);
        if (nchunks > 0) {
            int covered = 0, mx = 0, mn = N;
            for (int i = 0; i < nchunks; i++) {
                covered += chunk_sizes[i];
                if (chunk_sizes[i] > mx)
                    mx = chunk_sizes[i];
                if (chunk_sizes[i] < mn)
                    mn = chunk_sizes[i];
            }
            check(covered == N, "chunks cover the range");
            printf(" chunks %d max %d min %d first %d %d %d", nchunks, mx, mn, chunk_sizes[0], chunk_sizes[1],
                   chunk_sizes[2]);
        }
        printf("\n");
    }
    printf("all policies agree with the sequential digest %08x\n", ref);
    return 0;
}
