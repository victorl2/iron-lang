/*
 * title: Fork-join over processes with exit statuses and signals
 * topic: concurrency
 * covers: fork, waitpid, WIFEXITED, WIFSIGNALED, WIFSTOPPED, SIGCONT, WNOHANG, ECHILD, signal names
 * deps: libc, posix
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { NCHILD = 8 };

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static const char *signame(int sig) {
    if (sig == SIGTERM)
        return "SIGTERM";
    if (sig == SIGKILL)
        return "SIGKILL";
    if (sig == SIGUSR1)
        return "SIGUSR1";
    if (sig == SIGUSR2)
        return "SIGUSR2";
    if (sig == SIGSTOP)
        return "SIGSTOP";
    if (sig == SIGHUP)
        return "SIGHUP";
    return "other";
}

/* behaviour of child i, chosen from a small table */
static void child_body(int i) {
    switch (i) {
    case 0:
        _exit(0);
    case 1:
        _exit(3);
    case 2:
        _exit(255);
    case 3:
        signal(SIGTERM, SIG_DFL);
        kill(getpid(), SIGTERM);
        pause();
        _exit(90);
    case 4:
        kill(getpid(), SIGKILL);
        pause();
        _exit(91);
    case 5:
        signal(SIGUSR1, SIG_DFL);
        kill(getpid(), SIGUSR1);
        pause();
        _exit(92);
    case 6:
        kill(getpid(), SIGSTOP); /* wait to be continued, then exit 21 */
        _exit(21);
    default:
        signal(SIGHUP, SIG_DFL);
        kill(getpid(), SIGHUP);
        pause();
        _exit(93);
    }
}

static void describe(int i, int st) {
    if (WIFEXITED(st))
        printf("child %d: exited with status %d\n", i, WEXITSTATUS(st));
    else if (WIFSIGNALED(st))
        printf("child %d: killed by %s\n", i, signame(WTERMSIG(st)));
    else if (WIFSTOPPED(st))
        printf("child %d: stopped by %s\n", i, signame(WSTOPSIG(st)));
}

int main(void) {
    pid_t pids[NCHILD];
    fflush(stdout);
    for (int i = 0; i < NCHILD; i++) {
        pids[i] = fork();
        check(pids[i] >= 0, "fork");
        if (pids[i] == 0)
            child_body(i);
    }
    /* join in child order using waitpid on specific pids; child 6 needs WUNTRACED */
    for (int i = 0; i < NCHILD; i++) {
        int st = 0;
        pid_t r = waitpid(pids[i], &st, i == 6 ? WUNTRACED : 0);
        check(r == pids[i], "waitpid");
        describe(i, st);
        if (i == 6) {
            check(WIFSTOPPED(st), "child 6 stopped");
            check(kill(pids[i], SIGCONT) == 0, "SIGCONT");
            r = waitpid(pids[i], &st, 0);
            check(r == pids[i], "waitpid after continue");
            describe(i, st);
        }
        switch (i) {
        case 0:
            check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child 0");
            break;
        case 1:
            check(WIFEXITED(st) && WEXITSTATUS(st) == 3, "child 1");
            break;
        case 2:
            check(WIFEXITED(st) && WEXITSTATUS(st) == 255, "child 2");
            break;
        case 3:
            check(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "child 3");
            break;
        case 4:
            check(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "child 4");
            break;
        case 5:
            check(WIFSIGNALED(st) && WTERMSIG(st) == SIGUSR1, "child 5");
            break;
        case 6:
            check(WIFEXITED(st) && WEXITSTATUS(st) == 21, "child 6");
            break;
        default:
            check(WIFSIGNALED(st) && WTERMSIG(st) == SIGHUP, "child 7");
            break;
        }
    }
    /* nobody left: waitpid must report ECHILD */
    int st = 0;
    pid_t r = waitpid(-1, &st, WNOHANG);
    printf("waitpid with no children: %s\n", r == -1 && errno == ECHILD ? "ECHILD" : "unexpected");
    check(r == -1 && errno == ECHILD, "ECHILD");

    /* second round: reap in completion order with waitpid(-1), map back to logical ids */
    fflush(stdout);
    pid_t p2[5];
    for (int i = 0; i < 5; i++) {
        p2[i] = fork();
        check(p2[i] >= 0, "fork");
        if (p2[i] == 0)
            _exit(i * i + 1);
    }
    int status_by_child[5];
    for (int i = 0; i < 5; i++)
        status_by_child[i] = -1;
    for (int reaped = 0; reaped < 5; reaped++) {
        pid_t w = wait(&st);
        check(w > 0 && WIFEXITED(st), "wait");
        int idx = -1;
        for (int i = 0; i < 5; i++)
            if (p2[i] == w)
                idx = i;
        check(idx >= 0, "known child");
        status_by_child[idx] = WEXITSTATUS(st);
    }
    printf("statuses by logical id:");
    int sum = 0;
    for (int i = 0; i < 5; i++) {
        printf(" %d", status_by_child[i]);
        sum += status_by_child[i];
    }
    printf(" (sum %d)\n", sum);
    check(sum == 1 + 2 + 5 + 10 + 17, "sum of statuses");
    return 0;
}
