/*
 * title: Process pipeline chained with pipes and exec
 * topic: concurrency
 * covers: pipeline of forked stages, dup2, exec of /bin/cat and /bin/sh -c, line protocol, closing pipe ends
 * deps: libc, posix
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { STAGES = 5, N = 24 };

static int pp[STAGES + 1][2];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* keep only pipe stage_in (read end) and stage_out (write end) */
static void close_others(int in_idx, int out_idx) {
    for (int k = 0; k <= STAGES; k++) {
        if (k != in_idx)
            close(pp[k][0]);
        if (k != out_idx)
            close(pp[k][1]);
    }
}

static void stage_square(int in, int out) {
    FILE *fi = fdopen(in, "r"), *fo = fdopen(out, "w");
    long v;
    while (fscanf(fi, "%ld", &v) == 1)
        fprintf(fo, "%ld\n", v * v);
    fclose(fo);
    fclose(fi);
    _exit(0);
}

static void stage_filter(int in, int out) {
    FILE *fi = fdopen(in, "r"), *fo = fdopen(out, "w");
    long v;
    while (fscanf(fi, "%ld", &v) == 1)
        if (v % 3 != 0)
            fprintf(fo, "%ld\n", v);
    fclose(fo);
    fclose(fi);
    _exit(0);
}

static void stage_running_sum(int in, int out) {
    FILE *fi = fdopen(in, "r"), *fo = fdopen(out, "w");
    long v, sum = 0;
    while (fscanf(fi, "%ld", &v) == 1) {
        sum += v;
        fprintf(fo, "%ld\n", sum);
    }
    fclose(fo);
    fclose(fi);
    _exit(0);
}

int main(void) {
    for (int k = 0; k <= STAGES; k++)
        check(pipe(pp[k]) == 0, "pipe");
    pid_t pid[STAGES];
    fflush(stdout);
    /* stage 0 squares, stage 1 is /bin/cat, stage 2 drops multiples of 3,
       stage 3 is a sh read/echo loop, stage 4 emits a running sum */
    for (int s = 0; s < STAGES; s++) {
        pid[s] = fork();
        check(pid[s] >= 0, "fork");
        if (pid[s] == 0) {
            int in = pp[s][0], out = pp[s + 1][1];
            close_others(s, s + 1);
            if (s == 0)
                stage_square(in, out);
            else if (s == 1) {
                dup2(in, 0);
                dup2(out, 1);
                close(in);
                close(out);
                execl("/bin/cat", "cat", (char *)NULL);
                _exit(127);
            } else if (s == 2)
                stage_filter(in, out);
            else if (s == 3) {
                dup2(in, 0);
                dup2(out, 1);
                close(in);
                close(out);
                execl("/bin/sh", "sh", "-c", "while read a; do echo \"$a\"; done", (char *)NULL);
                _exit(127);
            } else
                stage_running_sum(in, out);
        }
    }
    /* the parent feeds pipe 0 and reads pipe STAGES */
    close_others(STAGES, 0);
    FILE *fo = fdopen(pp[0][1], "w");
    for (long i = 1; i <= N; i++)
        fprintf(fo, "%ld\n", i);
    fclose(fo);
    FILE *fi = fdopen(pp[STAGES][0], "r");
    long got[N], n = 0, v;
    while (fscanf(fi, "%ld", &v) == 1) {
        check(n < N, "too many outputs");
        got[n++] = v;
    }
    fclose(fi);
    for (int s = 0; s < STAGES; s++) {
        int st = 0;
        check(waitpid(pid[s], &st, 0) == pid[s] && WIFEXITED(st) && WEXITSTATUS(st) == 0, "stage exit");
    }
    long expect[N], m = 0, run = 0;
    for (long i = 1; i <= N; i++)
        if ((i * i) % 3 != 0) {
            run += i * i;
            expect[m++] = run;
        }
    check(n == m, "output count");
    printf("inputs 1..%d, outputs=%ld\n", N, n);
    for (long i = 0; i < n; i++) {
        check(got[i] == expect[i], "running sums");
        printf("%ld%c", got[i], (i % 8 == 7 || i == n - 1) ? '\n' : ' ');
    }
    return 0;
}
