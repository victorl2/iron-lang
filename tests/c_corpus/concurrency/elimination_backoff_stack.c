/*
 * title: Lock-free stack with an elimination backoff array
 * topic: concurrency
 * covers: Treiber stack, contention backoff, exchanger slots, push/pop pairing, tagged head, value conservation
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * When a CAS on the stack head fails (contention), a thread visits a random slot of the
 * elimination array instead of retrying at once. A pusher parks its value in an EMPTY slot as
 * WAITING(value); a popper that finds a WAITING slot CASes it to TAKEN and returns that value.
 * The pair cancels out without touching the stack: the push and the pop linearize at the
 * popper's CAS. If nobody takes the value within a short spin, the pusher CASes the slot back
 * to EMPTY; if that CAS fails, a popper took the value in the meantime and the push is done.
 *
 * Slot word: (value << 2) | state, states 0=EMPTY 1=WAITING 2=TAKEN. Only the parked pusher
 * resets a TAKEN slot to EMPTY, so the slot cannot suffer ABA.
 *
 * Oracle: values are unique per pushed item; every value must be popped exactly once, counting
 * the values left in the stack after the run.
 */
enum { POOL = 512, T = 6, ITERS = 2500, NSLOTS = 3, SPINS = 40 };

typedef struct {
    atomic_uint next;
    unsigned value;
} Node;

static Node pool[POOL];
static atomic_uint_least64_t head;
static atomic_uint_least64_t slot[NSLOTS];
static atomic_uint popped_flag[T * ITERS + 1];
static atomic_long eliminated_pairs;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static int try_push_node(unsigned n) {
    uint64_t h = atomic_load(&head);
    atomic_store(&pool[n].next, (unsigned)h);
    return atomic_compare_exchange_strong(&head, &h, (((h >> 32) + 1) << 32) | (n + 1u));
}

static int try_pop_node(unsigned *n, int *empty) {
    uint64_t h = atomic_load(&head);
    unsigned idx1 = (unsigned)h;
    *empty = 0;
    if (idx1 == 0) {
        *empty = 1;
        return 0;
    }
    unsigned nx = atomic_load(&pool[idx1 - 1].next);
    if (atomic_compare_exchange_strong(&head, &h, (((h >> 32) + 1) << 32) | nx)) {
        *n = idx1 - 1;
        return 1;
    }
    return 0;
}

/* pusher side of the exchanger: returns 1 if a popper took the value */
static int elim_push(int s, unsigned value) {
    uint64_t empty = 0;
    uint64_t mine = ((uint64_t)value << 2) | 1u;
    if (!atomic_compare_exchange_strong(&slot[s], &empty, mine))
        return 0;
    for (int i = 0; i < SPINS; i++) {
        if (atomic_load(&slot[s]) != mine)
            break;
        sched_yield();
    }
    uint64_t expect = mine;
    if (atomic_compare_exchange_strong(&slot[s], &expect, 0))
        return 0; /* nobody came */
    atomic_store(&slot[s], 0); /* it was TAKEN: reset the slot */
    return 1;
}

/* popper side: returns 1 and the value if a waiting pusher was found */
static int elim_pop(int s, unsigned *value) {
    uint64_t cur = atomic_load(&slot[s]);
    if ((cur & 3u) != 1u)
        return 0;
    if (atomic_compare_exchange_strong(&slot[s], &cur, 2u)) {
        *value = (unsigned)(cur >> 2);
        return 1;
    }
    return 0;
}

typedef struct {
    int id;
} Arg;

static void *worker(void *p) {
    int id = ((Arg *)p)->id;
    unsigned rs = 0x1234567u + 7919u * (unsigned)id;
    /* every thread owns POOL/T nodes, recycled between push and pop through the stack */
    unsigned mine[POOL];
    int nmine = 0;
    for (int k = 0; k < POOL / T; k++)
        mine[nmine++] = (unsigned)(id * (POOL / T) + k);
    for (int i = 0; i < ITERS; i++) {
        unsigned value = 1u + (unsigned)id * ITERS + (unsigned)i;
        if (nmine > 0) {
            unsigned n = mine[--nmine];
            pool[n].value = value;
            int done = 0;
            while (!done) {
                if (try_push_node(n)) {
                    done = 1;
                } else {
                    rs = rs * 1664525u + 1013904223u;
                    if (elim_push((int)((rs >> 16) % NSLOTS), value)) {
                        atomic_fetch_add(&eliminated_pairs, 1);
                        mine[nmine++] = n; /* node never entered the stack */
                        done = 1;
                    }
                }
            }
        }
        /* pop one item (if any) and count it */
        for (;;) {
            unsigned n, v;
            int empty;
            if (try_pop_node(&n, &empty)) {
                v = pool[n].value;
                mine[nmine++] = n;
                atomic_fetch_add(&popped_flag[v], 1);
                break;
            }
            if (empty)
                break;
            rs = rs * 1664525u + 1013904223u;
            if (elim_pop((int)((rs >> 16) % NSLOTS), &v)) {
                atomic_fetch_add(&popped_flag[v], 1);
                break;
            }
        }
    }
    return NULL;
}

int main(void) {
    for (int s = 0; s < NSLOTS; s++)
        atomic_store(&slot[s], 0);
    pthread_t th[T];
    Arg args[T];
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    unsigned n, left = 0;
    int empty;
    while (try_pop_node(&n, &empty)) {
        atomic_fetch_add(&popped_flag[pool[n].value], 1);
        left++;
    }
    long delivered = 0;
    for (unsigned v = 1; v <= (unsigned)T * ITERS; v++) {
        unsigned c = atomic_load(&popped_flag[v]);
        check(c <= 1, "a value was delivered twice");
        delivered += c;
    }
    check(delivered == (long)T * ITERS, "every pushed value delivered exactly once");
    for (int s = 0; s < NSLOTS; s++)
        check(atomic_load(&slot[s]) == 0, "exchanger slots empty");
    /* deterministic exchanger unit test */
    atomic_store(&slot[0], ((uint64_t)4242 << 2) | 1u);
    unsigned got = 0;
    check(elim_pop(0, &got) && got == 4242, "pop takes a parked value");
    check(atomic_load(&slot[0]) == 2u, "slot marked TAKEN");
    check(!elim_pop(0, &got), "a TAKEN slot cannot be taken twice");
    atomic_store(&slot[0], 0);
    check(!elim_pop(0, &got), "an EMPTY slot yields nothing");
    printf("values pushed=%d delivered=%ld duplicates=0\n", T * ITERS, delivered);
    check(left <= (unsigned)POOL, "leftovers bounded by the pool");
    printf("exchanger unit test: ok\n");
    return 0;
}
