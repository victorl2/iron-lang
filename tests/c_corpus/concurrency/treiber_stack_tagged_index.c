/*
 * title: Treiber stack over a node pool with tagged 64-bit heads
 * topic: concurrency
 * covers: lock-free stack, index+tag packed in uint64, ABA avoidance, node recycling, conservation check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The head is (tag << 32) | (index + 1), 0 meaning empty. Nodes live in a fixed pool and are
 * recycled immediately after a pop, which is exactly the ABA-prone pattern. Every successful
 * CAS bumps the tag, so a stale head (same index, older tag) can never be installed again.
 *
 * Correctness argument: a node is either in the stack or owned by exactly one thread. A pop CAS
 * succeeds only if the head is bit-identical to the one whose next pointer was read, and the
 * tag proves no push or pop happened in between, so the next read is still valid.
 */
enum { POOL = 96, T = 6, ITERS = 4000 };

typedef struct {
    atomic_uint next; /* index+1 of next node, 0 = end */
    unsigned value;
} Node;

static Node pool[POOL];
static atomic_uint_least64_t head;
static atomic_long ops; /* successful CASes on head */

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static uint64_t pack(uint32_t tag, uint32_t idx1) {
    return ((uint64_t)tag << 32) | idx1;
}

static void push(unsigned idx) {
    uint64_t h = atomic_load_explicit(&head, memory_order_relaxed);
    for (;;) {
        atomic_store_explicit(&pool[idx].next, (unsigned)(h & 0xffffffffu), memory_order_relaxed);
        uint64_t nh = pack((uint32_t)(h >> 32) + 1u, idx + 1u);
        if (atomic_compare_exchange_weak_explicit(&head, &h, nh, memory_order_release, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&ops, 1, memory_order_relaxed);
            return;
        }
    }
}

static int pop(unsigned *out) {
    uint64_t h = atomic_load_explicit(&head, memory_order_acquire);
    for (;;) {
        uint32_t idx1 = (uint32_t)(h & 0xffffffffu);
        if (idx1 == 0)
            return 0;
        unsigned nxt = atomic_load_explicit(&pool[idx1 - 1].next, memory_order_relaxed);
        uint64_t nh = pack((uint32_t)(h >> 32) + 1u, nxt);
        if (atomic_compare_exchange_weak_explicit(&head, &h, nh, memory_order_acquire, memory_order_acquire)) {
            atomic_fetch_add_explicit(&ops, 1, memory_order_relaxed);
            *out = idx1 - 1;
            return 1;
        }
    }
}

static atomic_int pops_done;

static void *worker(void *p) {
    int t = (int)(size_t)p;
    int per = POOL / T;
    for (int k = 0; k < per; k++)
        push((unsigned)(t * per + k));
    int held = 0;
    for (int i = 0; i < ITERS; i++) {
        unsigned n;
        if (pop(&n)) {
            held++;
            atomic_fetch_add(&pops_done, 1);
            push(n); /* immediate reuse: the ABA pattern */
        }
        if (i % 64 == 0)
            sched_yield();
    }
    (void)held;
    return NULL;
}

int main(void) {
    for (unsigned i = 0; i < POOL; i++)
        pool[i].value = i * 3 + 1;
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    unsigned char seen[POOL] = {0};
    unsigned long sum = 0;
    int n = 0;
    unsigned idx;
    while (pop(&idx)) {
        check(idx < POOL, "index range");
        check(seen[idx] == 0, "node appears once");
        seen[idx] = 1;
        sum += pool[idx].value;
        n++;
    }
    check(n == POOL, "all nodes conserved");
    unsigned long want = 0;
    for (unsigned i = 0; i < POOL; i++)
        want += i * 3 + 1;
    check(sum == want, "value checksum");
    /* every successful push or pop bumped the tag exactly once */
    uint64_t h = atomic_load(&head);
    check((uint32_t)(h >> 32) == (uint32_t)atomic_load(&ops), "tag counts successful CASes");
    check((uint32_t)(h & 0xffffffffu) == 0, "empty at the end");
    printf("pool=%d drained=%d value_sum=%lu\n", POOL, n, sum);
    printf("tag equals successful head updates: yes\n");
    printf("recycled pops > 0: %s\n", atomic_load(&pops_done) > 0 ? "yes" : "no");
    return 0;
}
