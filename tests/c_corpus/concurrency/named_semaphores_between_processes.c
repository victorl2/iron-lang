/*
 * title: Named POSIX semaphores between processes
 * topic: concurrency
 * covers: sem_open, sem_unlink, O_EXCL, binary and counting semaphores, ping-pong handshake, EAGAIN via sem_trywait
 * deps: libc, posix, pthread
 */
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { WORKERS = 4, INCS = 2500, ROUNDS = 8 };

typedef struct {
    long counter;
    int nlog;
    char log[2 * ROUNDS][8];
} Shared;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static const char *errname(int e) {
    switch (e) {
    case EEXIST:
        return "EEXIST";
    case ENOENT:
        return "ENOENT";
    case EAGAIN:
        return "EAGAIN";
    case 0:
        return "none";
    default:
        return "other";
    }
}

static void make_name(char *buf, size_t n, const char *tag) {
    snprintf(buf, n, "/ic%s%ld", tag, (long)getpid());
}

static sem_t *open_new(const char *name, unsigned value) {
    sem_unlink(name); /* stale leftovers from a crashed run */
    sem_t *s = sem_open(name, O_CREAT | O_EXCL, 0600, value);
    check(s != SEM_FAILED, "sem_open create");
    return s;
}

static sem_t *open_existing(const char *name) {
    sem_t *s = sem_open(name, 0);
    check(s != SEM_FAILED, "sem_open existing");
    return s;
}

int main(void) {
    Shared *sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    check(sh != MAP_FAILED, "mmap");
    memset(sh, 0, sizeof *sh);
    char n_mu[32], n_ping[32], n_pong[32], n_cnt[32];
    make_name(n_mu, sizeof n_mu, "m");
    make_name(n_ping, sizeof n_ping, "i");
    make_name(n_pong, sizeof n_pong, "o");
    make_name(n_cnt, sizeof n_cnt, "c");
    sem_t *mu = open_new(n_mu, 1);
    sem_t *ping = open_new(n_ping, 0);
    sem_t *pong = open_new(n_pong, 0);
    sem_t *cnt = open_new(n_cnt, 3);

    /* part 1: mutual exclusion between processes with a binary named semaphore */
    fflush(stdout);
    pid_t pids[WORKERS];
    for (int w = 0; w < WORKERS; w++) {
        pids[w] = fork();
        check(pids[w] >= 0, "fork");
        if (pids[w] == 0) {
            sem_t *m = open_existing(n_mu); /* each child reopens by name */
            for (int i = 0; i < INCS; i++) {
                sem_wait(m);
                long v = sh->counter;
                sh->counter = v + 1;
                sem_post(m);
            }
            sem_close(m);
            _exit(0);
        }
    }
    for (int w = 0; w < WORKERS; w++) {
        int st = 0;
        check(waitpid(pids[w], &st, 0) == pids[w] && WIFEXITED(st) && WEXITSTATUS(st) == 0, "worker exit");
    }
    printf("counter with named mutex: %ld (expected %d)\n", sh->counter, WORKERS * INCS);
    check(sh->counter == (long)WORKERS * INCS, "counter");

    /* part 2: ping-pong handshake between parent and child */
    fflush(stdout);
    pid_t c = fork();
    check(c >= 0, "fork");
    if (c == 0) {
        sem_t *pi = open_existing(n_ping), *po = open_existing(n_pong);
        for (int r = 0; r < ROUNDS; r++) {
            sem_wait(pi);
            snprintf(sh->log[sh->nlog++], sizeof sh->log[0], "ping%d", r);
            sem_post(po);
        }
        sem_close(pi);
        sem_close(po);
        _exit(7);
    }
    for (int r = 0; r < ROUNDS; r++) {
        sem_post(ping);
        sem_wait(pong);
        snprintf(sh->log[sh->nlog++], sizeof sh->log[0], "pong%d", r);
    }
    int st = 0;
    check(waitpid(c, &st, 0) == c && WIFEXITED(st), "child exit");
    printf("handshake child exit=%d log:", WEXITSTATUS(st));
    for (int i = 0; i < sh->nlog; i++)
        printf(" %s", sh->log[i]);
    printf("\n");
    check(sh->nlog == 2 * ROUNDS, "log length");

    /* part 3: counting semaphore with initial value 3 */
    int ok = 0, first_err = 0;
    for (int i = 0; i < 5; i++) {
        if (sem_trywait(cnt) == 0)
            ok++;
        else if (!first_err)
            first_err = errno;
    }
    printf("trywait x5 on value 3: %d succeeded, first failure %s\n", ok, errname(first_err));
    check(ok == 3 && first_err == EAGAIN, "counting semaphore");
    sem_post(cnt);
    sem_post(cnt);
    int again = 0;
    while (sem_trywait(cnt) == 0)
        again++;
    printf("after two posts: %d more succeeded\n", again);
    check(again == 2, "posts");

    /* part 4: naming rules */
    sem_t *dup = sem_open(n_cnt, O_CREAT | O_EXCL, 0600, 1u);
    int e1 = dup == SEM_FAILED ? errno : 0;
    if (dup != SEM_FAILED)
        sem_close(dup);
    sem_t *same = sem_open(n_cnt, 0);
    int reopened = same != SEM_FAILED;
    if (reopened)
        sem_close(same);
    check(sem_unlink(n_cnt) == 0, "unlink");
    sem_t *gone = sem_open(n_cnt, 0);
    int e2 = gone == SEM_FAILED ? errno : 0;
    if (gone != SEM_FAILED)
        sem_close(gone);
    printf("O_EXCL on existing: %s; plain reopen ok: %d; open after unlink: %s\n", errname(e1), reopened, errname(e2));
    check(e1 == EEXIST && reopened && e2 == ENOENT, "naming rules");

    sem_close(mu);
    sem_close(ping);
    sem_close(pong);
    sem_close(cnt);
    sem_unlink(n_mu);
    sem_unlink(n_ping);
    sem_unlink(n_pong);
    munmap(sh, sizeof *sh);
    return 0;
}
