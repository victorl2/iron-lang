/*
 * title: Generation-checked handles over a lock-free slot map
 * topic: concurrency
 * covers: (generation,index) handles, stale handle detection, exactly-once free under a race, tagged free list, slot reuse
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Each slot has a generation counter: even = free, odd = live. A handle is (gen << 32 | index).
 *   alloc: pop a slot from the tagged free list, gen += 1 (becomes odd), return (gen, index).
 *   free:  CAS(gen, handle.gen -> handle.gen + 1) then push the slot on the free list.
 *   valid: gen == handle.gen.
 * The CAS in free() succeeds for exactly one caller even if many race with the same handle, so a
 * double free is detected and refused instead of corrupting the free list. Once freed and
 * reallocated the slot has a higher generation, so old handles are recognized as stale (this is
 * the ABA defence at the API level).
 */
enum { SLOTS = 64, T = 6, ITERS = 3000 };

typedef struct {
    atomic_uint gen;
    atomic_uint free_next; /* index+1 */
    atomic_int payload;
} Slot;

static Slot slots[SLOTS];
static atomic_uint_least64_t free_head;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void free_list_push(unsigned idx) {
    uint64_t h = atomic_load(&free_head);
    uint64_t nh;
    do {
        atomic_store(&slots[idx].free_next, (unsigned)h);
        nh = (((h >> 32) + 1) << 32) | (idx + 1u);
    } while (!atomic_compare_exchange_weak(&free_head, &h, nh));
}

static int free_list_pop(unsigned *idx) {
    uint64_t h = atomic_load(&free_head);
    for (;;) {
        unsigned i1 = (unsigned)h;
        if (i1 == 0)
            return 0;
        unsigned nx = atomic_load(&slots[i1 - 1].free_next);
        if (atomic_compare_exchange_weak(&free_head, &h, (((h >> 32) + 1) << 32) | nx)) {
            *idx = i1 - 1;
            return 1;
        }
    }
}

static uint64_t handle_alloc(int payload) {
    unsigned idx;
    while (!free_list_pop(&idx))
        sched_yield();
    unsigned g = atomic_fetch_add(&slots[idx].gen, 1u) + 1u; /* even -> odd */
    atomic_store(&slots[idx].payload, payload);
    return ((uint64_t)g << 32) | idx;
}

static int handle_valid(uint64_t h) {
    return atomic_load(&slots[(unsigned)h].gen) == (unsigned)(h >> 32);
}

static int handle_free(uint64_t h) {
    unsigned idx = (unsigned)h, g = (unsigned)(h >> 32);
    unsigned expect = g;
    if (!atomic_compare_exchange_strong(&slots[idx].gen, &expect, g + 1u))
        return 0;
    free_list_push(idx);
    return 1;
}

static atomic_long free_wins;
static atomic_long stale_lookups_ok;
static uint64_t shared_handles[SLOTS / 2];

static void *racer(void *p) {
    (void)p;
    for (int i = 0; i < SLOTS / 2; i++)
        if (handle_free(shared_handles[i]))
            atomic_fetch_add(&free_wins, 1);
    return NULL;
}

static atomic_int bad;

static void *churn(void *p) {
    int id = (int)(size_t)p;
    uint64_t mine[4];
    int n = 0;
    unsigned s = 0xfeedu + (unsigned)id * 977u;
    for (int i = 0; i < ITERS; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        if (n < 4 && (s & 1u)) {
            mine[n] = handle_alloc(id * 100000 + i);
            if (!handle_valid(mine[n]))
                atomic_fetch_add(&bad, 1);
            n++;
        } else if (n > 0) {
            uint64_t h = mine[--n];
            if (!handle_valid(h))
                atomic_fetch_add(&bad, 1);
            if (!handle_free(h))
                atomic_fetch_add(&bad, 1);
            if (handle_valid(h) || handle_free(h)) /* stale now, and double free is refused */
                atomic_fetch_add(&bad, 1);
            else
                atomic_fetch_add(&stale_lookups_ok, 1);
        }
    }
    while (n > 0)
        handle_free(mine[--n]);
    return NULL;
}

int main(void) {
    for (int i = SLOTS - 1; i >= 0; i--)
        free_list_push((unsigned)i);
    /* phase 1: many threads race to free the same handles */
    for (int i = 0; i < SLOTS / 2; i++)
        shared_handles[i] = handle_alloc(i);
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, racer, NULL) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    check(atomic_load(&free_wins) == SLOTS / 2, "each handle freed exactly once");
    for (int i = 0; i < SLOTS / 2; i++)
        check(!handle_valid(shared_handles[i]), "handle stale after free");
    printf("phase 1: %d handles, %d racing freers, successful frees=%ld\n", SLOTS / 2, T, atomic_load(&free_wins));

    /* phase 2: reuse makes older handles permanently stale */
    uint64_t old = shared_handles[0];
    int reused = 0;
    unsigned newgen = 0;
    uint64_t fresh[SLOTS];
    for (int i = 0; i < SLOTS; i++) {
        fresh[i] = handle_alloc(-i);
        if ((unsigned)fresh[i] == (unsigned)old) {
            reused = 1;
            newgen = (unsigned)(fresh[i] >> 32);
        }
    }
    check(reused, "slot of the old handle was reused");
    check(!handle_valid(old), "old handle stays stale after reuse");
    printf("phase 2: old handle's slot reused, old handle valid=%d, new generation is %u\n", handle_valid(old), newgen);
    for (int i = 0; i < SLOTS; i++)
        check(handle_free(fresh[i]), "free fresh");

    /* phase 3: churn */
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, churn, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    check(atomic_load(&bad) == 0, "handle protocol violations");
    int nfree = 0;
    unsigned idx;
    while (free_list_pop(&idx)) {
        nfree++;
        check((atomic_load(&slots[idx].gen) & 1u) == 0, "free slots have even generation");
    }
    check(nfree == SLOTS, "all slots free");
    printf("phase 3: violations=%d, double frees refused and stale lookups verified=%s, free slots=%d\n",
           atomic_load(&bad), atomic_load(&stale_lookups_ok) > 0 ? "yes" : "no", nfree);
    return 0;
}
