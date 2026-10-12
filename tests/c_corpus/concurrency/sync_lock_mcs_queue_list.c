/*
 * title: MCS queue lock guarding a doubly linked list
 * topic: concurrency
 * covers: MCS lock, per-thread queue nodes on stack, tail exchange, successor handoff, trylock via CAS, list invariants
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct QNode {
    _Atomic(struct QNode *) next;
    atomic_int locked;
} QNode;

typedef struct {
    _Atomic(QNode *) tail;
} MCS;

static MCS mcs;

static void mcs_lock(MCS *l, QNode *me) {
    atomic_store_explicit(&me->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&me->locked, 1, memory_order_relaxed);
    QNode *pred = atomic_exchange_explicit(&l->tail, me, memory_order_acq_rel);
    if (pred != NULL) {
        atomic_store_explicit(&pred->next, me, memory_order_release);
        while (atomic_load_explicit(&me->locked, memory_order_acquire))
            sched_yield();
    }
}

static int mcs_trylock(MCS *l, QNode *me) {
    atomic_store_explicit(&me->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&me->locked, 0, memory_order_relaxed);
    QNode *expected = NULL;
    return atomic_compare_exchange_strong_explicit(&l->tail, &expected, me, memory_order_acq_rel, memory_order_relaxed);
}

static void mcs_unlock(MCS *l, QNode *me) {
    QNode *succ = atomic_load_explicit(&me->next, memory_order_acquire);
    if (succ == NULL) {
        QNode *expected = me;
        if (atomic_compare_exchange_strong_explicit(&l->tail, &expected, NULL, memory_order_acq_rel,
                                                    memory_order_relaxed))
            return; /* nobody queued behind us */
        while ((succ = atomic_load_explicit(&me->next, memory_order_acquire)) == NULL)
            sched_yield(); /* a successor swapped the tail but has not linked yet */
    }
    atomic_store_explicit(&succ->locked, 0, memory_order_release);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

typedef struct Item {
    struct Item *prev, *next;
    int owner, seq;
} Item;

static Item *head, *tail_item;
static int list_len;
static atomic_int in_cs, violations;
static int pushed[8], popped[8];

static void push_front(Item *it) {
    it->prev = NULL;
    it->next = head;
    if (head)
        head->prev = it;
    head = it;
    if (!tail_item)
        tail_item = it;
    list_len++;
}
static Item *pop_back(void) {
    Item *it = tail_item;
    if (!it)
        return NULL;
    tail_item = it->prev;
    if (tail_item)
        tail_item->next = NULL;
    else
        head = NULL;
    list_len--;
    return it;
}

static int walk_ok(void) {
    int n = 0;
    Item *prev = NULL;
    for (Item *p = head; p; p = p->next) {
        if (p->prev != prev)
            return 0;
        prev = p;
        n++;
    }
    return prev == tail_item && n == list_len;
}

enum { T = 4, OPS = 400 };

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    QNode node;
    for (int i = 0; i < OPS; i++) {
        Item *fresh = NULL;
        if ((i + id) % 3 != 2) {
            fresh = malloc(sizeof *fresh);
            fresh->owner = id;
            fresh->seq = i;
        }
        mcs_lock(&mcs, &node);
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        if (fresh) {
            push_front(fresh);
            pushed[id]++;
        } else {
            Item *gone = pop_back();
            if (gone) {
                popped[gone->owner]++;
                free(gone);
            }
        }
        if (i % 7 == 0) {
            sched_yield();
            if (!walk_ok())
                atomic_fetch_add(&violations, 1);
        }
        atomic_fetch_sub(&in_cs, 1);
        mcs_unlock(&mcs, &node);
    }
    return NULL;
}

int main(void) {
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    int total_push = 0, total_pop = 0;
    for (int i = 0; i < T; i++) {
        printf("thread %d pushed %d\n", i, pushed[i]);
        total_push += pushed[i];
    }
    for (int i = 0; i < T; i++)
        total_pop += popped[i];
    printf("pushed %d, pushed == popped + remaining: %s\n", total_push, total_push == total_pop + list_len ? "yes" : "no");
    printf("list links consistent: %s, violations %d\n", walk_ok() ? "yes" : "no", atomic_load(&violations));
    check(total_push - total_pop == list_len, "conservation");
    check(walk_ok() && atomic_load(&violations) == 0, "violations");
    check(atomic_load(&mcs.tail) == NULL, "queue empty");
    while (head) {
        Item *d = pop_back();
        free(d);
    }

    QNode a, b;
    int t1 = mcs_trylock(&mcs, &a);
    int t2 = mcs_trylock(&mcs, &b);
    mcs_unlock(&mcs, &a);
    int t3 = mcs_trylock(&mcs, &b);
    mcs_unlock(&mcs, &b);
    printf("trylock sequence: %d %d %d\n", t1, t2, t3);
    check(t1 == 1 && t2 == 0 && t3 == 1 && atomic_load(&mcs.tail) == NULL, "trylock");
    return 0;
}
