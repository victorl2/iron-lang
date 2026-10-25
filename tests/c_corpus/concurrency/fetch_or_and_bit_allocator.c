/*
 * title: 64-slot bit allocator with fetch_or claim and fetch_and release
 * topic: concurrency
 * covers: atomic_fetch_or, atomic_fetch_and, single-word bitmap, exclusive ownership audit, lowest-free-first
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Correctness argument: to claim bit b a thread does fetch_or(word, 1<<b) and looks at the OLD
 * value. Among all threads racing for bit b, exactly one sees the old bit clear: that thread owns
 * the slot. Release does fetch_and(word, ~(1<<b)), legal only for the owner. An owner table
 * (exchange to the caller id on claim, exchange to 0 on release) audits that no slot is ever
 * owned by two threads at the same time.
 */
enum { T = 6, ROUNDS = 3000, HOLD = 5 };

static atomic_uint_least64_t word;
static atomic_int owner[64];
static atomic_int violations;
static atomic_long claims;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* returns slot or -1 when all 64 bits are taken */
static int alloc_slot(void) {
    for (;;) {
        uint64_t w = atomic_load_explicit(&word, memory_order_relaxed);
        if (w == UINT64_MAX)
            return -1;
        int b = 0;
        while ((w >> b) & 1u)
            b++;
        uint64_t m = (uint64_t)1 << b;
        uint64_t old = atomic_fetch_or_explicit(&word, m, memory_order_acquire);
        if (!(old & m))
            return b;
        /* lost the race for this bit; rescan */
    }
}

static void free_slot(int b) {
    atomic_fetch_and_explicit(&word, ~((uint64_t)1 << b), memory_order_release);
}

static unsigned next_rand(unsigned *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void *worker(void *p) {
    int id = (int)(size_t)p + 1;
    unsigned rs = 0x9e3779b9u * (unsigned)id;
    int held[HOLD];
    int nheld = 0;
    for (int r = 0; r < ROUNDS; r++) {
        unsigned x = next_rand(&rs);
        if (nheld < HOLD && (x & 3u) != 0) {
            int b = alloc_slot();
            if (b >= 0) {
                if (atomic_exchange(&owner[b], id) != 0)
                    atomic_fetch_add(&violations, 1);
                held[nheld++] = b;
                atomic_fetch_add(&claims, 1);
            }
        } else if (nheld > 0) {
            int k = (int)((x >> 8) % (unsigned)nheld);
            int b = held[k];
            held[k] = held[--nheld];
            if (atomic_exchange(&owner[b], 0) != id)
                atomic_fetch_add(&violations, 1);
            free_slot(b);
        }
        if ((x & 15u) == 0)
            sched_yield();
    }
    while (nheld > 0) {
        int b = held[--nheld];
        if (atomic_exchange(&owner[b], 0) != id)
            atomic_fetch_add(&violations, 1);
        free_slot(b);
    }
    return NULL;
}

int main(void) {
    /* Deterministic single-threaded behavior first. */
    int got[64];
    for (int i = 0; i < 64; i++) {
        got[i] = alloc_slot();
        check(got[i] == i, "lowest free first");
    }
    check(alloc_slot() == -1, "full word");
    printf("allocated 0..63 in order, 65th allocation fails\n");
    free_slot(5);
    free_slot(40);
    free_slot(0);
    int a = alloc_slot();
    int b = alloc_slot();
    int c = alloc_slot();
    check(a == 0 && b == 5 && c == 40, "reuse lowest freed");
    printf("after freeing 5,40,0 the next allocations are %d %d %d\n", a, b, c);
    for (int i = 0; i < 64; i++)
        free_slot(i);
    check(atomic_load(&word) == 0, "empty after frees");

    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    check(atomic_load(&violations) == 0, "exclusive ownership");
    check(atomic_load(&word) == 0, "all slots returned");
    for (int i = 0; i < 64; i++)
        check(atomic_load(&owner[i]) == 0, "owner table clear");
    check(atomic_load(&claims) > 0, "some claims");
    printf("concurrent phase: violations=%d final word=%llu\n", atomic_load(&violations),
           (unsigned long long)atomic_load(&word));
    return 0;
}
