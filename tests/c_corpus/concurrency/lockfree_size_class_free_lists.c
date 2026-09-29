/*
 * title: Size-class free lists over an atomic bump arena
 * topic: concurrency
 * covers: lock-free allocator, tagged free lists, fetch_add bump pointer, block signatures, exclusive ownership
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Blocks come in two sizes (16 and 48 bytes). Each class has a lock-free free list whose head is
 * (tag << 32 | offset+1). When a list is empty the allocator takes fresh bytes with fetch_add on
 * the arena top, so an offset is never handed out twice by the arena. Freed blocks return to
 * their class list and are recycled.
 *
 * Correctness argument: the tag defeats ABA on the lists; the arena bump is a single RMW. Each
 * thread stamps its whole block with a signature, yields, and re-checks it. Any double
 * hand-out would let another thread overwrite the stamp and be detected.
 */
enum { ARENA = 1 << 16, T = 6, ITERS = 4000, HOLD = 4, NCLASS = 2 };

static const unsigned class_size[NCLASS] = {16, 48};
static unsigned char arena[ARENA];
static atomic_uint arena_top;
static atomic_uint_least64_t list_head[NCLASS];
static atomic_int created[NCLASS];
static atomic_int bad;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* the link lives in the first 4 bytes of a free block */
static uint32_t alloc_block(int cls) {
    uint64_t h = atomic_load(&list_head[cls]);
    for (;;) {
        uint32_t off1 = (uint32_t)h;
        if (off1 == 0)
            break;
        uint32_t nxt;
        memcpy(&nxt, arena + off1 - 1, sizeof nxt); /* racy by design; validated by the tag */
        uint64_t nh = ((uint64_t)((uint32_t)(h >> 32) + 1) << 32) | nxt;
        if (atomic_compare_exchange_weak(&list_head[cls], &h, nh))
            return off1 - 1;
    }
    unsigned off = atomic_fetch_add(&arena_top, class_size[cls]);
    check(off + class_size[cls] <= ARENA, "arena exhausted");
    atomic_fetch_add(&created[cls], 1);
    return off;
}

static void free_block(int cls, uint32_t off) {
    uint64_t h = atomic_load(&list_head[cls]);
    uint64_t nh;
    do {
        uint32_t nxt = (uint32_t)h;
        memcpy(arena + off, &nxt, sizeof nxt);
        nh = ((uint64_t)((uint32_t)(h >> 32) + 1) << 32) | (off + 1);
    } while (!atomic_compare_exchange_weak(&list_head[cls], &h, nh));
}

typedef struct {
    int cls;
    uint32_t off;
} Held;

static void *worker(void *p) {
    unsigned id = (unsigned)(size_t)p + 1;
    unsigned s = id * 2654435761u;
    Held held[HOLD];
    int n = 0;
    for (int i = 0; i < ITERS; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        if (n < HOLD && (s & 1u)) {
            int cls = (int)((s >> 4) & 1u);
            uint32_t off = alloc_block(cls);
            unsigned char sig = (unsigned char)(id * 16 + 1 + (unsigned)(i & 7));
            memset(arena + off, sig, class_size[cls]);
            held[n].cls = cls;
            held[n].off = off;
            n++;
        } else if (n > 0) {
            int k = (int)((s >> 8) % (unsigned)n);
            Held h = held[k];
            held[k] = held[--n];
            unsigned char first = arena[h.off];
            for (unsigned j = 0; j < class_size[h.cls]; j++)
                if (arena[h.off + j] != first)
                    atomic_fetch_add(&bad, 1);
            if ((first >> 4) != id)
                atomic_fetch_add(&bad, 1);
            free_block(h.cls, h.off);
        }
        if ((s & 31u) == 0)
            sched_yield();
    }
    while (n > 0) {
        Held h = held[--n];
        free_block(h.cls, h.off);
    }
    return NULL;
}

int main(void) {
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    check(atomic_load(&bad) == 0, "no block was handed out twice");
    unsigned total_bytes = 0;
    for (int c = 0; c < NCLASS; c++) {
        int in_list = 0;
        uint32_t off1 = (uint32_t)atomic_load(&list_head[c]);
        while (off1) {
            in_list++;
            check(in_list <= atomic_load(&created[c]), "free list longer than creations");
            memcpy(&off1, arena + off1 - 1, sizeof off1);
        }
        check(in_list == atomic_load(&created[c]), "every created block is back in its free list");
        check(created[c] >= 1 && created[c] <= T * HOLD, "creations bounded by concurrent holds");
        total_bytes += (unsigned)in_list * class_size[c];
    }
    check(total_bytes == atomic_load(&arena_top), "arena bytes accounted");
    printf("double hand-outs detected: %d\n", atomic_load(&bad));
    printf("all created blocks recycled into free lists: yes\n");
    printf("blocks created per class bounded by %d: yes\n", T * HOLD);
    return 0;
}
