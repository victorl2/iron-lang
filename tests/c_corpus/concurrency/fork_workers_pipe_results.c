/*
 * title: Fork workers returning results through pipes
 * topic: concurrency
 * covers: fork, pipe, struct over a pipe, partial reads, waitpid exit status, closing unused pipe ends
 * deps: libc, posix
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { WORKERS = 6, RANGE = 20000 };

typedef struct {
    int worker;
    long count_primes;
    long sum_primes;
    unsigned long checksum;
} Result;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int is_prime(long n) {
    if (n < 2)
        return 0;
    for (long d = 2; d * d <= n; d++)
        if (n % d == 0)
            return 0;
    return 1;
}

static Result work(int w) {
    Result r = {w, 0, 0, 5381};
    long lo = (long)w * RANGE, hi = lo + RANGE;
    for (long n = lo; n < hi; n++)
        if (is_prime(n)) {
            r.count_primes++;
            r.sum_primes += n;
            r.checksum = r.checksum * 33u + (unsigned long)n;
        }
    return r;
}

static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, (char *)buf + got, n - got);
        if (k < 0)
            return -1;
        if (k == 0)
            break;
        got += (size_t)k;
    }
    return (int)got;
}

int main(void) {
    int fds[WORKERS][2];
    pid_t pids[WORKERS];
    fflush(stdout);
    for (int w = 0; w < WORKERS; w++) {
        check(pipe(fds[w]) == 0, "pipe");
        pid_t pid = fork();
        check(pid >= 0, "fork");
        if (pid == 0) {
            /* child: close every pipe end that is not its own write end */
            for (int k = 0; k <= w; k++) {
                close(fds[k][0]);
                if (k != w && fds[k][1] >= 0)
                    close(fds[k][1]);
            }
            Result r = work(w);
            if (w == WORKERS - 1) {
                /* the last worker dies after sending only half of its record */
                ssize_t n = write(fds[w][1], &r, sizeof r / 2);
                (void)n;
                close(fds[w][1]);
                _exit(40 + w);
            }
            size_t off = 0;
            while (off < sizeof r) {
                ssize_t n = write(fds[w][1], (char *)&r + off, sizeof r - off);
                if (n < 0)
                    _exit(99);
                off += (size_t)n;
            }
            close(fds[w][1]);
            _exit(w); /* exit status carries the worker number */
        }
        pids[w] = pid;
        close(fds[w][1]);
        fds[w][1] = -1; /* parent's copy is gone: a child must not close a recycled number */
    }
    long total_count = 0, total_sum = 0;
    for (int w = 0; w < WORKERS; w++) {
        Result r;
        memset(&r, 0, sizeof r);
        int got = read_full(fds[w][0], &r, sizeof r);
        close(fds[w][0]);
        int status = 0;
        check(waitpid(pids[w], &status, 0) == pids[w], "waitpid");
        check(WIFEXITED(status), "exited normally");
        if (got == (int)sizeof r) {
            Result expect = work(w);
            check(r.worker == w && r.count_primes == expect.count_primes && r.sum_primes == expect.sum_primes &&
                      r.checksum == expect.checksum,
                  "result matches local computation");
            printf("worker %d: primes in [%ld,%ld) = %ld, sum=%ld, exit=%d\n", w, (long)w * RANGE, (long)(w + 1) * RANGE, r.count_primes,
                   r.sum_primes, WEXITSTATUS(status));
            check(WEXITSTATUS(status) == w, "exit status");
            total_count += r.count_primes;
            total_sum += r.sum_primes;
        } else {
            printf("worker %d: truncated record (%d of %d bytes), exit=%d\n", w, got, (int)sizeof r, WEXITSTATUS(status));
            check(got == (int)(sizeof r / 2), "half a record");
            check(WEXITSTATUS(status) == 40 + w, "exit status of the failing worker");
        }
    }
    printf("total primes=%ld sum=%ld\n", total_count, total_sum);
    return 0;
}
