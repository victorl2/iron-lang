/*
 * title: C11 atomics with pthreads: CAS max, fetch_add, atomic_flag lock
 * topic: concurrency
 * covers: stdatomic, compare_exchange loop, atomic_flag spin lock, fetch_add totals, atomic bit set
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 6, N = 4000 };

static atomic_long total;
static atomic_long maxval;
static atomic_ulong bitset;
static atomic_flag spin = ATOMIC_FLAG_INIT;
static long guarded; /* plain variable protected by the spin flag */
static atomic_int cas_retries_any; /* set to 1 if any CAS ever failed (informational only) */

typedef struct {
    int id;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long value_of(int id, int i) {
    unsigned x = (unsigned)(id * 100003 + i) * 2246822519u;
    x ^= x >> 13;
    return (long)(x % 100000u);
}

static void update_max(long v) {
    long cur = atomic_load(&maxval);
    while (v > cur) {
        if (atomic_compare_exchange_weak(&maxval, &cur, v))
            return;
        atomic_store(&cas_retries_any, 1);
    }
}

static void *worker(void *p) {
    Arg *a = p;
    for (int i = 0; i < N; i++) {
        long v = value_of(a->id, i);
        atomic_fetch_add(&total, v);
        update_max(v);
        atomic_fetch_or(&bitset, 1ul << (unsigned)(v % 40));
        while (atomic_flag_test_and_set_explicit(&spin, memory_order_acquire))
            sched_yield();
        guarded += v % 7;
        atomic_flag_clear_explicit(&spin, memory_order_release);
    }
    return NULL;
}

int main(void) {
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    long etotal = 0, emax = 0, eguard = 0;
    unsigned long ebits = 0;
    for (int id = 0; id < T; id++)
        for (int i = 0; i < N; i++) {
            long v = value_of(id, i);
            etotal += v;
            if (v > emax)
                emax = v;
            ebits |= 1ul << (unsigned)(v % 40);
            eguard += v % 7;
        }
    check(atomic_load(&total) == etotal, "fetch_add total");
    check(atomic_load(&maxval) == emax, "CAS max");
    check(atomic_load(&bitset) == ebits, "bitset");
    check(guarded == eguard, "spin-guarded sum");
    unsigned long b = atomic_load(&bitset);
    int pop = 0;
    for (int i = 0; i < 40; i++)
        pop += (int)((b >> i) & 1ul);
    printf("total=%ld max=%ld\n", atomic_load(&total), atomic_load(&maxval));
    printf("bits set=%d of 40\n", pop);
    printf("guarded=%ld\n", guarded);
    return 0;
}
