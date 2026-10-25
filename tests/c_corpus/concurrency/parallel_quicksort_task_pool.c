/*
 * title: Parallel quicksort over a shared task stack
 * topic: concurrency
 * covers: range tasks, pending-task counter termination, median-of-three partition, insertion-sort cutoff, multiset checksum
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 30000, CUTOFF = 24, NW = 4, MAXT = 4096 };

typedef struct {
    int lo, hi; /* [lo, hi) */
} Range;

static unsigned a[N];
static Range stack[MAXT];
static int sp;
static long pending;
static long partitions, leaves;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void insertion(int lo, int hi) {
    for (int i = lo + 1; i < hi; i++) {
        unsigned x = a[i];
        int j = i - 1;
        while (j >= lo && a[j] > x) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
}

static void swap(int i, int j) {
    unsigned t = a[i];
    a[i] = a[j];
    a[j] = t;
}

/* Hoare-style partition around a median-of-three pivot; returns split index. */
static int partition(int lo, int hi) {
    int mid = lo + (hi - lo) / 2, last = hi - 1;
    if (a[mid] < a[lo])
        swap(mid, lo);
    if (a[last] < a[lo])
        swap(last, lo);
    if (a[last] < a[mid])
        swap(last, mid);
    unsigned pivot = a[mid];
    int i = lo, j = last;
    while (i <= j) {
        while (a[i] < pivot)
            i++;
        while (a[j] > pivot)
            j--;
        if (i <= j) {
            swap(i, j);
            i++;
            j--;
        }
    }
    return i; /* [lo, j] <= pivot, [i, hi) >= pivot, j < i */
}

static void push_locked(int lo, int hi) {
    check(sp < MAXT, "stack space");
    stack[sp].lo = lo;
    stack[sp].hi = hi;
    sp++;
    pending++;
}

static void *worker(void *arg) {
    pthread_mutex_lock(&mu);
    for (;;) {
        while (sp == 0 && pending > 0)
            pthread_cond_wait(&cv, &mu);
        if (sp == 0)
            break; /* pending == 0: sorting finished */
        Range r = stack[--sp];
        pthread_mutex_unlock(&mu);
        if (r.hi - r.lo <= CUTOFF) {
            insertion(r.lo, r.hi);
            pthread_mutex_lock(&mu);
            leaves++;
            pending--;
        } else {
            int i = partition(r.lo, r.hi);
            /* elements in [lo, i) and [i, hi) are two independent sub-problems; the
               region between j and i (equal to pivot) is already in place when i - j == 2 */
            pthread_mutex_lock(&mu);
            partitions++;
            if (i - r.lo > 1)
                push_locked(r.lo, i);
            if (r.hi - i > 1)
                push_locked(i, r.hi);
            pending--;
        }
        pthread_cond_broadcast(&cv);
    }
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    unsigned s = 0xC0FFEEu;
    unsigned long long sum0 = 0, sq0 = 0, xr0 = 0;
    for (int i = 0; i < N; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        a[i] = i % 50 == 0 ? 7u : s % 1000000u; /* sprinkle duplicates */
        sum0 += a[i];
        sq0 += (unsigned long long)a[i] * a[i];
        xr0 ^= (unsigned long long)a[i] * 0x9e3779b97f4a7c15ULL;
    }
    pthread_mutex_lock(&mu);
    push_locked(0, N);
    pthread_mutex_unlock(&mu);
    pthread_t th[NW];
    for (int i = 0; i < NW; i++)
        check(pthread_create(&th[i], NULL, worker, NULL) == 0, "create");
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);

    unsigned long long sum1 = 0, sq1 = 0, xr1 = 0;
    for (int i = 0; i < N; i++) {
        if (i > 0)
            check(a[i - 1] <= a[i], "sorted");
        sum1 += a[i];
        sq1 += (unsigned long long)a[i] * a[i];
        xr1 ^= (unsigned long long)a[i] * 0x9e3779b97f4a7c15ULL;
    }
    check(sum0 == sum1 && sq0 == sq1 && xr0 == xr1, "same multiset");
    check(sp == 0 && pending == 0, "no work left");
    printf("sorted %d values: min %u median %u max %u\n", N, a[0], a[N / 2], a[N - 1]);
    printf("sum %llu\n", sum1);
    printf("partition tasks %ld, insertion-sort leaves %ld\n", partitions, leaves);
    return 0;
}
