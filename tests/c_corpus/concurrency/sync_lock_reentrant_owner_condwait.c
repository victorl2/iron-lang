/*
 * title: Reentrant lock with owner tracking and full-depth condition wait
 * topic: concurrency
 * covers: recursive lock from scratch, owner identity, hold depth, misuse error codes, wait releasing all levels
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RL_OK = 0, RL_NOT_HELD = 1, RL_NOT_OWNER = 2 };

static const char *rl_name(int rc) {
    switch (rc) {
    case RL_OK: return "OK";
    case RL_NOT_HELD: return "NOT_HELD";
    case RL_NOT_OWNER: return "NOT_OWNER";
    }
    return "?";
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t free_cv;  /* signalled when the lock becomes free */
    pthread_cond_t event_cv; /* condition queue for wait/notify */
    pthread_t owner;
    int held;
    int depth;
    long max_depth;
} RLock;

static RLock rl = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0};

static void rl_lock(void) {
    pthread_t self = pthread_self();
    pthread_mutex_lock(&rl.mu);
    if (rl.held && pthread_equal(rl.owner, self)) {
        rl.depth++;
    } else {
        while (rl.held)
            pthread_cond_wait(&rl.free_cv, &rl.mu);
        rl.held = 1;
        rl.owner = self;
        rl.depth = 1;
    }
    if (rl.depth > rl.max_depth)
        rl.max_depth = rl.depth;
    pthread_mutex_unlock(&rl.mu);
}

static int rl_unlock(void) {
    int rc = RL_OK;
    pthread_mutex_lock(&rl.mu);
    if (!rl.held) {
        rc = RL_NOT_HELD;
    } else if (!pthread_equal(rl.owner, pthread_self())) {
        rc = RL_NOT_OWNER;
    } else if (--rl.depth == 0) {
        rl.held = 0;
        pthread_cond_signal(&rl.free_cv);
    }
    pthread_mutex_unlock(&rl.mu);
    return rc;
}

static int rl_depth(void) {
    int d = 0;
    pthread_mutex_lock(&rl.mu);
    if (rl.held && pthread_equal(rl.owner, pthread_self()))
        d = rl.depth;
    pthread_mutex_unlock(&rl.mu);
    return d;
}

/* Caller must own the lock at any depth; the whole depth is released while waiting and restored after. */
static void rl_wait(void) {
    pthread_mutex_lock(&rl.mu);
    int saved = rl.depth;
    rl.held = 0;
    rl.depth = 0;
    pthread_cond_signal(&rl.free_cv);
    pthread_cond_wait(&rl.event_cv, &rl.mu);
    while (rl.held)
        pthread_cond_wait(&rl.free_cv, &rl.mu);
    rl.held = 1;
    rl.owner = pthread_self();
    rl.depth = saved;
    pthread_mutex_unlock(&rl.mu);
}
static void rl_notify_all(void) {
    pthread_mutex_lock(&rl.mu);
    pthread_cond_broadcast(&rl.event_cv);
    pthread_mutex_unlock(&rl.mu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { T = 4, ROUNDS = 60, TREE_DEPTH = 5 };

static long counter;
static atomic_int in_cs, violations;

/* Each recursion level takes the lock again; only the outermost entry is a real acquisition. */
static long tree_sum(int depth, int label) {
    rl_lock();
    long total = 0;
    if (depth == 0) {
        if (atomic_fetch_add(&in_cs, 1) != 0 && rl_depth() == 0)
            atomic_fetch_add(&violations, 1);
        long v = counter;
        if (label % 3 == 0)
            sched_yield();
        counter = v + 1;
        atomic_fetch_sub(&in_cs, 1);
        total = 1;
    } else {
        total = tree_sum(depth - 1, label * 2) + tree_sum(depth - 1, label * 2 + 1);
    }
    int rc = rl_unlock();
    if (rc != RL_OK)
        atomic_fetch_add(&violations, 1);
    return total;
}

static long leaves_done[T];
static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    for (int r = 0; r < ROUNDS; r++)
        leaves_done[id] += tree_sum(TREE_DEPTH, id + r);
    return NULL;
}

static int ready_flag, value_seen, depth_before, depth_after;
static void level3_wait(int lvl) {
    rl_lock();
    if (lvl < 3) {
        level3_wait(lvl + 1);
    } else {
        depth_before = rl_depth();
        while (!ready_flag)
            rl_wait();
        depth_after = rl_depth();
        value_seen = 42;
    }
    rl_unlock();
}
static void *deep_waiter(void *p) {
    (void)p;
    level3_wait(1);
    return NULL;
}
static void *misuser(void *p) {
    int *rc = p;
    *rc = rl_unlock(); /* holds nothing: whichever state main leaves the lock in decides the code */
    return NULL;
}

int main(void) {
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    long leaves = 0;
    for (int i = 0; i < T; i++)
        leaves += leaves_done[i];
    long per_round = 1L << TREE_DEPTH;
    printf("leaves visited %ld (expected %ld), counter %ld\n", leaves, T * ROUNDS * per_round, counter);
    printf("max nesting depth %ld, violations %d\n", rl.max_depth, atomic_load(&violations));
    check(leaves == T * ROUNDS * per_round && counter == leaves, "counts");
    check(rl.max_depth >= TREE_DEPTH + 1 && atomic_load(&violations) == 0, "depth and exclusion");
    check(rl.held == 0, "released");

    /* Misuse codes. */
    int rc_unheld = rl_unlock();
    rl_lock();
    int rc_foreign = -1;
    pthread_t mt;
    pthread_create(&mt, NULL, misuser, &rc_foreign);
    pthread_join(mt, NULL);
    rl_lock();
    int depth2 = rl_depth();
    rl_unlock();
    int rc_ok = rl_unlock();
    int rc_after = rl_unlock();
    printf("unlock unheld: %s; unlock from other thread: %s; nested depth seen %d; final unlocks: %s then %s\n",
           rl_name(rc_unheld), rl_name(rc_foreign), depth2, rl_name(rc_ok), rl_name(rc_after));
    check(rc_unheld == RL_NOT_HELD && rc_foreign == RL_NOT_OWNER && depth2 == 2 && rc_ok == RL_OK &&
              rc_after == RL_NOT_HELD,
          "misuse codes");

    /* Wait at depth 3: main must be able to take the lock (fully released) and notify. */
    pthread_t dw;
    pthread_create(&dw, NULL, deep_waiter, NULL);
    for (;;) {
        rl_lock();
        int waiting = depth_before == 3 && rl.held == 1; /* we got in, so waiter released everything */
        rl_unlock();
        if (waiting)
            break;
        sched_yield();
    }
    rl_lock();
    ready_flag = 1;
    rl_notify_all();
    rl_unlock();
    pthread_join(dw, NULL);
    printf("waiter depth before wait %d, after wait %d, resumed with value %d\n", depth_before, depth_after,
           value_seen);
    check(depth_before == 3 && depth_after == 3 && value_seen == 42, "depth restore");
    check(rl.held == 0 && rl.depth == 0, "final idle");
    return 0;
}
