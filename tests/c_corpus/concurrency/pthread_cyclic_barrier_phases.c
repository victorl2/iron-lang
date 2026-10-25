/*
 * title: Hand-built cyclic barrier driving lockstep phases
 * topic: concurrency
 * covers: barrier from mutex+condvar, generation counter, reuse across phases, double buffering
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 6, CELLS = 24, STEPS = 10 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int parties, waiting;
    unsigned generation;
} Barrier;

static void barrier_init(Barrier *b, int n) {
    pthread_mutex_init(&b->mu, NULL);
    pthread_cond_init(&b->cv, NULL);
    b->parties = n;
    b->waiting = 0;
    b->generation = 0;
}

/* returns 1 for exactly one caller per phase */
static int barrier_wait(Barrier *b) {
    pthread_mutex_lock(&b->mu);
    unsigned gen = b->generation;
    int leader = 0;
    if (++b->waiting == b->parties) {
        b->waiting = 0;
        b->generation++;
        leader = 1;
        pthread_cond_broadcast(&b->cv);
    } else {
        while (gen == b->generation)
            pthread_cond_wait(&b->cv, &b->mu);
    }
    pthread_mutex_unlock(&b->mu);
    return leader;
}

static Barrier bar;
static int grid[2][CELLS];
static int leaders[STEPS];
static pthread_mutex_t lead_mu = PTHREAD_MUTEX_INITIALIZER;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* one step of a 1-D ring automaton: new = (left + center*2 + right) mod 7 */
static int rule(int l, int c, int r) {
    return (l + 2 * c + r) % 7;
}

typedef struct {
    int id;
    int lo, hi;
} Arg;

static void *worker(void *p) {
    Arg *a = p;
    for (int s = 0; s < STEPS; s++) {
        int (*cur)[CELLS] = &grid[s % 2];
        int (*nxt)[CELLS] = &grid[(s + 1) % 2];
        for (int i = a->lo; i < a->hi; i++)
            (*nxt)[i] = rule((*cur)[(i + CELLS - 1) % CELLS], (*cur)[i], (*cur)[(i + 1) % CELLS]);
        if (barrier_wait(&bar)) {
            pthread_mutex_lock(&lead_mu);
            leaders[s]++;
            pthread_mutex_unlock(&lead_mu);
        }
    }
    return NULL;
}

int main(void) {
    for (int i = 0; i < CELLS; i++)
        grid[0][i] = (i * i + 3) % 7;
    int ref[2][CELLS];
    for (int i = 0; i < CELLS; i++)
        ref[0][i] = grid[0][i];
    for (int s = 0; s < STEPS; s++)
        for (int i = 0; i < CELLS; i++)
            ref[(s + 1) % 2][i] = rule(ref[s % 2][(i + CELLS - 1) % CELLS], ref[s % 2][i],
                                       ref[s % 2][(i + 1) % CELLS]);

    barrier_init(&bar, T);
    Arg args[T];
    pthread_t th[T];
    int per = CELLS / T;
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        args[i].lo = i * per;
        args[i].hi = (i + 1) * per;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    for (int s = 0; s < STEPS; s++)
        check(leaders[s] == 1, "exactly one leader per phase");
    int final = STEPS % 2;
    long sum = 0;
    printf("final state:");
    for (int i = 0; i < CELLS; i++) {
        check(grid[final][i] == ref[final][i], "matches sequential run");
        printf(" %d", grid[final][i]);
        sum += grid[final][i];
    }
    printf("\nsum=%ld generations=%u\n", sum, bar.generation);
    check(bar.generation == STEPS, "generation count");
    pthread_mutex_destroy(&bar.mu);
    pthread_cond_destroy(&bar.cv);
    return 0;
}
