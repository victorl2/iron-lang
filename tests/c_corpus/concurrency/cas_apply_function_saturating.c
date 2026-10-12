/*
 * title: Generic CAS update loop with saturating, budgeted and gcd functions
 * topic: concurrency
 * covers: compare_exchange_weak, function pointer update rules, saturating add, bounded budget, running gcd
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Correctness argument: cas_update() re-reads the current value on every failed CAS and applies
 * the pure function again, so each successful CAS replaces exactly the value the function was
 * computed from. That linearizes each update at its successful CAS. For commutative/associative
 * rules (saturating add without underflow mixing, gcd) the result is order independent; for the
 * budget rule the number of successful takes equals the budget however threads interleave.
 */
typedef uint64_t (*UpdateFn)(uint64_t cur, uint64_t arg, int *applied);

static uint64_t cas_update(atomic_uint_least64_t *a, UpdateFn f, uint64_t arg, int *applied) {
    uint64_t cur = atomic_load_explicit(a, memory_order_relaxed);
    for (;;) {
        int ok = 0;
        uint64_t nv = f(cur, arg, &ok);
        if (!ok) {
            *applied = 0;
            return cur;
        }
        if (atomic_compare_exchange_weak_explicit(a, &cur, nv, memory_order_acq_rel, memory_order_relaxed)) {
            *applied = 1;
            return cur;
        }
    }
}

static uint64_t f_sat_add(uint64_t cur, uint64_t arg, int *ok) {
    *ok = 1;
    return cur + arg > 5000 ? 5000 : cur + arg;
}
static uint64_t f_take_budget(uint64_t cur, uint64_t arg, int *ok) {
    if (cur < arg) {
        *ok = 0;
        return cur;
    }
    *ok = 1;
    return cur - arg;
}
static uint64_t f_gcd(uint64_t cur, uint64_t arg, int *ok) {
    uint64_t a = cur, b = arg;
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    *ok = 1;
    return a;
}
static uint64_t f_floor_dec(uint64_t cur, uint64_t arg, int *ok) {
    *ok = 1;
    return cur > arg ? cur - arg : 0;
}

enum { T = 5 };

static atomic_uint_least64_t sat, budget, gcd_cell, floor_cell;
static atomic_int takes[T];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static uint64_t gcd_input(int t, int i) {
    return 6u * (uint64_t)(1 + (t * 13 + i * 7) % 40) * 35u; /* always a multiple of 210 */
}

static void *worker(void *p) {
    int t = (int)(size_t)p;
    int ok;
    for (int i = 0; i < 1500; i++) {
        cas_update(&sat, f_sat_add, (uint64_t)(1 + (t + i) % 3), &ok);
        cas_update(&gcd_cell, f_gcd, gcd_input(t, i), &ok);
        cas_update(&floor_cell, f_floor_dec, 2, &ok);
    }
    for (int i = 0; i < 4000; i++) {
        cas_update(&budget, f_take_budget, 1, &ok);
        if (ok)
            takes[t]++;
    }
    return NULL;
}

int main(void) {
    atomic_store(&sat, 0);
    atomic_store(&budget, 6001);
    atomic_store(&gcd_cell, 0);
    atomic_store(&floor_cell, 9000);
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    uint64_t g = 0;
    for (int t = 0; t < T; t++)
        for (int i = 0; i < 1500; i++) {
            int ok;
            g = f_gcd(g, gcd_input(t, i), &ok);
        }
    check(atomic_load(&sat) == 5000, "saturated");
    check(atomic_load(&gcd_cell) == g, "gcd");
    check(atomic_load(&floor_cell) == 0, "floor");
    /* budget: 20000 attempts compete for 6001 units, exactly 6001 takes may succeed */
    uint64_t left = atomic_load(&budget);
    long total_takes = 0;
    for (int t = 0; t < T; t++)
        total_takes += takes[t];
    check(left == 0, "budget drained");
    check(total_takes == 6001, "successful takes equal budget");
    printf("saturating counter = %llu\n", (unsigned long long)atomic_load(&sat));
    printf("running gcd = %llu\n", (unsigned long long)atomic_load(&gcd_cell));
    printf("floored counter = %llu\n", (unsigned long long)atomic_load(&floor_cell));
    printf("budget remaining = %llu, successful takes = %ld\n", (unsigned long long)left, total_takes);
    return 0;
}
