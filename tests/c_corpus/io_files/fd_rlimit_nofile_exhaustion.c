/*
 * title: Descriptor exhaustion with a lowered RLIMIT_NOFILE
 * topic: io_files
 * covers: getrlimit, setrlimit, RLIMIT_NOFILE, EMFILE from open/dup/pipe, lowest-free-fd allocation, recovery after close
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);          \
            exit(1);                                                      \
        }                                                                 \
    } while (0)



/* read until n bytes or EOF; returns count or -1 */
static inline long rd_full(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (long)got;
}
static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
#define LIMIT 48

typedef struct {
    int pre_open;       /* descriptors below LIMIT already open in the child */
    int opened;         /* successful opens until failure */
    int total_open;     /* pre_open + opened */
    int open_err_emfile;
    int pipe_err_emfile;
    int dup_err_emfile;
    int dup2_high_err_ebadf;
    int reuse_lowest;   /* a closed slot is handed out again */
    int recovered;      /* after closing 4 descriptors exactly 4 more opens succeed */
    int limit_read_back;
} Report;

static void child(int wfd) {
    Report r;
    memset(&r, 0, sizeof r);
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(2);
    rl.rlim_cur = LIMIT;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(3);
    struct rlimit back;
    if (getrlimit(RLIMIT_NOFILE, &back) != 0) _exit(4);
    r.limit_read_back = back.rlim_cur == LIMIT;

    for (int fd = 0; fd < LIMIT; fd++)
        if (fcntl(fd, F_GETFD) >= 0) r.pre_open++;

    int fds[LIMIT + 8];
    int n = 0;
    for (;;) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            r.open_err_emfile = errno == EMFILE;
            break;
        }
        if (n >= LIMIT) _exit(5);
        fds[n++] = fd;
    }
    r.opened = n;
    r.total_open = r.pre_open + n;

    int p[2];
    r.pipe_err_emfile = pipe(p) < 0 && errno == EMFILE;
    r.dup_err_emfile = dup(0) < 0 && errno == EMFILE;
    r.dup2_high_err_ebadf = dup2(0, LIMIT + 1) < 0 && errno == EBADF;

    /* free a few in the middle, the lowest freed number must come back first */
    int lo = fds[3], hi = fds[10];
    close(fds[3]); close(fds[10]); close(fds[7]); close(fds[5]);
    int again = open("/dev/null", O_RDONLY);
    r.reuse_lowest = again == lo;
    (void)hi;
    int extra = 0;
    for (;;) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        extra++;
    }
    r.recovered = extra == 3;

    if (write(wfd, &r, sizeof r) != (ssize_t)sizeof r) _exit(6);
    _exit(0);
}

int main(void) {
    struct rlimit before;
    CHECK(getrlimit(RLIMIT_NOFILE, &before) == 0);
    CHECK(before.rlim_cur >= 64 || before.rlim_cur == RLIM_INFINITY);

    int p[2];
    CHECK(pipe(p) == 0);
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        close(p[0]);
        child(p[1]);
    }
    close(p[1]);
    Report r;
    CHECK(rd_full(p[0], &r, sizeof r) == (long)sizeof r);
    close(p[0]);
    CHECK(wait_exit(c) == 0);

    printf("limit read back as lowered value: %d\n", r.limit_read_back);
    printf("descriptors open in total when open() failed: %d (limit %d)\n", r.total_open, LIMIT);
    printf("open() failure was EMFILE: %d\n", r.open_err_emfile);
    printf("pipe() failure was EMFILE: %d\n", r.pipe_err_emfile);
    printf("dup() failure was EMFILE: %d\n", r.dup_err_emfile);
    printf("dup2 above the limit failed with EBADF: %d\n", r.dup2_high_err_ebadf);
    printf("lowest closed number handed out first: %d\n", r.reuse_lowest);
    printf("three more opens after freeing four: %d\n", r.recovered);
    CHECK(r.limit_read_back && r.open_err_emfile && r.pipe_err_emfile && r.dup_err_emfile);
    CHECK(r.dup2_high_err_ebadf && r.reuse_lowest && r.recovered);
    CHECK(r.total_open == LIMIT);
    CHECK(r.pre_open >= 3 && r.pre_open < LIMIT);

    /* the parent's limit is untouched */
    struct rlimit after;
    CHECK(getrlimit(RLIMIT_NOFILE, &after) == 0);
    CHECK(after.rlim_cur == before.rlim_cur);
    puts("parent limit unchanged");

    /* and the parent can still open descriptors freely */
    int fds[80];
    int n = 0;
    for (; n < 80; n++) {
        fds[n] = open("/dev/null", O_RDONLY);
        if (fds[n] < 0) break;
    }
    printf("parent opened 80 descriptors: %d\n", n == 80);
    CHECK(n == 80);
    for (int i = 0; i < n; i++) close(fds[i]);
    return 0;
}
