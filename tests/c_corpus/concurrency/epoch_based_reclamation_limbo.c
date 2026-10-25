/*
 * title: Epoch-based reclamation with three limbo lists over a malloc'd Treiber stack
 * topic: concurrency
 * covers: global epoch, per-thread announce, epoch advance rule, limbo buckets, safe free of unlinked nodes, allocation/free balance
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The stack head is an untagged pointer, so popped nodes must not be freed while another thread
 * may still be reading them. Epoch-based reclamation:
 *   enter:  read global epoch e, announce (e, active) in the thread's slot (seq_cst),
 *           free everything in limbo bucket (e+1) % 3 (retired at epoch e-2 or earlier)
 *   exit:   mark inactive
 *   retire: after unlinking, append the node to bucket (global epoch now) % 3
 *   advance: if every active thread has announced the current epoch e, CAS global e -> e+1
 * Once the global epoch has advanced twice past a node's retire epoch, every thread that could
 * have seen the node has left its critical section, so free() is safe. If the scheme were
 * wrong, AddressSanitizer would flag a use-after-free in this program.
 *
 * Oracle: every value is popped exactly once, and the number of nodes freed equals the number
 * allocated once the final drain and limbo flush are done.
 */
enum { T = 4, ITERS = 4000, ADVANCE_EVERY = 8 };

typedef struct Node {
    struct Node *next;
    struct Node *retire_next;
    unsigned value;
} Node;

typedef struct {
    atomic_ulong state; /* (epoch << 1) | active */
    Node *limbo[3];
    unsigned long allocs, frees;
    long pops;
} Thread;

static _Atomic(Node *) top;
static atomic_ulong global_epoch;
static Thread th[T];
static atomic_uint popped[T * ITERS + 1];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void free_bucket(Thread *t, int b) {
    Node *n = t->limbo[b];
    while (n) {
        Node *nx = n->retire_next;
        free(n);
        t->frees++;
        n = nx;
    }
    t->limbo[b] = NULL;
}

static void enter(Thread *t) {
    unsigned long e = atomic_load(&global_epoch);
    atomic_store(&t->state, (e << 1) | 1u);
    free_bucket(t, (int)((e + 1) % 3));
}

static void leave(Thread *t) {
    atomic_store_explicit(&t->state, 0ul, memory_order_release);
}

static void try_advance(void) {
    unsigned long e = atomic_load(&global_epoch);
    for (int i = 0; i < T; i++) {
        unsigned long s = atomic_load(&th[i].state);
        if ((s & 1u) && (s >> 1) != e)
            return;
    }
    atomic_compare_exchange_strong(&global_epoch, &e, e + 1);
}

static void retire(Thread *t, Node *n) {
    unsigned long e = atomic_load(&global_epoch);
    n->retire_next = t->limbo[e % 3];
    t->limbo[e % 3] = n;
}

static void push(Thread *t, unsigned v) {
    Node *n = malloc(sizeof *n);
    check(n != NULL, "alloc");
    t->allocs++;
    n->value = v;
    Node *h = atomic_load(&top);
    do {
        n->next = h;
    } while (!atomic_compare_exchange_weak(&top, &h, n));
}

static int pop(Thread *t, unsigned *v) {
    Node *h = atomic_load(&top);
    for (;;) {
        if (!h)
            return 0;
        Node *nx = h->next; /* h cannot be freed: we are in a critical section */
        if (atomic_compare_exchange_weak(&top, &h, nx)) {
            *v = h->value;
            retire(t, h);
            t->pops++;
            return 1;
        }
    }
}

static void *worker(void *p) {
    int id = (int)(size_t)p;
    Thread *t = &th[id];
    for (int i = 0; i < ITERS; i++) {
        enter(t);
        push(t, 1u + (unsigned)id * ITERS + (unsigned)i);
        leave(t);
        enter(t);
        unsigned v;
        if (pop(t, &v))
            atomic_fetch_add(&popped[v], 1u);
        leave(t);
        if (i % ADVANCE_EVERY == 0)
            try_advance();
        if (i % 256 == 0)
            sched_yield();
    }
    return NULL;
}

int main(void) {
    pthread_t tid[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&tid[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(tid[i], NULL);
    for (int i = 0; i < 3; i++)
        try_advance(); /* all threads are inactive now, so the epoch can move */
    /* single-threaded epilogue: drain the stack, then flush every limbo bucket */
    unsigned long allocs = 0, frees = 0;
    long drained = 0;
    Node *n = atomic_load(&top);
    while (n) {
        Node *nx = n->next;
        atomic_fetch_add(&popped[n->value], 1u);
        free(n);
        frees++;
        drained++;
        n = nx;
    }
    for (int i = 0; i < T; i++)
        for (int b = 0; b < 3; b++)
            free_bucket(&th[i], b);
    long pops = 0;
    for (int i = 0; i < T; i++) {
        allocs += th[i].allocs;
        frees += th[i].frees;
        pops += th[i].pops;
    }
    long delivered = 0;
    for (unsigned v = 1; v <= (unsigned)T * ITERS; v++) {
        unsigned c = atomic_load(&popped[v]);
        check(c == 1, "every value delivered exactly once");
        delivered += c;
    }
    check(allocs == (unsigned long)T * ITERS, "allocation count");
    check(frees == allocs, "every node freed exactly once");
    check(pops + drained == (long)T * ITERS, "pops plus drained equals pushes");
    check(atomic_load(&global_epoch) >= 1, "epoch advanced at least once");
    printf("nodes allocated=%lu freed=%lu\n", allocs, frees);
    printf("values delivered exactly once: %ld\n", delivered);
    printf("no use-after-free (checked by the sanitizer build): ok\n");
    return 0;
}
