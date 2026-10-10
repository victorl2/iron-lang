/*
 * title: SPSC ring buffer in shared memory between parent and child
 * topic: memory
 * covers: MAP_SHARED|MAP_ANON, fork, C11 atomics across processes, acquire/release indices, wraparound, bounded spinning
 * deps: posix
 */
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CAP 64u /* slots, power of two */
#define ITEMS 20000u

typedef struct {
    uint32_t seq;
    uint32_t value;
} Item;

typedef struct {
    _Alignas(64) _Atomic uint32_t head; /* next slot to read (consumer owned) */
    _Alignas(64) _Atomic uint32_t tail; /* next slot to write (producer owned) */
    _Alignas(64) Item slots[CAP];
    _Atomic uint32_t done;
} Ring;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t value_of(uint32_t i) {
    uint32_t x = i * 2654435761u + 0x9E3779B9u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    return x;
}

static int deadline_passed(time_t start) {
    return time(NULL) - start > 20;
}

static int push(Ring *r, Item it, time_t start) {
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    while (t - atomic_load_explicit(&r->head, memory_order_acquire) == CAP) {
        if (deadline_passed(start))
            return -1;
        sched_yield();
    }
    r->slots[t & (CAP - 1)] = it;
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return 0;
}

static int pop(Ring *r, Item *it, time_t start, pid_t child) {
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    while (atomic_load_explicit(&r->tail, memory_order_acquire) == h) {
        if (atomic_load_explicit(&r->done, memory_order_acquire) &&
            atomic_load_explicit(&r->tail, memory_order_acquire) == h)
            return 0;
        if (deadline_passed(start))
            return -1;
        (void)child;
        sched_yield();
    }
    *it = r->slots[h & (CAP - 1)];
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return 1;
}

int main(void) {
    _Atomic uint32_t probe;
    atomic_init(&probe, 0);
    check(atomic_is_lock_free(&probe), "lock-free 32-bit atomics");
    Ring *r = mmap(NULL, sizeof(Ring), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    check(r != MAP_FAILED, "mmap");
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    atomic_init(&r->done, 0);
    fflush(stdout);
    time_t start = time(NULL);

    pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        for (uint32_t i = 0; i < ITEMS; i++) {
            Item it = {i, value_of(i)};
            if (push(r, it, start) != 0)
                _exit(3);
        }
        atomic_store_explicit(&r->done, 1, memory_order_release);
        _exit(0);
    }

    uint32_t expect = 0;
    uint64_t sum = 0;
    unsigned long wraps = 0;
    Item it;
    int rc;
    while ((rc = pop(r, &it, start, pid)) == 1) {
        check(it.seq == expect, "in-order delivery");
        check(it.value == value_of(it.seq), "payload intact");
        sum += it.value;
        expect++;
        if ((expect & (CAP - 1)) == 0)
            wraps++;
    }
    check(rc == 0, "consumer finished without timeout");
    int st = 0;
    check(waitpid(pid, &st, 0) == pid, "waitpid");
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "producer exit");
    printf("items received: %u\n", (unsigned)expect);
    printf("checksum: %llu\n", (unsigned long long)sum);
    printf("ring wraparounds: %lu\n", wraps);
    printf("final head=%u tail=%u\n", (unsigned)atomic_load(&r->head), (unsigned)atomic_load(&r->tail));
    check(expect == ITEMS, "all items");
    uint64_t want = 0;
    for (uint32_t i = 0; i < ITEMS; i++)
        want += value_of(i);
    check(sum == want, "checksum");
    check(munmap(r, sizeof(Ring)) == 0, "munmap");
    return 0;
}
