/*
 * title: Santa Claus problem with staged arrivals
 * topic: concurrency
 * covers: santa claus, reindeer priority over elves, group of three, monitor, deterministic staging
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { REINDEER = 9, ELVES = 12, GROUP = 3, MAXLOG = 32 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t santa_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t other_cv = PTHREAD_COND_INITIALIZER; /* everyone else, broadcast */

static int paused = 1;
static int quitting;
static int deer_q[REINDEER], deer_n;
static int elf_q[ELVES], elf_n;
static int registered; /* total threads that have checked in, used by main for staging */
static int deer_released[REINDEER], elf_released[ELVES];
static int deliveries;

static char logbuf[MAXLOG][64];
static int logn;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *santa(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!quitting && (paused || (deer_n < REINDEER && elf_n < GROUP)))
            pthread_cond_wait(&santa_cv, &mu);
        if (quitting)
            break;
        if (deer_n == REINDEER) { /* reindeer have priority */
            int len = snprintf(logbuf[logn], sizeof logbuf[logn], "deliver toys with reindeer");
            for (int i = 0; i < deer_n; i++)
                deer_released[deer_q[i]] = 1;
            deer_n = 0;
            deliveries++;
            (void)len;
            logn++;
        } else {
            int len = snprintf(logbuf[logn], sizeof logbuf[logn], "help elves");
            for (int i = 0; i < GROUP; i++) {
                len += snprintf(logbuf[logn] + len, sizeof logbuf[logn] - (size_t)len, " %d", elf_q[i]);
                elf_released[elf_q[i]] = 1;
            }
            for (int i = GROUP; i < elf_n; i++)
                elf_q[i - GROUP] = elf_q[i];
            elf_n -= GROUP;
            logn++;
        }
        pthread_cond_broadcast(&other_cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void *reindeer(void *arg) {
    int id = (int)(long)arg;
    pthread_mutex_lock(&mu);
    deer_q[deer_n++] = id;
    registered++;
    pthread_cond_signal(&santa_cv);
    pthread_cond_broadcast(&other_cv);
    while (!deer_released[id])
        pthread_cond_wait(&other_cv, &mu);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void *elf(void *arg) {
    int id = (int)(long)arg;
    pthread_mutex_lock(&mu);
    elf_q[elf_n++] = id;
    registered++;
    pthread_cond_signal(&santa_cv);
    pthread_cond_broadcast(&other_cv);
    while (!elf_released[id])
        pthread_cond_wait(&other_cv, &mu);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static pthread_t th_deer[REINDEER], th_elf[ELVES];

static void start_thread(void *(*fn)(void *), pthread_t *t, long id) {
    int want;
    pthread_mutex_lock(&mu);
    want = registered + 1;
    pthread_mutex_unlock(&mu);
    check(pthread_create(t, NULL, fn, (void *)id) == 0, "create");
    pthread_mutex_lock(&mu);
    while (registered < want)
        pthread_cond_wait(&other_cv, &mu);
    pthread_mutex_unlock(&mu);
}

static void wait_log(int n) {
    pthread_mutex_lock(&mu);
    while (logn < n)
        pthread_cond_wait(&other_cv, &mu);
    pthread_mutex_unlock(&mu);
}

static void set_paused(int p) {
    pthread_mutex_lock(&mu);
    paused = p;
    pthread_cond_signal(&santa_cv);
    pthread_mutex_unlock(&mu);
}

int main(void) {
    pthread_t st;
    check(pthread_create(&st, NULL, santa, NULL) == 0, "create santa");

    /* stage 1: santa is napping; elves 0..5 queue up, then he helps two groups in order */
    for (long e = 0; e < 6; e++)
        start_thread(elf, &th_elf[e], e);
    set_paused(0);
    wait_log(2);
    for (int e = 0; e < 6; e++)
        pthread_join(th_elf[e], NULL);

    /* stage 2: santa is held while 3 elves and all 9 reindeer show up: reindeer win */
    set_paused(1);
    for (long e = 6; e < 9; e++)
        start_thread(elf, &th_elf[e], e);
    for (long r = 0; r < REINDEER; r++)
        start_thread(reindeer, &th_deer[r], r);
    set_paused(0);
    wait_log(4);
    for (int e = 6; e < 9; e++)
        pthread_join(th_elf[e], NULL);
    for (int r = 0; r < REINDEER; r++)
        pthread_join(th_deer[r], NULL);

    /* stage 3: 2 elves alone are not enough, the third one completes the group */
    for (long e = 9; e < 11; e++)
        start_thread(elf, &th_elf[e], e);
    pthread_mutex_lock(&mu);
    int early = logn;
    pthread_mutex_unlock(&mu);
    check(early == 4, "santa stays asleep for two elves");
    start_thread(elf, &th_elf[11], 11);
    wait_log(5);
    for (int e = 9; e < 12; e++)
        pthread_join(th_elf[e], NULL);

    pthread_mutex_lock(&mu);
    quitting = 1;
    pthread_cond_signal(&santa_cv);
    pthread_mutex_unlock(&mu);
    pthread_join(st, NULL);

    for (int i = 0; i < logn; i++)
        printf("%d: %s\n", i + 1, logbuf[i]);
    check(logn == 5, "log length");
    check(deliveries == 1, "one delivery");
    check(elf_n == 0 && deer_n == 0, "queues drained");
    printf("deliveries=%d elf groups=%d\n", deliveries, logn - deliveries);
    return 0;
}
