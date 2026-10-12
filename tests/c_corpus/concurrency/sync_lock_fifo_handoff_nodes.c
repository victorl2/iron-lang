/*
 * title: FIFO lock with per-waiter condvars and direct handoff
 * topic: concurrency
 * covers: fair lock, waiter queue, ownership handoff without barging, trylock refusal, arrival order log
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Waiter {
    pthread_cond_t cv;
    int granted;
    unsigned arrival;
    struct Waiter *next;
} Waiter;

typedef struct {
    pthread_mutex_t mu;
    int held;
    Waiter *head, *tail;
    int queued;
    unsigned arrivals;
} FifoLock;

static FifoLock fl = {PTHREAD_MUTEX_INITIALIZER, 0, NULL, NULL, 0, 0};

/* Returns the arrival number; grants come strictly in arrival order. */
static unsigned fl_lock(void) {
    Waiter me;
    pthread_cond_init(&me.cv, NULL);
    me.granted = 0;
    me.next = NULL;
    pthread_mutex_lock(&fl.mu);
    me.arrival = fl.arrivals++;
    if (!fl.held && fl.head == NULL) {
        fl.held = 1;
    } else {
        if (fl.tail)
            fl.tail->next = &me;
        else
            fl.head = &me;
        fl.tail = &me;
        fl.queued++;
        while (!me.granted)
            pthread_cond_wait(&me.cv, &fl.mu);
    }
    pthread_mutex_unlock(&fl.mu);
    pthread_cond_destroy(&me.cv);
    return me.arrival;
}

static int fl_trylock(unsigned *arrival) {
    int ok = 0;
    pthread_mutex_lock(&fl.mu);
    if (!fl.held && fl.head == NULL) {
        fl.held = 1;
        *arrival = fl.arrivals++;
        ok = 1;
    }
    pthread_mutex_unlock(&fl.mu);
    return ok;
}

static void fl_unlock(void) {
    pthread_mutex_lock(&fl.mu);
    Waiter *w = fl.head;
    if (w) { /* hand the lock over directly: `held` stays 1, so nobody can barge in between */
        fl.head = w->next;
        if (!fl.head)
            fl.tail = NULL;
        fl.queued--;
        w->granted = 1;
        pthread_cond_signal(&w->cv);
    } else {
        fl.held = 0;
    }
    pthread_mutex_unlock(&fl.mu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { T = 5, PER = 120 };

static unsigned grant_log[T * PER + 2048];
static int glen;
static atomic_int in_cs, violations, stop_barging;
static long counter;
static atomic_int barger_wins;

static void record_and_work(unsigned arrival) {
    if (atomic_fetch_add(&in_cs, 1) != 0)
        atomic_fetch_add(&violations, 1);
    grant_log[glen++] = arrival;
    long v = counter;
    if (arrival % 5 == 0)
        sched_yield();
    counter = v + 1;
    atomic_fetch_sub(&in_cs, 1);
}

static void *worker(void *p) {
    (void)p;
    for (int i = 0; i < PER; i++) {
        unsigned a = fl_lock();
        record_and_work(a);
        fl_unlock();
    }
    return NULL;
}

static void *barger(void *p) {
    (void)p;
    while (!atomic_load(&stop_barging) && atomic_load(&barger_wins) < 1000) {
        unsigned a;
        if (fl_trylock(&a)) {
            record_and_work(a);
            atomic_fetch_add(&barger_wins, 1);
            fl_unlock();
        }
        sched_yield();
    }
    return NULL;
}

static unsigned order_seen[4];
static atomic_int order_pos;
static atomic_int trylock_done; /* waiters keep the lock until main has probed it */
static void *ordered(void *p) {
    (void)p;
    unsigned a = fl_lock();
    order_seen[atomic_fetch_add(&order_pos, 1)] = a;
    while (!atomic_load(&trylock_done))
        sched_yield();
    fl_unlock();
    return NULL;
}

static void wait_queued(int n) {
    for (;;) {
        pthread_mutex_lock(&fl.mu);
        int q = fl.queued;
        pthread_mutex_unlock(&fl.mu);
        if (q == n)
            return;
        sched_yield();
    }
}

int main(void) {
    pthread_t th[T], bg;
    pthread_create(&bg, NULL, barger, NULL);
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    atomic_store(&stop_barging, 1);
    pthread_join(bg, NULL);

    int in_order = 1;
    for (int k = 0; k < glen; k++)
        if (grant_log[k] != (unsigned)k)
            in_order = 0;
    printf("worker critical sections %d\n", T * PER);
    printf("grants in arrival order: %s, violations %d\n", in_order ? "yes" : "no", atomic_load(&violations));
    check(in_order && glen == T * PER + atomic_load(&barger_wins), "arrival order");
    check(atomic_load(&violations) == 0 && counter == glen, "exclusion");
    check(fl.queued == 0 && fl.held == 0, "idle");

    /* Handoff demo: three waiters queue behind main; after unlock the lock is still 'held' by the first waiter. */
    unsigned mine = fl_lock();
    (void)mine;
    pthread_t ot[3];
    for (int i = 0; i < 3; i++) {
        pthread_create(&ot[i], NULL, ordered, NULL);
        wait_queued(i + 1); /* enqueue one at a time so arrival order is known */
    }
    fl_unlock();
    unsigned a;
    int got = fl_trylock(&a);
    atomic_store(&trylock_done, 1);
    printf("trylock right after handoff succeeded: %s\n", got ? "yes" : "no");
    for (int i = 0; i < 3; i++)
        pthread_join(ot[i], NULL);
    printf("waiters served in arrival order: %s\n",
           (order_seen[0] + 1 == order_seen[1] && order_seen[1] + 1 == order_seen[2]) ? "yes" : "no");
    check(!got, "barging must fail after handoff");
    check(order_seen[0] + 1 == order_seen[1] && order_seen[1] + 1 == order_seen[2], "handoff order");
    check(fl.held == 0 && fl.queued == 0, "final idle");
    return 0;
}
