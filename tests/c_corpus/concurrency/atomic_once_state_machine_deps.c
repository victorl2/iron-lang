/*
 * title: Lazy initialization state machine with dependency chain
 * topic: concurrency
 * covers: CAS UNINIT->RUNNING, release READY, spin wait, recursive init of dependencies, exactly-once init, no locks
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Resource i is built lazily from resource i-1 (resource 0 has no dependency). get(i) does:
 *   READY   -> return the value (acquire load pairs with the initializer's release store)
 *   UNINIT  -> CAS to RUNNING, initialize (which itself calls get(i-1)), store READY (release)
 *   RUNNING -> another thread is initializing: spin until READY
 * A thread initializing i waits only for resources below i, and the initializer of resource j
 * waits only for resources below j, so waits always point to smaller indices and cannot cycle.
 *
 * Oracle: every resource is initialized exactly once, and each value equals the folded chain
 * value(i) = mix(value(i-1), i) regardless of who initialized it.
 */
enum { N = 24, T = 6, UNINIT = 0, RUNNING = 1, READY = 2 };

typedef struct {
    atomic_int state;
    atomic_int init_count;
    unsigned value; /* plain: written before the release store to READY */
} Slot;

static Slot slots[N];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static unsigned mix(unsigned prev, unsigned i) {
    unsigned x = prev * 2654435761u + i * 40503u + 12345u;
    return x ^ (x >> 13);
}

static unsigned get(int i) {
    Slot *s = &slots[i];
    for (;;) {
        int st = atomic_load_explicit(&s->state, memory_order_acquire);
        if (st == READY)
            return s->value;
        if (st == UNINIT) {
            int expect = UNINIT;
            if (atomic_compare_exchange_strong_explicit(&s->state, &expect, RUNNING, memory_order_acquire,
                                                        memory_order_relaxed)) {
                unsigned dep = i == 0 ? 0u : get(i - 1); /* recursive dependency */
                atomic_fetch_add(&s->init_count, 1);
                sched_yield(); /* widen the window for racing threads */
                s->value = mix(dep, (unsigned)i);
                atomic_store_explicit(&s->state, READY, memory_order_release);
                return s->value;
            }
        } else {
            sched_yield();
        }
    }
}

static unsigned thread_sum[T];
static const int strides[T] = {1, 5, 7, 11, 13, 23};

static void *worker(void *p) {
    int t = (int)(size_t)p;
    unsigned sum = 0;
    /* different entry points: thread t asks for a scattered order of resources */
    for (int k = 0; k < N; k++) {
        int i = (k * strides[t] + t * 5) % N; /* strides are coprime with N: a permutation */
        sum += get(i);
    }
    thread_sum[t] = sum;
    return NULL;
}

int main(void) {
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    unsigned ref = 0, total = 0;
    for (int i = 0; i < N; i++) {
        ref = mix(ref, (unsigned)i);
        check(atomic_load(&slots[i].init_count) == 1, "initialized exactly once");
        check(atomic_load(&slots[i].state) == READY, "ready");
        check(slots[i].value == ref, "chain value");
        total += ref;
    }
    for (int i = 0; i < T; i++)
        check(thread_sum[i] == total, "every thread saw every value");
    printf("resources=%d each initialized once\n", N);
    printf("value[0]=%u value[%d]=%u chain sum=%u\n", slots[0].value, N - 1, slots[N - 1].value, total);
    printf("all %d threads read the same total: yes\n", T);
    return 0;
}
