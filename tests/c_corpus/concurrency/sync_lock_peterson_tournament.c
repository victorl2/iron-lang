/*
 * title: Peterson lock and four-thread tournament tree
 * topic: concurrency
 * covers: Peterson's algorithm, flag and victim, seq_cst atomics, tournament of two-party locks, hierarchical release order
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    atomic_int flag[2];
    atomic_int victim;
} Peterson;

static void pet_lock(Peterson *l, int side) {
    int other = 1 - side;
    atomic_store(&l->flag[side], 1);
    atomic_store(&l->victim, side);
    while (atomic_load(&l->flag[other]) && atomic_load(&l->victim) == side)
        sched_yield();
}
static void pet_unlock(Peterson *l, int side) { atomic_store(&l->flag[side], 0); }

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* Part 1: plain two-thread Peterson. */
static Peterson duo;
static long duo_counter;
static atomic_int duo_in, duo_viol;

static void *duo_worker(void *p) {
    int side = (int)(intptr_t)p;
    for (int i = 0; i < 1500; i++) {
        pet_lock(&duo, side);
        if (atomic_fetch_add(&duo_in, 1) != 0)
            atomic_fetch_add(&duo_viol, 1);
        long v = duo_counter;
        if (i % 9 == 0)
            sched_yield();
        duo_counter = v + 1;
        atomic_fetch_sub(&duo_in, 1);
        pet_unlock(&duo, side);
    }
    return NULL;
}

/*
 * Part 2: tournament tree for four threads. Nodes 1 (root), 2 and 3 (leaves' parents).
 * Thread t plays at node 2 + t/2 as side t%2, then at the root as side t/2.
 */
static Peterson node[4];
static long tour_counter;
static atomic_int tour_in, tour_viol, root_holders;
static int tour_entries[4];

static void tour_lock(int t) {
    pet_lock(&node[2 + t / 2], t % 2);
    pet_lock(&node[1], t / 2);
}
static void tour_unlock(int t) {
    pet_unlock(&node[1], t / 2); /* release root first, then the leaf pair */
    pet_unlock(&node[2 + t / 2], t % 2);
}

static void *tour_worker(void *p) {
    int t = (int)(intptr_t)p;
    for (int i = 0; i < 400; i++) {
        tour_lock(t);
        if (atomic_fetch_add(&tour_in, 1) != 0)
            atomic_fetch_add(&tour_viol, 1);
        if (atomic_fetch_add(&root_holders, 1) != 0)
            atomic_fetch_add(&tour_viol, 1);
        long v = tour_counter;
        if (i % 7 == 0)
            sched_yield();
        tour_counter = v + 1;
        tour_entries[t]++;
        atomic_fetch_sub(&root_holders, 1);
        atomic_fetch_sub(&tour_in, 1);
        tour_unlock(t);
    }
    return NULL;
}

int main(void) {
    pthread_t a, b;
    pthread_create(&a, NULL, duo_worker, (void *)(intptr_t)0);
    pthread_create(&b, NULL, duo_worker, (void *)(intptr_t)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("peterson: counter=%ld (expected 3000) violations=%d\n", duo_counter, atomic_load(&duo_viol));
    check(duo_counter == 3000 && atomic_load(&duo_viol) == 0, "peterson");
    check(!atomic_load(&duo.flag[0]) && !atomic_load(&duo.flag[1]), "flags cleared");

    pthread_t th[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, tour_worker, (void *)(intptr_t)i);
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);
    printf("tournament: counter=%ld (expected 1600) violations=%d\n", tour_counter, atomic_load(&tour_viol));
    for (int i = 0; i < 4; i++)
        printf("  thread %d entered %d times\n", i, tour_entries[i]);
    check(tour_counter == 1600 && atomic_load(&tour_viol) == 0, "tournament");
    for (int i = 1; i < 4; i++)
        check(!atomic_load(&node[i].flag[0]) && !atomic_load(&node[i].flag[1]), "node flags cleared");
    printf("all nodes idle: yes\n");
    return 0;
}
