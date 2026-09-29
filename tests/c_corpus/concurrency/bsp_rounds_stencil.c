/*
 * title: Bulk synchronous rounds with a hand-built barrier and reduction
 * topic: concurrency
 * covers: generation barrier, double buffering, per-round max reduction, conservation invariant, sequential cross-check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 4, CELLS = 64, ROUNDS = 48 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int count, gen;
} Barrier;

static Barrier bar = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};
static long buf[2][CELLS];
static long slots[2][T];
static long round_max[ROUNDS];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void barrier_wait(void) {
    pthread_mutex_lock(&bar.mu);
    int g = bar.gen;
    if (++bar.count == T) {
        bar.count = 0;
        bar.gen++;
        pthread_cond_broadcast(&bar.cv);
    } else {
        while (bar.gen == g)
            pthread_cond_wait(&bar.cv, &bar.mu);
    }
    pthread_mutex_unlock(&bar.mu);
}

/* Each cell gives a quarter of its content to each neighbour on the ring (integer division). */
static long step_cell(const long *cur, int i) {
    int l = (i + CELLS - 1) % CELLS, r = (i + 1) % CELLS;
    return cur[i] - 2 * (cur[i] / 4) + cur[l] / 4 + cur[r] / 4;
}

static void *worker(void *arg) {
    int t = (int)(long)arg;
    int per = CELLS / T;
    for (int r = 0; r < ROUNDS; r++) {
        const long *cur = buf[r % 2];
        long *nxt = buf[(r + 1) % 2];
        long mx = 0;
        for (int i = t * per; i < (t + 1) * per; i++) {
            nxt[i] = step_cell(cur, i);
            if (nxt[i] > mx)
                mx = nxt[i];
        }
        slots[r % 2][t] = mx;
        barrier_wait();
        long g = 0;
        for (int k = 0; k < T; k++)
            if (slots[r % 2][k] > g)
                g = slots[r % 2][k];
        if (t == 0)
            round_max[r] = g;
    }
    return NULL;
}

int main(void) {
    long ref[2][CELLS];
    for (int i = 0; i < CELLS; i++)
        buf[0][i] = ref[0][i] = 0;
    buf[0][5] = ref[0][5] = 1000;
    buf[0][40] = ref[0][40] = 777;
    buf[0][41] = ref[0][41] = 123;
    long total0 = 1000 + 777 + 123;

    pthread_t th[T];
    for (long t = 0; t < T; t++)
        check(pthread_create(&th[t], NULL, worker, (void *)t) == 0, "create");
    for (int t = 0; t < T; t++)
        pthread_join(th[t], NULL);

    long ref_max[ROUNDS];
    for (int r = 0; r < ROUNDS; r++) {
        long mx = 0;
        for (int i = 0; i < CELLS; i++) {
            ref[(r + 1) % 2][i] = step_cell(ref[r % 2], i);
            if (ref[(r + 1) % 2][i] > mx)
                mx = ref[(r + 1) % 2][i];
        }
        ref_max[r] = mx;
    }
    const long *fin = buf[ROUNDS % 2];
    long total = 0;
    for (int i = 0; i < CELLS; i++) {
        check(fin[i] == ref[ROUNDS % 2][i], "final cell equals sequential");
        total += fin[i];
    }
    for (int r = 0; r < ROUNDS; r++)
        check(round_max[r] == ref_max[r], "round max equals sequential");
    check(total == total0, "total conserved");
    for (int r = 0; r < ROUNDS; r += 6)
        printf("round %2d max %ld\n", r + 1, round_max[r]);
    printf("final max %ld total %ld\n", round_max[ROUNDS - 1], total);
    printf("cells 0..7 after %d rounds:", ROUNDS);
    for (int i = 0; i < 8; i++)
        printf(" %ld", fin[i]);
    printf("\n");
    return 0;
}
