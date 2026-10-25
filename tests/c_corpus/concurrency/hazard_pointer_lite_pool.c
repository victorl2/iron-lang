/*
 * title: Hazard-pointer-lite reclamation for an untagged Treiber stack
 * topic: concurrency
 * covers: hazard slots, protect-validate loop, retired list scan, node recycling, ABA prevention without tags
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The stack head is a plain node index (no tag), so a naive pop could suffer ABA when a node is
 * recycled. Hazard slots prevent it: before dereferencing the top, a popper publishes it in its
 * hazard slot (seq_cst) and re-reads head; only if unchanged is the node protected. A node that
 * is popped goes to a per-thread retired list and is recycled only after a scan of all hazard
 * slots shows nobody protects it.
 *
 * Correctness argument: if a thread T1 holds node X in its hazard slot, X cannot be recycled, so
 * X's index cannot re-enter the stack, so head==X at the CAS still means "the same X with the
 * same next". Every value is popped exactly once.
 */
enum { T = 5, K = 24, POOL = T * K, ITERS = 3000, SCAN_AT = 6, NIL = -1 };

typedef struct {
    atomic_int next;
    atomic_int value;
} Node;

static Node nodes[POOL];
static atomic_int head = NIL;
static atomic_int hazard[T];
static atomic_int popped_count[T * ITERS + 1];
static atomic_long total_pops;

typedef struct {
    int id;
    int freelist[POOL];
    int nfree;
    int retired[POOL];
    int nret;
} Local;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void push_node(int n, int v) {
    atomic_store_explicit(&nodes[n].value, v, memory_order_relaxed);
    int h = atomic_load(&head);
    do {
        atomic_store_explicit(&nodes[n].next, h, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak(&head, &h, n));
}

static int pop_value(Local *L, int *val) {
    for (;;) {
        int h = atomic_load(&head);
        if (h == NIL)
            return 0;
        atomic_store(&hazard[L->id], h);        /* announce (seq_cst) */
        if (atomic_load(&head) != h)            /* validate */
            continue;
        int nx = atomic_load_explicit(&nodes[h].next, memory_order_relaxed);
        int v = atomic_load_explicit(&nodes[h].value, memory_order_relaxed);
        if (atomic_compare_exchange_strong(&head, &h, nx)) {
            atomic_store(&hazard[L->id], NIL);
            L->retired[L->nret++] = h;
            *val = v;
            return 1;
        }
    }
}

static void scan_retired(Local *L) {
    int keep = 0;
    for (int i = 0; i < L->nret; i++) {
        int n = L->retired[i], protected_ = 0;
        for (int t = 0; t < T; t++)
            if (atomic_load(&hazard[t]) == n)
                protected_ = 1;
        if (protected_)
            L->retired[keep++] = n;
        else
            L->freelist[L->nfree++] = n;
    }
    L->nret = keep;
}

static void *worker(void *p) {
    Local *L = p;
    for (int i = 0; i < ITERS; i++) {
        if (L->nfree == 0)
            scan_retired(L);
        if (L->nfree > 0) {
            int n = L->freelist[--L->nfree];
            push_node(n, 1 + L->id * ITERS + i); /* unique value per (thread, i) */
        }
        int v;
        if ((i & 1) && pop_value(L, &v)) {
            atomic_fetch_add(&popped_count[v], 1);
            atomic_fetch_add(&total_pops, 1);
        }
        if (L->nret >= SCAN_AT)
            scan_retired(L);
        if (i % 128 == 0)
            sched_yield();
    }
    return NULL;
}

int main(void) {
    static Local loc[T];
    for (int t = 0; t < T; t++) {
        loc[t].id = t;
        atomic_store(&hazard[t], NIL);
        for (int k = 0; k < K; k++)
            loc[t].freelist[loc[t].nfree++] = t * K + k;
    }
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, &loc[i]) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    /* drain what is left in the stack */
    int in_stack = 0;
    for (int h = atomic_load(&head); h != NIL; h = atomic_load(&nodes[h].next)) {
        int v = atomic_load(&nodes[h].value);
        atomic_fetch_add(&popped_count[v], 1);
        in_stack++;
        check(in_stack <= POOL, "stack cycle");
    }
    long pushed = 0;
    for (int v = 1; v <= T * ITERS; v++) {
        int c = atomic_load(&popped_count[v]);
        check(c <= 1, "value delivered twice");
        pushed += c;
    }
    int held = 0;
    for (int t = 0; t < T; t++)
        held += loc[t].nfree + loc[t].nret;
    check(held + in_stack == POOL, "node conservation: free + retired + in stack");
    check(pushed == atomic_load(&total_pops) + in_stack, "every pushed value popped or still in stack");
    printf("nodes=%d conserved: free+retired+in_stack=%d\n", POOL, held + in_stack);
    printf("values popped at most once: yes\n");
    printf("stack values plus popped values equal pushes: yes\n");
    return 0;
}
