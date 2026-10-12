/*
 * title: Timed lock acquisition on a logical clock
 * topic: concurrency
 * covers: try-lock with timeout, logical time instead of wall time, deadline expiry, scripted tick schedule, timeout accounting
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    long now;      /* logical clock, advanced only by tick_to() */
    int held;
    int waiting;
    int timeouts;  /* waiters that gave up */
    int acquired;
} TimedLock;

static TimedLock tl = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0};

static void tick_to(long t) {
    pthread_mutex_lock(&tl.mu);
    if (t > tl.now)
        tl.now = t;
    pthread_cond_broadcast(&tl.cv);
    pthread_mutex_unlock(&tl.mu);
}
static long now_(void) {
    pthread_mutex_lock(&tl.mu);
    long n = tl.now;
    pthread_mutex_unlock(&tl.mu);
    return n;
}

/* Returns 1 when acquired, 0 when the logical deadline passed first. A free lock always wins over the deadline. */
static int lock_until(long deadline) {
    int ok = 1;
    pthread_mutex_lock(&tl.mu);
    tl.waiting++;
    while (tl.held) {
        if (tl.now >= deadline) {
            ok = 0;
            break;
        }
        pthread_cond_wait(&tl.cv, &tl.mu);
    }
    tl.waiting--;
    if (ok) {
        tl.held = 1;
        tl.acquired++;
    } else {
        tl.timeouts++;
    }
    pthread_cond_broadcast(&tl.cv); /* let the script observe the change */
    pthread_mutex_unlock(&tl.mu);
    return ok;
}
static void lock_(void) { (void)lock_until(1L << 40); }
static void unlock_(void) {
    pthread_mutex_lock(&tl.mu);
    tl.held = 0;
    pthread_cond_broadcast(&tl.cv);
    pthread_mutex_unlock(&tl.mu);
}

static void wait_state(int waiting, int timeouts) {
    for (;;) {
        pthread_mutex_lock(&tl.mu);
        int ok = tl.waiting == waiting && tl.timeouts == timeouts;
        pthread_mutex_unlock(&tl.mu);
        if (ok)
            return;
        sched_yield();
    }
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

typedef struct {
    long deadline;
    int result;
    long finished_at;
    int hold_until_release;
} Job;

static atomic_int holder_may_release;

static void *timed_waiter(void *p) {
    Job *j = p;
    j->result = lock_until(j->deadline);
    j->finished_at = now_();
    if (j->result) {
        while (!atomic_load(&holder_may_release))
            sched_yield();
        unlock_();
    }
    return NULL;
}

/* ---- stress ---- */
enum { T = 4, TRIES = 300 };
static atomic_int in_cs, violations, stop_ticker;
static long counter;
static int wins[T], losses[T];

static void *ticker(void *p) {
    (void)p;
    long t = 0;
    while (!atomic_load(&stop_ticker)) {
        tick_to(++t);
        sched_yield();
    }
    return NULL;
}
static void *stress(void *p) {
    int id = (int)(intptr_t)p;
    unsigned s = 77u + (unsigned)id * 131u;
    for (int i = 0; i < TRIES; i++) {
        s = s * 1103515245u + 12345u;
        long dl = now_() + (long)((s >> 16) % 6);
        if (lock_until(dl)) {
            if (atomic_fetch_add(&in_cs, 1) != 0)
                atomic_fetch_add(&violations, 1);
            counter++;
            if (i % 3 == 0)
                sched_yield();
            atomic_fetch_sub(&in_cs, 1);
            wins[id]++;
            unlock_();
        } else {
            losses[id]++;
        }
    }
    return NULL;
}

int main(void) {
    /* Scripted schedule: main holds the lock; deadlines 5, 10, 15 and 20 wait on it. */
    lock_();
    Job jobs[5] = {{5, -1, 0, 0}, {10, -1, 0, 0}, {15, -1, 0, 0}, {20, -1, 0, 0}, {30, -1, 0, 0}};
    pthread_t th[5];
    for (int i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, timed_waiter, &jobs[i]);
    wait_state(4, 0);
    tick_to(5);
    wait_state(3, 1);
    tick_to(10);
    wait_state(2, 2);
    tick_to(15);
    wait_state(1, 3); /* only deadline 20 is still alive */
    unlock_(); /* only the deadline-20 waiter is queued, so it takes the lock */
    for (;;) {
        pthread_mutex_lock(&tl.mu);
        int got = tl.acquired;
        pthread_mutex_unlock(&tl.mu);
        if (got == 2) /* main's own lock plus the deadline-20 waiter */
            break;
        sched_yield();
    }
    pthread_create(&th[4], NULL, timed_waiter, &jobs[4]); /* joins while the lock is held */
    wait_state(1, 3);
    tick_to(30);
    wait_state(0, 4);
    atomic_store(&holder_may_release, 1);
    for (int i = 0; i < 5; i++)
        pthread_join(th[i], NULL);
    for (int i = 0; i < 5; i++)
        printf("waiter deadline %2ld: %s (clock at finish >= %ld: %s)\n", jobs[i].deadline,
               jobs[i].result ? "acquired" : "timed out", jobs[i].result ? 15L : jobs[i].deadline,
               jobs[i].finished_at >= (jobs[i].result ? 15L : jobs[i].deadline) ? "yes" : "no");
    check(!jobs[0].result && !jobs[1].result && !jobs[2].result && jobs[3].result && !jobs[4].result, "script");
    check(tl.timeouts == 4 && tl.acquired == 2 && !tl.held, "script accounting");

    /* An already expired deadline: fails while held, still succeeds when the lock is free. */
    long past = now_() - 1;
    int free_ok = lock_until(past);
    int held_fail = lock_until(past);
    unlock_();
    printf("expired deadline: free lock -> %d, held lock -> %d\n", free_ok, held_fail);
    check(free_ok == 1 && held_fail == 0, "expired deadline");

    /* Stress with a free-running logical clock. */
    pthread_t tk, st[T];
    pthread_create(&tk, NULL, ticker, NULL);
    for (int i = 0; i < T; i++)
        pthread_create(&st[i], NULL, stress, (void *)(intptr_t)i);
    for (int i = 0; i < T; i++)
        pthread_join(st[i], NULL);
    atomic_store(&stop_ticker, 1);
    pthread_join(tk, NULL);
    long w = 0, l = 0;
    for (int i = 0; i < T; i++) {
        w += wins[i];
        l += losses[i];
    }
    printf("stress: attempts %d, wins+timeouts %ld, counter equals wins: %s, violations %d\n", T * TRIES, w + l,
           counter == w ? "yes" : "no", atomic_load(&violations));
    check(w + l == T * TRIES && counter == w && atomic_load(&violations) == 0 && w > 0, "stress");
    check(!tl.held && tl.waiting == 0, "idle");
    return 0;
}
