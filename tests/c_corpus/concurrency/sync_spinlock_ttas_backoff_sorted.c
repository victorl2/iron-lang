/*
 * title: TTAS spinlock with exponential backoff and sorted inserts
 * topic: concurrency
 * covers: test-and-test-and-set, exponential backoff, atomic_exchange, sorted array invariant, multiset check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_int lockword; /* 0 free, 1 held */

static void ttas_lock(void) {
    int backoff = 1;
    for (;;) {
        while (atomic_load_explicit(&lockword, memory_order_relaxed) != 0) {
            for (int i = 0; i < backoff; i++)
                sched_yield();
            if (backoff < 8)
                backoff <<= 1; /* exponential backoff, capped */
        }
        if (atomic_exchange_explicit(&lockword, 1, memory_order_acquire) == 0)
            return;
    }
}
static void ttas_unlock(void) { atomic_store_explicit(&lockword, 0, memory_order_release); }

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { T = 4, PER = 200, CAP = T * PER, VALUE_RANGE = 500 };

static int arr[CAP];
static int count;
static atomic_int in_cs, violations;

static int value_for(int id, int k) {
    unsigned x = (unsigned)(id * 7919 + k * 104729 + 13);
    x ^= x >> 7;
    x *= 2654435761u;
    return (int)((x >> 8) % VALUE_RANGE);
}

static void *inserter(void *p) {
    int id = (int)(intptr_t)p;
    for (int k = 0; k < PER; k++) {
        int v = value_for(id, k);
        ttas_lock();
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        int pos = count;
        while (pos > 0 && arr[pos - 1] > v) {
            arr[pos] = arr[pos - 1];
            if ((pos & 31) == 0)
                sched_yield();
            pos--;
        }
        arr[pos] = v;
        count++;
        atomic_fetch_sub(&in_cs, 1);
        ttas_unlock();
    }
    return NULL;
}

int main(void) {
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, inserter, (void *)(intptr_t)i);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    int sorted = 1;
    for (int i = 1; i < count; i++)
        if (arr[i - 1] > arr[i])
            sorted = 0;
    /* multiset check by histogram against a sequential recomputation */
    static int hist_expect[VALUE_RANGE], hist_got[VALUE_RANGE];
    for (int id = 0; id < T; id++)
        for (int k = 0; k < PER; k++)
            hist_expect[value_for(id, k)]++;
    for (int i = 0; i < count; i++)
        hist_got[arr[i]]++;
    int same = 1;
    for (int v = 0; v < VALUE_RANGE; v++)
        if (hist_expect[v] != hist_got[v])
            same = 0;
    long sum = 0;
    for (int i = 0; i < count; i++)
        sum += arr[i];
    printf("count=%d sorted=%s multiset preserved=%s\n", count, sorted ? "yes" : "no", same ? "yes" : "no");
    printf("min=%d median=%d max=%d sum=%ld\n", arr[0], arr[count / 2], arr[count - 1], sum);
    printf("violations %d, lock released %s\n", atomic_load(&violations), atomic_load(&lockword) == 0 ? "yes" : "no");
    check(count == CAP, "count");
    check(sorted && same, "array");
    check(atomic_load(&violations) == 0, "exclusion");
    check(atomic_load(&lockword) == 0, "released");
    return 0;
}
