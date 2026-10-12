/*
 * title: Lamport fast mutex with contention slow path
 * topic: concurrency
 * covers: Lamport fast mutex, x and y registers, b flags, fast path and slow path, seq_cst atomics
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 4, PER = 300 }; /* thread ids are 1..N; 0 means "nobody" */

static atomic_int x, y;
static atomic_int b[N + 1];
static atomic_int fast_path_entries, slow_path_entries;

static void fm_lock(int i) {
    for (;;) {
        atomic_store(&b[i], 1);
        atomic_store(&x, i);
        if (atomic_load(&y) != 0) {
            atomic_store(&b[i], 0);
            while (atomic_load(&y) != 0)
                sched_yield();
            continue;
        }
        atomic_store(&y, i);
        if (atomic_load(&x) != i) { /* somebody else raced through the doorway: slow path */
            atomic_store(&b[i], 0);
            for (int j = 1; j <= N; j++)
                while (atomic_load(&b[j]))
                    sched_yield();
            if (atomic_load(&y) != i) {
                while (atomic_load(&y) != 0)
                    sched_yield();
                continue;
            }
            atomic_fetch_add(&slow_path_entries, 1);
        } else {
            atomic_fetch_add(&fast_path_entries, 1);
        }
        return;
    }
}

static void fm_unlock(int i) {
    atomic_store(&y, 0);
    atomic_store(&b[i], 0);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long counter;
static atomic_int in_cs, violations;
static int entries[N + 1];

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    for (int i = 0; i < PER; i++) {
        fm_lock(id);
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        long v = counter;
        if (i % 6 == 0)
            sched_yield();
        counter = v + 1;
        entries[id]++;
        atomic_fetch_sub(&in_cs, 1);
        fm_unlock(id);
    }
    return NULL;
}

int main(void) {
    /* Uncontended: a lone thread always takes the fast path (7 memory operations). */
    for (int i = 0; i < 10; i++) {
        fm_lock(1);
        fm_unlock(1);
    }
    printf("uncontended: fast=%d slow=%d\n", atomic_load(&fast_path_entries), atomic_load(&slow_path_entries));
    check(atomic_load(&fast_path_entries) == 10 && atomic_load(&slow_path_entries) == 0, "uncontended");
    atomic_store(&fast_path_entries, 0);

    pthread_t th[N];
    for (int i = 0; i < N; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)(i + 1));
    for (int i = 0; i < N; i++)
        pthread_join(th[i], NULL);
    int paths = atomic_load(&fast_path_entries) + atomic_load(&slow_path_entries);
    for (int i = 1; i <= N; i++)
        printf("thread %d entered %d times\n", i, entries[i]);
    printf("counter=%ld violations=%d\n", counter, atomic_load(&violations));
    printf("every entry took the fast or slow path exactly once: %s\n", paths == N * PER ? "yes" : "no");
    check(counter == N * PER && atomic_load(&violations) == 0, "exclusion");
    check(paths == N * PER, "path accounting");
    check(atomic_load(&y) == 0 && atomic_load(&x) >= 0, "released");
    for (int i = 0; i <= N; i++)
        check(atomic_load(&b[i]) == 0, "flags cleared");
    return 0;
}
