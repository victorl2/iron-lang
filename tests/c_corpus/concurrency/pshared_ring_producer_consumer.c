/*
 * title: Producer and consumer processes on a shared-memory ring
 * topic: concurrency
 * covers: MAP_SHARED ring buffer, process-shared mutex and two condvars, per-producer ordering, fork
 * deps: libc, posix, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { CAP = 8, PRODUCERS = 3, PER = 400 };

typedef struct {
    int producer, seq;
    long value;
} Item;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t not_full, not_empty;
    Item ring[CAP];
    int head, count;
    long pushed, popped;
    /* results written by the consumer before it exits */
    long sum_by_producer[PRODUCERS];
    long order_errors;
    long wraps;
} Shared;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void producer(Shared *sh, int id) {
    for (int i = 0; i < PER; i++) {
        Item it = {id, i, (long)id * 7919 + (long)i * i % 1013};
        pthread_mutex_lock(&sh->mu);
        while (sh->count == CAP)
            pthread_cond_wait(&sh->not_full, &sh->mu);
        int tail = (sh->head + sh->count) % CAP;
        sh->ring[tail] = it;
        sh->count++;
        sh->pushed++;
        if (tail == CAP - 1)
            sh->wraps++;
        pthread_cond_signal(&sh->not_empty);
        pthread_mutex_unlock(&sh->mu);
    }
}

static void consumer(Shared *sh) {
    int next_seq[PRODUCERS] = {0};
    for (int n = 0; n < PRODUCERS * PER; n++) {
        pthread_mutex_lock(&sh->mu);
        while (sh->count == 0)
            pthread_cond_wait(&sh->not_empty, &sh->mu);
        Item it = sh->ring[sh->head];
        sh->head = (sh->head + 1) % CAP;
        sh->count--;
        sh->popped++;
        pthread_cond_signal(&sh->not_full);
        pthread_mutex_unlock(&sh->mu);
        if (it.seq != next_seq[it.producer])
            sh->order_errors++; /* only the consumer touches these fields until it exits */
        next_seq[it.producer] = it.seq + 1;
        sh->sum_by_producer[it.producer] += it.value;
    }
}

int main(void) {
    Shared *sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    check(sh != MAP_FAILED, "mmap");
    memset(sh, 0, sizeof *sh);
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;
    pthread_mutexattr_init(&ma);
    pthread_condattr_init(&ca);
    check(pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED) == 0, "mutex pshared");
    check(pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED) == 0, "cond pshared");
    check(pthread_mutex_init(&sh->mu, &ma) == 0, "mutex");
    check(pthread_cond_init(&sh->not_full, &ca) == 0, "cond1");
    check(pthread_cond_init(&sh->not_empty, &ca) == 0, "cond2");
    pthread_mutexattr_destroy(&ma);
    pthread_condattr_destroy(&ca);

    fflush(stdout);
    pid_t cpid = fork();
    check(cpid >= 0, "fork consumer");
    if (cpid == 0) {
        consumer(sh);
        _exit(0);
    }
    pid_t ppid[PRODUCERS];
    for (int p = 0; p < PRODUCERS; p++) {
        ppid[p] = fork();
        check(ppid[p] >= 0, "fork producer");
        if (ppid[p] == 0) {
            producer(sh, p);
            _exit(0);
        }
    }
    for (int p = 0; p < PRODUCERS; p++) {
        int st = 0;
        check(waitpid(ppid[p], &st, 0) == ppid[p] && WIFEXITED(st) && WEXITSTATUS(st) == 0, "producer exit");
    }
    int st = 0;
    check(waitpid(cpid, &st, 0) == cpid && WIFEXITED(st) && WEXITSTATUS(st) == 0, "consumer exit");

    long grand = 0;
    for (int p = 0; p < PRODUCERS; p++) {
        long expect = 0;
        for (int i = 0; i < PER; i++)
            expect += (long)p * 7919 + (long)i * i % 1013;
        check(sh->sum_by_producer[p] == expect, "per-producer sum");
        printf("producer %d: %d items, value sum %ld\n", p, PER, sh->sum_by_producer[p]);
        grand += sh->sum_by_producer[p];
    }
    check(sh->order_errors == 0, "per-producer order preserved");
    check(sh->pushed == PRODUCERS * PER && sh->popped == PRODUCERS * PER && sh->count == 0, "counts");
    printf("pushed=%ld popped=%ld grand sum=%ld order errors=%ld\n", sh->pushed, sh->popped, grand, sh->order_errors);
    printf("ring capacity %d, left empty: %s\n", CAP, sh->count == 0 ? "yes" : "no");
    pthread_mutex_destroy(&sh->mu);
    pthread_cond_destroy(&sh->not_full);
    pthread_cond_destroy(&sh->not_empty);
    munmap(sh, sizeof *sh);
    return 0;
}
