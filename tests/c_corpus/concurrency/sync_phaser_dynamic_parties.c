/*
 * title: Phaser with dynamic registration and deregistration
 * topic: concurrency
 * covers: phaser, phase numbers, register and deregister parties, arrive-and-await, termination, per-phase party counts
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { MAXP = 16 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int parties, arrived;
    int phase;
    int terminated;
    int cur;
    int parties_at_phase[MAXP]; /* parties that took part in each completed phase */
} Phaser;

static Phaser ph = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, {0}};

/* `cur` counts everyone who takes part in the current phase: parties alive at its start plus late
 * registrations. A party that deregisters still counts for the phase it leaves in. */
static void advance_locked(void) {
    ph.parties_at_phase[ph.phase] = ph.cur;
    ph.phase++;
    ph.arrived = 0;
    ph.cur = ph.parties;
    pthread_cond_broadcast(&ph.cv);
}

/* The new party is expected to arrive in the phase it registered in. Returns that phase. */
static int ph_register(void) {
    pthread_mutex_lock(&ph.mu);
    ph.parties++;
    ph.cur++;
    int p = ph.phase;
    pthread_mutex_unlock(&ph.mu);
    return p;
}

static int ph_arrive_and_await(void) {
    pthread_mutex_lock(&ph.mu);
    int my = ph.phase;
    if (++ph.arrived == ph.parties) {
        advance_locked();
    } else {
        while (ph.phase == my)
            pthread_cond_wait(&ph.cv, &ph.mu);
    }
    int next = ph.phase;
    pthread_mutex_unlock(&ph.mu);
    return next;
}

static void ph_arrive_and_deregister(void) {
    pthread_mutex_lock(&ph.mu);
    ph.parties--;
    if (ph.parties == 0) {
        ph.parties_at_phase[ph.phase] = ph.cur;
        ph.phase++;
        ph.terminated = 1;
        pthread_cond_broadcast(&ph.cv);
    } else if (ph.arrived == ph.parties) { /* the leaver was the last one everybody waited for */
        advance_locked();
    }
    pthread_mutex_unlock(&ph.mu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { W = 5 };
static const int lifetime[W] = {3, 5, 4, 7, 6}; /* phases each worker takes part in */
static int wrote[MAXP][MAXP];                    /* wrote[phase][id]: filled before arriving */
static atomic_int missing_writes;
static int child_spawn_phase = 2;
static pthread_t child_thread;
static int child_id = W;

static void *child(void *p) {
    (void)p;
    /* registered during phase 2: takes part in phases 2, 3 and 4 */
    for (int phase = 2; phase <= 4; phase++) {
        wrote[phase][child_id] = 1;
        if (phase == 4)
            ph_arrive_and_deregister();
        else
            ph_arrive_and_await();
    }
    return NULL;
}

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    for (int phase = 0; phase < lifetime[id]; phase++) {
        wrote[phase][id] = 1;
        if (id == 0 && phase == child_spawn_phase) {
            ph_register(); /* worker 0 recruits a child during phase 2 */
            pthread_create(&child_thread, NULL, child, NULL);
        }
        if (phase == lifetime[id] - 1) {
            ph_arrive_and_deregister();
        } else {
            int next = ph_arrive_and_await();
            /* everyone who was a party of `phase` must have written its mark before we got here */
            int expect_parties = ph.parties_at_phase[phase];
            int marks = 0;
            for (int k = 0; k <= W; k++)
                marks += wrote[phase][k];
            if (marks != expect_parties)
                atomic_fetch_add(&missing_writes, 1);
            if (next != phase + 1)
                atomic_fetch_add(&missing_writes, 1);
        }
    }
    return NULL;
}

int main(void) {
    pthread_t th[W];
    for (int i = 0; i < W; i++)
        ph_register();
    for (int i = 0; i < W; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < W; i++)
        pthread_join(th[i], NULL);
    pthread_join(child_thread, NULL);

    int expect[MAXP] = {0};
    for (int i = 0; i < W; i++)
        for (int p = 0; p < lifetime[i]; p++)
            expect[p]++;
    for (int p = 2; p <= 4; p++)
        expect[p]++; /* the child takes part in phases 2, 3 and 4 */
    int phases_run = ph.phase;
    printf("phases completed: %d, terminated: %s\n", phases_run, ph.terminated ? "yes" : "no");
    int ok = 1;
    for (int p = 0; p < phases_run; p++) {
        printf("phase %d: %d parties (expected %d)\n", p, ph.parties_at_phase[p], expect[p]);
        if (ph.parties_at_phase[p] != expect[p])
            ok = 0;
    }
    printf("missing writes seen after a phase: %d\n", atomic_load(&missing_writes));
    check(ok, "party counts");
    check(phases_run == 7 && ph.terminated && ph.parties == 0, "termination");
    check(atomic_load(&missing_writes) == 0, "barrier property");
    return 0;
}
