/*
 * title: Process pool with dynamic task distribution over pipes
 * topic: concurrency
 * covers: worker processes, task pipes, shared result pipe, poll, atomic small writes, EOF shutdown
 * deps: libc, posix
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { WORKERS = 4, TASKS = 40 };

typedef struct {
    int id;
    unsigned long n;
} Task;

typedef struct {
    int id, worker;
    unsigned long value;
} Answer;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* the "job": length of the Collatz orbit plus a modular power, so tasks cost different amounts */
static unsigned long job(unsigned long n) {
    unsigned long steps = 0, x = n;
    while (x != 1) {
        x = (x % 2) ? 3 * x + 1 : x / 2;
        steps++;
    }
    unsigned long b = n % 1000003u + 2, e = n % 977u + 5, r = 1;
    while (e) {
        if (e & 1)
            r = (r * b) % 1000003u;
        b = (b * b) % 1000003u;
        e >>= 1;
    }
    return steps * 1000003u + r;
}

static int read_exact(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, (char *)buf + got, n - got);
        if (k <= 0)
            return 0;
        got += (size_t)k;
    }
    return 1;
}

static int write_exact(int fd, const void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t k = write(fd, (const char *)buf + off, n - off);
        if (k < 0)
            return 0;
        off += (size_t)k;
    }
    return 1;
}

int main(void) {
    int task_pipe[WORKERS][2], result_pipe[2];
    pid_t pids[WORKERS];
    check(pipe(result_pipe) == 0, "result pipe");
    fflush(stdout);
    for (int w = 0; w < WORKERS; w++) {
        check(pipe(task_pipe[w]) == 0, "task pipe");
        pid_t pid = fork();
        check(pid >= 0, "fork");
        if (pid == 0) {
            close(result_pipe[0]);
            for (int k = 0; k <= w; k++) {
                close(task_pipe[k][1]);
                if (k != w && task_pipe[k][0] >= 0)
                    close(task_pipe[k][0]);
            }
            Task t;
            while (read_exact(task_pipe[w][0], &t, sizeof t)) {
                Answer a = {t.id, w, job(t.n)};
                if (!write_exact(result_pipe[1], &a, sizeof a))
                    _exit(2);
            }
            _exit(0); /* EOF on the task pipe: parent is done */
        }
        pids[w] = pid;
        close(task_pipe[w][0]);
        task_pipe[w][0] = -1; /* parent's copy is gone: never let a child close a recycled number */
    }
    close(result_pipe[1]);

    Task tasks[TASKS];
    unsigned s = 4711u;
    for (int i = 0; i < TASKS; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        tasks[i] = (Task){i, 2 + (unsigned long)(s % 200000u)};
    }
    unsigned long answer[TASKS];
    int have[TASKS] = {0};
    int per_worker[WORKERS] = {0};
    int next = 0, outstanding = 0, finished = 0;
    /* prime every worker with one task, then feed whichever worker answers */
    for (int w = 0; w < WORKERS && next < TASKS; w++) {
        check(write_exact(task_pipe[w][1], &tasks[next++], sizeof(Task)), "send task");
        outstanding++;
    }
    while (finished < TASKS) {
        struct pollfd pfd = {result_pipe[0], POLLIN, 0};
        int pr = poll(&pfd, 1, 20000);
        check(pr == 1, "poll result");
        Answer a;
        check(read_exact(result_pipe[0], &a, sizeof a), "read answer");
        check(a.id >= 0 && a.id < TASKS && !have[a.id], "fresh answer");
        answer[a.id] = a.value;
        have[a.id] = 1;
        per_worker[a.worker]++;
        finished++;
        outstanding--;
        if (next < TASKS) {
            check(write_exact(task_pipe[a.worker][1], &tasks[next++], sizeof(Task)), "send task");
            outstanding++;
        }
    }
    check(outstanding == 0, "nothing outstanding");
    for (int w = 0; w < WORKERS; w++)
        close(task_pipe[w][1]);
    close(result_pipe[0]);
    for (int w = 0; w < WORKERS; w++) {
        int st = 0;
        check(waitpid(pids[w], &st, 0) == pids[w], "waitpid");
        check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "clean worker exit");
    }
    int served = 0;
    for (int w = 0; w < WORKERS; w++)
        served += per_worker[w];
    check(served == TASKS, "all tasks served");
    unsigned long grand = 0;
    for (int i = 0; i < TASKS; i++) {
        check(answer[i] == job(tasks[i].n), "answer matches local computation");
        grand = grand * 31u + answer[i];
        if (i % 4 == 0)
            printf("task %2d: n=%lu steps=%lu modpow=%lu\n", i, tasks[i].n, answer[i] / 1000003u, answer[i] % 1000003u);
    }
    printf("tasks=%d workers=%d answers checksum=%lu\n", TASKS, WORKERS, grand);
    return 0;
}
