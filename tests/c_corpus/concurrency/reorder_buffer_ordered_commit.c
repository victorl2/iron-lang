/*
 * title: Reorder buffer committing out-of-order results in sequence
 * topic: concurrency
 * covers: sliding window credit, out-of-order completion, in-order commit thread, slot ring by index, window bound
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 600, W = 8, NW = 4 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int next_claim, next_commit;
static long slot_val[W];
static int slot_full[W];
static int buffered, max_buffered, window_violation;
static unsigned long long chain;
static long committed_first[6];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long compute(int i) {
    long v = i + 17;
    for (int k = 0; k < 10 + (i * 7) % 23; k++)
        v = (v * 48271 + k) % 2147483647L;
    return v;
}

static void *worker(void *arg) {
    for (;;) {
        pthread_mutex_lock(&mu);
        while (next_claim < N && next_claim >= next_commit + W)
            pthread_cond_wait(&cv, &mu);
        if (next_claim >= N) {
            pthread_mutex_unlock(&mu);
            return NULL;
        }
        int i = next_claim++;
        pthread_mutex_unlock(&mu);
        long v = compute(i);
        /* uneven latency so results really do finish out of order */
        for (int y = 0; y < (i * 13) % 5; y++)
            sched_yield();
        pthread_mutex_lock(&mu);
        if (i >= next_commit + W)
            window_violation++;
        check(!slot_full[i % W], "slot free");
        slot_val[i % W] = v;
        slot_full[i % W] = 1;
        if (i != next_commit)
            buffered++;
        if (buffered > max_buffered)
            max_buffered = buffered;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
}

static void *committer(void *arg) {
    pthread_mutex_lock(&mu);
    while (next_commit < N) {
        while (!slot_full[next_commit % W])
            pthread_cond_wait(&cv, &mu);
        int i = next_commit;
        long v = slot_val[i % W];
        slot_full[i % W] = 0;
        if (i < 6)
            committed_first[i] = v;
        chain = chain * 1000003ULL + (unsigned long long)v;
        next_commit++;
        /* everything now buffered ahead of the commit point was counted in 'buffered' */
        int ahead = 0;
        for (int s = 0; s < W; s++)
            if (slot_full[s])
                ahead++;
        buffered = ahead;
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    pthread_t wt[NW], ct;
    check(pthread_create(&ct, NULL, committer, NULL) == 0, "committer");
    for (int i = 0; i < NW; i++)
        check(pthread_create(&wt[i], NULL, worker, NULL) == 0, "worker");
    for (int i = 0; i < NW; i++)
        pthread_join(wt[i], NULL);
    pthread_join(ct, NULL);

    unsigned long long ref = 0;
    for (int i = 0; i < N; i++)
        ref = ref * 1000003ULL + (unsigned long long)compute(i);
    check(chain == ref, "commit chain equals in-order evaluation");
    check(window_violation == 0, "no result outside the window");
    check(max_buffered <= W, "buffer bounded by window");
    check(next_commit == N && next_claim == N, "all committed");
    printf("items %d window %d workers %d\n", N, W, NW);
    printf("first commits:");
    for (int i = 0; i < 6; i++)
        printf(" %ld", committed_first[i]);
    printf("\n");
    printf("commit chain %llu\n", chain);
    printf("buffer bound respected: yes, commits strictly in order: yes\n");
    return 0;
}
