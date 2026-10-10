/*
 * title: Upgradable read lock with check-then-act and downgrade
 * topic: concurrency
 * covers: upgradable rwlock, single upgrader, upgrade without deadlock, downgrade, decision stability
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
    int readers, writer, upgradable, upgrading;
} URW;

static URW rw = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0};

static void rd_lock(void) {
    pthread_mutex_lock(&rw.mu);
    while (rw.writer || rw.upgrading)
        pthread_cond_wait(&rw.cv, &rw.mu);
    rw.readers++;
    pthread_mutex_unlock(&rw.mu);
}
static void rd_unlock(void) {
    pthread_mutex_lock(&rw.mu);
    if (--rw.readers == 0)
        pthread_cond_broadcast(&rw.cv);
    pthread_mutex_unlock(&rw.mu);
}
/* At most one upgradable holder; it coexists with plain readers but excludes writers. */
static void up_lock(void) {
    pthread_mutex_lock(&rw.mu);
    while (rw.writer || rw.upgradable)
        pthread_cond_wait(&rw.cv, &rw.mu);
    rw.upgradable = 1;
    pthread_mutex_unlock(&rw.mu);
}
static void up_unlock(void) {
    pthread_mutex_lock(&rw.mu);
    rw.upgradable = 0;
    pthread_cond_broadcast(&rw.cv);
    pthread_mutex_unlock(&rw.mu);
}
static void up_to_write(void) {
    pthread_mutex_lock(&rw.mu);
    rw.upgrading = 1; /* stop new readers so the existing ones can drain */
    while (rw.readers > 0)
        pthread_cond_wait(&rw.cv, &rw.mu);
    rw.upgrading = 0;
    rw.upgradable = 0;
    rw.writer = 1;
    pthread_mutex_unlock(&rw.mu);
}
static void wr_lock(void) {
    pthread_mutex_lock(&rw.mu);
    while (rw.writer || rw.upgradable || rw.readers > 0)
        pthread_cond_wait(&rw.cv, &rw.mu);
    rw.writer = 1;
    pthread_mutex_unlock(&rw.mu);
}
static void wr_unlock(void) {
    pthread_mutex_lock(&rw.mu);
    rw.writer = 0;
    pthread_cond_broadcast(&rw.cv);
    pthread_mutex_unlock(&rw.mu);
}
static void wr_to_up(void) { /* downgrade: keep exclusion against writers, let readers back in */
    pthread_mutex_lock(&rw.mu);
    rw.writer = 0;
    rw.upgradable = 1;
    pthread_cond_broadcast(&rw.cv);
    pthread_mutex_unlock(&rw.mu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { LIMIT = 300, UPGRADERS = 4, READERS = 3, WRITERS = 2, W_ITERS = 60, ATTEMPTS = 200 };

static int cell;      /* only grows: upgraders add to it while below LIMIT */
static int side;      /* writers bump this together with `mirror` */
static int mirror;
static atomic_int active_w, active_r, bad, decision_changed, upgrades_done;
static int per_thread[UPGRADERS];

static void *upgrader(void *p) {
    int id = (int)(intptr_t)p;
    for (int i = 0; i < ATTEMPTS; i++) {
        up_lock();
        int seen = cell;
        if (seen < LIMIT) {
            sched_yield(); /* others may read but nobody may write while we hold the upgradable lock */
            up_to_write();
            if (atomic_fetch_add(&active_w, 1) != 0 || atomic_load(&active_r) != 0)
                atomic_fetch_add(&bad, 1);
            if (cell != seen)
                atomic_fetch_add(&decision_changed, 1);
            cell = seen + 1;
            per_thread[id]++;
            atomic_fetch_add(&upgrades_done, 1);
            atomic_fetch_sub(&active_w, 1);
            wr_unlock();
        } else {
            up_unlock();
        }
    }
    return NULL;
}

static int monotonic_violations;
static void *reader(void *p) {
    (void)p;
    int last = 0, viol = 0;
    for (int i = 0; i < 400; i++) {
        rd_lock();
        atomic_fetch_add(&active_r, 1);
        if (atomic_load(&active_w) != 0)
            atomic_fetch_add(&bad, 1);
        int c = cell;
        if (c < last)
            viol++;
        last = c;
        if (side != mirror)
            atomic_fetch_add(&bad, 1);
        atomic_fetch_sub(&active_r, 1);
        rd_unlock();
    }
    if (viol)
        atomic_fetch_add(&bad, viol);
    return NULL;
}

static void *plain_writer(void *p) {
    (void)p;
    for (int i = 0; i < W_ITERS; i++) {
        wr_lock();
        if (atomic_fetch_add(&active_w, 1) != 0 || atomic_load(&active_r) != 0)
            atomic_fetch_add(&bad, 1);
        side++;
        sched_yield();
        mirror++;
        atomic_fetch_sub(&active_w, 1);
        wr_unlock();
    }
    return NULL;
}

int main(void) {
    pthread_t th[UPGRADERS + READERS + WRITERS];
    int n = 0;
    for (int i = 0; i < UPGRADERS; i++)
        pthread_create(&th[n++], NULL, upgrader, (void *)(intptr_t)i);
    for (int i = 0; i < READERS; i++)
        pthread_create(&th[n++], NULL, reader, NULL);
    for (int i = 0; i < WRITERS; i++)
        pthread_create(&th[n++], NULL, plain_writer, NULL);
    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);
    (void)monotonic_violations;

    int sum = 0;
    for (int i = 0; i < UPGRADERS; i++)
        sum += per_thread[i];
    printf("cell=%d limit=%d upgrades=%d sum of per-thread=%d\n", cell, LIMIT, atomic_load(&upgrades_done), sum);
    printf("side=%d mirror=%d\n", side, mirror);
    printf("decision changed across upgrade: %d, violations: %d\n", atomic_load(&decision_changed), atomic_load(&bad));
    check(cell == LIMIT && sum == LIMIT, "exactly LIMIT upgrades");
    check(atomic_load(&decision_changed) == 0, "decision stability");
    check(atomic_load(&bad) == 0, "violations");
    check(side == WRITERS * W_ITERS && side == mirror, "writers");

    /* Downgrade path: write -> upgradable admits readers but blocks another upgradable holder. */
    wr_lock();
    cell = 1000;
    wr_to_up();
    rd_lock();
    int seen_by_reader = cell;
    rd_unlock();
    printf("reader sees %d after downgrade\n", seen_by_reader);
    check(seen_by_reader == 1000, "reader after downgrade");
    check(rw.upgradable == 1 && rw.writer == 0, "downgrade state");
    up_unlock();
    check(rw.readers == 0 && !rw.writer && !rw.upgradable && !rw.upgrading, "idle");
    printf("idle state restored\n");
    return 0;
}
