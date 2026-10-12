/*
 * title: Process-shared mutex and condvar in anonymous shared memory
 * topic: concurrency
 * covers: mmap MAP_SHARED|MAP_ANONYMOUS, PTHREAD_PROCESS_SHARED mutex and condvar, fork, turn taking across processes
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

enum { NPROC = 4, INCS = 3000, ROUNDS = 6 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    long counter;
    int arrived;
    int turn;
    int nlog;
    int log[NPROC * ROUNDS];
} Shared;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void child(Shared *sh, int k) {
    /* phase 1: hammer a counter under the shared mutex */
    for (int i = 0; i < INCS; i++) {
        pthread_mutex_lock(&sh->mu);
        long v = sh->counter;
        sh->counter = v + 1;
        pthread_mutex_unlock(&sh->mu);
    }
    /* phase 2: rendezvous, everybody waits until all processes arrived */
    pthread_mutex_lock(&sh->mu);
    sh->arrived++;
    pthread_cond_broadcast(&sh->cv);
    while (sh->arrived < NPROC)
        pthread_cond_wait(&sh->cv, &sh->mu);
    pthread_mutex_unlock(&sh->mu);
    /* phase 3: strict round robin, process k only acts when it is its turn */
    for (int r = 0; r < ROUNDS; r++) {
        pthread_mutex_lock(&sh->mu);
        while (sh->turn != k)
            pthread_cond_wait(&sh->cv, &sh->mu);
        sh->log[sh->nlog++] = k * 100 + r;
        sh->turn = (sh->turn + 1) % NPROC;
        pthread_cond_broadcast(&sh->cv);
        pthread_mutex_unlock(&sh->mu);
    }
}

int main(void) {
    Shared *sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    check(sh != MAP_FAILED, "mmap");
    memset(sh, 0, sizeof *sh);
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;
    check(pthread_mutexattr_init(&ma) == 0 && pthread_condattr_init(&ca) == 0, "attr init");
    check(pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED) == 0, "mutex pshared");
    check(pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED) == 0, "cond pshared");
    check(pthread_mutex_init(&sh->mu, &ma) == 0, "mutex init");
    check(pthread_cond_init(&sh->cv, &ca) == 0, "cond init");
    pthread_mutexattr_destroy(&ma);
    pthread_condattr_destroy(&ca);

    pid_t pids[NPROC];
    fflush(stdout);
    for (int k = 0; k < NPROC; k++) {
        pids[k] = fork();
        check(pids[k] >= 0, "fork");
        if (pids[k] == 0) {
            child(sh, k);
            _exit(10 + k);
        }
    }
    for (int k = 0; k < NPROC; k++) {
        int st = 0;
        check(waitpid(pids[k], &st, 0) == pids[k], "waitpid");
        check(WIFEXITED(st), "normal exit");
        printf("process %d exit status %d\n", k, WEXITSTATUS(st));
        check(WEXITSTATUS(st) == 10 + k, "status");
    }
    printf("shared counter = %ld (expected %d)\n", sh->counter, NPROC * INCS);
    check(sh->counter == (long)NPROC * INCS, "counter");
    check(sh->nlog == NPROC * ROUNDS, "log length");
    printf("round robin log:");
    for (int i = 0; i < sh->nlog; i++) {
        printf(" %d", sh->log[i]);
        check(sh->log[i] == (i % NPROC) * 100 + i / NPROC, "strict order");
    }
    printf("\n");
    pthread_mutex_destroy(&sh->mu);
    pthread_cond_destroy(&sh->cv);
    munmap(sh, sizeof *sh);
    return 0;
}
