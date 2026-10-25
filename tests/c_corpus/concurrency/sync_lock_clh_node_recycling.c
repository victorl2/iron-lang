/*
 * title: CLH queue lock with node recycling
 * topic: concurrency
 * covers: CLH lock, implicit queue, predecessor spinning, node reuse after release, multi-word invariant
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct CNode {
    atomic_int locked;
} CNode;

typedef struct {
    _Atomic(CNode *) tail;
} CLH;

typedef struct {
    CNode *node; /* our node for the next acquisition */
    CNode *pred; /* predecessor node from the last acquisition, recycled on release */
} Handle;

static CLH clh;

static void clh_init(CLH *l) {
    CNode *dummy = malloc(sizeof *dummy);
    atomic_store(&dummy->locked, 0);
    atomic_store(&l->tail, dummy);
}

static void clh_lock(CLH *l, Handle *h) {
    atomic_store_explicit(&h->node->locked, 1, memory_order_relaxed);
    h->pred = atomic_exchange_explicit(&l->tail, h->node, memory_order_acq_rel);
    while (atomic_load_explicit(&h->pred->locked, memory_order_acquire))
        sched_yield();
}

static void clh_unlock(Handle *h) {
    CNode *mine = h->node;
    atomic_store_explicit(&mine->locked, 0, memory_order_release);
    h->node = h->pred; /* the predecessor's node is free for us to reuse; ours now belongs to our successor */
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { T = 4, PER = 300, WORDS = 16 };

static int words[WORDS];
static atomic_int in_cs, violations, torn;
static int rounds_by[T];

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    Handle h;
    h.node = malloc(sizeof *h.node);
    atomic_store(&h.node->locked, 0);
    h.pred = NULL;
    for (int i = 0; i < PER; i++) {
        clh_lock(&clh, &h);
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        int first = words[0];
        for (int k = 0; k < WORDS; k++) {
            if (words[k] != first)
                atomic_fetch_add(&torn, 1);
        }
        for (int k = 0; k < WORDS; k++) {
            words[k] = first + 1;
            if (k == WORDS / 2 && i % 4 == 0)
                sched_yield();
        }
        rounds_by[id]++;
        atomic_fetch_sub(&in_cs, 1);
        clh_unlock(&h);
    }
    free(h.node);
    return NULL;
}

int main(void) {
    clh_init(&clh);
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    int sum = 0;
    for (int i = 0; i < T; i++)
        sum += rounds_by[i];
    printf("threads=%d words=%d acquisitions=%d\n", T, WORDS, sum);
    printf("words[0]=%d words[15]=%d\n", words[0], words[WORDS - 1]);
    printf("torn views %d, violations %d\n", atomic_load(&torn), atomic_load(&violations));
    check(sum == T * PER && words[0] == sum, "count");
    for (int k = 1; k < WORDS; k++)
        check(words[k] == words[0], "words equal");
    check(atomic_load(&torn) == 0 && atomic_load(&violations) == 0, "exclusion");
    CNode *last = atomic_load(&clh.tail);
    check(atomic_load(&last->locked) == 0, "tail released");
    free(last);
    printf("tail node released and freed\n");
    return 0;
}
