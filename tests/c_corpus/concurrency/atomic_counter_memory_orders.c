/*
 * title: Atomic counters under relaxed, acq_rel and seq_cst orders
 * topic: concurrency
 * covers: fetch_add memory orders, unique tickets, RMW atomicity, release sequence, last-arriver publication
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Correctness argument:
 * 1. Every read-modify-write on one atomic object reads the latest value in that object's
 *    modification order, whatever memory order is used. So N fetch_adds of 1 hand out N
 *    distinct tickets 0..N-1, even with memory_order_relaxed.
 * 2. The memory order only matters for OTHER memory. In the arrival phase each thread writes a
 *    plain slot, then does fetch_add(release-or-stronger). The RMWs form a release sequence, so
 *    the thread that gets ticket T-1 (the last RMW, using acquire) synchronizes with every
 *    earlier releaser and may read all plain slots without a data race.
 */
enum { T = 4, N = 3000, MODES = 3 };

static const char *mode_name[MODES] = {"relaxed", "acq_rel", "seq_cst"};

static atomic_uint counter;
static unsigned char seen[T * N];

typedef struct {
    int mode;
    unsigned long sum;
} Arg;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void *worker(void *p) {
    Arg *a = p;
    for (int i = 0; i < N; i++) {
        unsigned t = 0;
        switch (a->mode) {
        case 0: t = atomic_fetch_add_explicit(&counter, 1u, memory_order_relaxed); break;
        case 1: t = atomic_fetch_add_explicit(&counter, 1u, memory_order_acq_rel); break;
        default: t = atomic_fetch_add_explicit(&counter, 1u, memory_order_seq_cst); break;
        }
        seen[t]++; /* ticket is unique, so this index is written by one thread only */
        a->sum += t;
    }
    return NULL;
}

static int slot_data[T];
static atomic_uint arrived;
static long last_sum;
static atomic_int last_count;

static void *arriver(void *p) {
    int id = (int)(size_t)p;
    slot_data[id] = (id + 1) * 1000 + 7; /* plain write, published by the release below */
    unsigned prev = atomic_fetch_add_explicit(&arrived, 1u, memory_order_acq_rel);
    if (prev == T - 1) {
        long s = 0;
        for (int i = 0; i < T; i++)
            s += slot_data[i]; /* plain reads, ordered after every releaser */
        last_sum = s;
        atomic_fetch_add(&last_count, 1);
    }
    return NULL;
}

int main(void) {
    for (int m = 0; m < MODES; m++) {
        atomic_store(&counter, 0u);
        memset(seen, 0, sizeof seen);
        pthread_t th[T];
        Arg args[T];
        for (int i = 0; i < T; i++) {
            args[i].mode = m;
            args[i].sum = 0;
            check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
        }
        unsigned long total = 0;
        for (int i = 0; i < T; i++) {
            pthread_join(th[i], NULL);
            total += args[i].sum;
        }
        int distinct = 0;
        for (int i = 0; i < T * N; i++) {
            check(seen[i] == 1, "ticket handed out exactly once");
            distinct++;
        }
        unsigned long expect = (unsigned long)T * N * (T * N - 1) / 2;
        check(total == expect, "ticket sum");
        check(atomic_load(&counter) == (unsigned)(T * N), "final counter");
        printf("%-8s final=%u distinct=%d ticket_sum=%lu\n", mode_name[m], atomic_load(&counter), distinct,
               total);
    }
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, arriver, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    check(atomic_load(&last_count) == 1, "exactly one last arriver");
    long expect = 0;
    for (int i = 0; i < T; i++)
        expect += (i + 1) * 1000 + 7;
    check(last_sum == expect, "last arriver saw every slot");
    printf("last arriver saw slots summing to %ld, arrivals=%u\n", last_sum, atomic_load(&arrived));
    return 0;
}
