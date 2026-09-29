/*
 * title: Lamport bakery lock with seq_cst atomics
 * topic: concurrency
 * covers: bakery algorithm, choosing flags, ticket numbers, lexicographic (number, id) order, sequential consistency
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 4, PER = 250 };

static atomic_int choosing[N];
static atomic_int number[N];

/* (na, ia) precedes (nb, ib) in the bakery order. */
static int precedes(int na, int ia, int nb, int ib) { return na < nb || (na == nb && ia < ib); }

static int bakery_lock(int i) {
    atomic_store(&choosing[i], 1);
    int mx = 0;
    for (int j = 0; j < N; j++) {
        int n = atomic_load(&number[j]);
        if (n > mx)
            mx = n;
    }
    int mine = mx + 1;
    atomic_store(&number[i], mine);
    atomic_store(&choosing[i], 0);
    for (int j = 0; j < N; j++) {
        if (j == i)
            continue;
        while (atomic_load(&choosing[j]))
            sched_yield();
        for (;;) {
            int nj = atomic_load(&number[j]);
            if (nj == 0 || !precedes(nj, j, mine, i))
                break;
            sched_yield();
        }
    }
    return mine;
}

static void bakery_unlock(int i) { atomic_store(&number[i], 0); }

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long counter, shadow;
static atomic_int in_cs, violations, bad_numbers;
static int acquisitions[N];

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    for (int i = 0; i < PER; i++) {
        int n = bakery_lock(id);
        if (n < 1)
            atomic_fetch_add(&bad_numbers, 1);
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        long v = counter;
        if (i % 5 == 0)
            sched_yield();
        counter = v + 1;
        shadow += 3;
        acquisitions[id]++;
        atomic_fetch_sub(&in_cs, 1);
        bakery_unlock(id);
    }
    return NULL;
}

int main(void) {
    /* The tie-break relation is a strict total order on (number, id) pairs. */
    int total_order = 1;
    for (int na = 1; na <= 3; na++)
        for (int ia = 0; ia < N; ia++)
            for (int nb = 1; nb <= 3; nb++)
                for (int ib = 0; ib < N; ib++) {
                    int ab = precedes(na, ia, nb, ib), ba = precedes(nb, ib, na, ia);
                    int same = na == nb && ia == ib;
                    if (same ? (ab || ba) : (ab == ba))
                        total_order = 0;
                }
    printf("(number,id) order is total and antisymmetric: %s\n", total_order ? "yes" : "no");
    check(total_order, "order relation");
    printf("tie: (2,1) before (2,3): %d, (2,3) before (2,1): %d\n", precedes(2, 1, 2, 3), precedes(2, 3, 2, 1));

    pthread_t th[N];
    for (int i = 0; i < N; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < N; i++)
        pthread_join(th[i], NULL);
    for (int i = 0; i < N; i++)
        printf("thread %d entered %d times\n", i, acquisitions[i]);
    printf("counter=%ld shadow=%ld violations=%d bad tickets=%d\n", counter, shadow, atomic_load(&violations),
           atomic_load(&bad_numbers));
    check(counter == N * PER && shadow == 3L * N * PER, "counts");
    check(atomic_load(&violations) == 0 && atomic_load(&bad_numbers) == 0, "exclusion");
    for (int i = 0; i < N; i++)
        check(atomic_load(&number[i]) == 0 && atomic_load(&choosing[i]) == 0, "tickets returned");
    return 0;
}
