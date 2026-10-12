/*
 * title: Memory-model litmus tests with forbidden-outcome counting
 * topic: concurrency
 * covers: store buffering with seq_cst, message passing with release/acquire, IRIW with seq_cst, read-read coherence, per-iteration spin barrier
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Each litmus test runs many iterations. Threads meet at a reusable spin barrier, run their few
 * instructions, meet again, and thread 0 classifies the outcome. Outcomes the C11 model FORBIDS
 * must never appear (the count must be exactly 0); outcomes that are merely allowed are not
 * printed because they depend on the machine and the schedule.
 *
 *   SB   (seq_cst):        T0: x=1; r0=y     T1: y=1; r1=x       forbidden: r0==0 && r1==0
 *   MP   (release/acquire):T0: d=42; f=1(rel) T1: r0=f(acq); r1=d forbidden: r0==1 && r1!=42
 *   IRIW (seq_cst):        W(x) W(y) and two readers that read x,y and y,x in opposite order;
 *                          forbidden: readers disagree on the order of the two writes
 *   CoRR (relaxed):        T0: x=1; x=2      T1: r0=x; r1=x       forbidden: r0==2 && r1==1
 */
enum { ITERS = 4000 };

typedef struct {
    atomic_int count, gen;
    int n;
} Barrier;

static void barrier_wait(Barrier *b) {
    int g = atomic_load(&b->gen);
    if (atomic_fetch_add(&b->count, 1) == b->n - 1) {
        atomic_store(&b->count, 0);
        atomic_store(&b->gen, g + 1);
    } else {
        while (atomic_load(&b->gen) == g)
            sched_yield();
    }
}

static Barrier bar;
static atomic_int x, y, d, f;
static int r[4];
static long forbidden[4];
static long iterations[4];
static int test_id;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void reset(void) {
    atomic_store_explicit(&x, 0, memory_order_relaxed);
    atomic_store_explicit(&y, 0, memory_order_relaxed);
    atomic_store_explicit(&d, 0, memory_order_relaxed);
    atomic_store_explicit(&f, 0, memory_order_relaxed);
    for (int i = 0; i < 4; i++)
        r[i] = -1;
}

static void *thread_main(void *p) {
    int id = (int)(size_t)p;
    for (int it = 0; it < ITERS; it++) {
        if (id == 0)
            reset();
        barrier_wait(&bar);
        switch (test_id) {
        case 0: /* SB */
            if (id == 0) {
                atomic_store(&x, 1);
                r[0] = atomic_load(&y);
            } else if (id == 1) {
                atomic_store(&y, 1);
                r[1] = atomic_load(&x);
            }
            break;
        case 1: /* MP */
            if (id == 0) {
                atomic_store_explicit(&d, 42, memory_order_relaxed);
                atomic_store_explicit(&f, 1, memory_order_release);
            } else if (id == 1) {
                r[0] = atomic_load_explicit(&f, memory_order_acquire);
                r[1] = atomic_load_explicit(&d, memory_order_relaxed);
            }
            break;
        case 2: /* IRIW */
            if (id == 0) {
                atomic_store(&x, 1);
            } else if (id == 1) {
                atomic_store(&y, 1);
            } else if (id == 2) {
                r[0] = atomic_load(&x);
                r[1] = atomic_load(&y);
            } else {
                r[2] = atomic_load(&y);
                r[3] = atomic_load(&x);
            }
            break;
        default: /* CoRR */
            if (id == 0) {
                atomic_store_explicit(&x, 1, memory_order_relaxed);
                atomic_store_explicit(&x, 2, memory_order_relaxed);
            } else if (id == 1) {
                r[0] = atomic_load_explicit(&x, memory_order_relaxed);
                r[1] = atomic_load_explicit(&x, memory_order_relaxed);
            }
            break;
        }
        barrier_wait(&bar);
        if (id == 0) {
            iterations[test_id]++;
            switch (test_id) {
            case 0: if (r[0] == 0 && r[1] == 0) forbidden[0]++; break;
            case 1: if (r[0] == 1 && r[1] != 42) forbidden[1]++; break;
            case 2:
                if (r[0] == 1 && r[1] == 0 && r[2] == 1 && r[3] == 0)
                    forbidden[2]++;
                if (r[0] == 0 && r[1] == 1 && r[2] == 0 && r[3] == 1)
                    forbidden[2]++;
                break;
            default: if (r[0] == 2 && r[1] == 1) forbidden[3]++; break;
            }
        }
        barrier_wait(&bar);
    }
    return NULL;
}

int main(void) {
    static const char *names[4] = {"SB seq_cst", "MP release/acquire", "IRIW seq_cst", "CoRR relaxed"};
    static const int nthreads[4] = {2, 2, 4, 2};
    for (int t = 0; t < 4; t++) {
        test_id = t;
        atomic_store(&bar.count, 0);
        atomic_store(&bar.gen, 0);
        bar.n = nthreads[t];
        pthread_t th[4];
        for (int i = 0; i < nthreads[t]; i++)
            check(pthread_create(&th[i], NULL, thread_main, (void *)(size_t)i) == 0, "create");
        for (int i = 0; i < nthreads[t]; i++)
            pthread_join(th[i], NULL);
        check(iterations[t] == ITERS, "all iterations classified");
        check(forbidden[t] == 0, "forbidden outcome observed");
        printf("%-20s iterations=%ld forbidden outcomes=%ld\n", names[t], iterations[t], forbidden[t]);
    }
    return 0;
}
