/*
 * title: FD_CLOEXEC visibility across exec of /bin/sh
 * topic: io_files
 * covers: FD_CLOEXEC, fork, execl /bin/sh, inherited descriptors, shell redirection to fd, pipes
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



/* write everything, retrying short writes; 0 on success, -1 on error */
static inline int wr_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
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
/*
 * Run `/bin/sh -c script` with data_fd remapped to fd `target` (either inheritable
 * or close-on-exec) and stdout connected to a capture pipe. Returns the shell's
 * output in out (trimmed of the trailing newline).
 */
static void run_sh(const char *script, int target, int src_fd, int cloexec, char *out, size_t cap) {
    int cap_pipe[2];
    CHECK(pipe(cap_pipe) == 0);
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        close(cap_pipe[0]);
        if (dup2(cap_pipe[1], 1) != 1) _exit(120);
        if (cap_pipe[1] != 1) close(cap_pipe[1]);
        if (src_fd >= 0) {
            if (src_fd != target && dup2(src_fd, target) != target) _exit(121);
            int fl = fcntl(target, F_GETFD);
            if (cloexec) fcntl(target, F_SETFD, fl | FD_CLOEXEC);
            else fcntl(target, F_SETFD, fl & ~FD_CLOEXEC);
        }
        execl("/bin/sh", "sh", "-c", script, (char *)NULL);
        _exit(127);
    }
    close(cap_pipe[1]);
    long n = rd_full(cap_pipe[0], out, cap - 1);
    CHECK(n >= 0);
    out[n] = 0;
    while (n > 0 && out[n - 1] == '\n') out[--n] = 0;
    close(cap_pipe[0]);
    CHECK(wait_exit(c) == 0);
}

int main(void) {
    char out[256];
    char script[256];
    int data[2];
    /* pick a target descriptor number that is free in this process */
    int target = 0;
    for (int t = 9; t >= 3; t--) /* sh only guarantees single-digit fd numbers */
        if (fcntl(t, F_GETFD) < 0 && errno == EBADF) { target = t; break; }
    CHECK(target != 0);
    CHECK(pipe(data) == 0);
    CHECK(data[0] != target && data[1] != target);

    /* inherited: the shell can write to the descriptor */
    snprintf(script, sizeof script, "echo hello >&%d; echo done", target);
    run_sh(script, target, data[1], 0, out, sizeof out);
    printf("inheritable: shell output '%s'\n", out);
    CHECK(strcmp(out, "done") == 0);
    char b[32];
    CHECK(read(data[0], b, 6) == 6);
    b[6] = 0;
    printf("data pipe received: %.5s\n", b);
    CHECK(memcmp(b, "hello\n", 6) == 0);

    /*
     * Probe: the shell tries to write a marker to the descriptor. If the descriptor is
     * really the data pipe the marker arrives there. (Some emulation layers leave stray
     * read-only descriptors open in the shell; writing to those fails, so the probe
     * only counts a write that reaches the pipe.)
     */
    snprintf(script, sizeof script,
             "exec 2>/dev/null; if echo MARK >&%d; then echo wrote; else echo failed; fi", target);
    int fl = fcntl(data[0], F_GETFL);
    CHECK(fcntl(data[0], F_SETFL, fl | O_NONBLOCK) == 0);
    static const struct { const char *name; int src; int cloexec; } cases[] = {
        {"cloexec", 1, 1}, {"inheritable", 1, 0}, {"unmapped", 0, 0},
    };
    for (int i = 0; i < 3; i++) {
        run_sh(script, target, cases[i].src ? data[1] : -1, cases[i].cloexec, out, sizeof out);
        errno = 0;
        ssize_t n = read(data[0], b, sizeof b);
        int reached = n == 5 && memcmp(b, "MARK\n", 5) == 0;
        if (!reached) CHECK(n < 0 && errno == EAGAIN);
        printf("%-12s marker reached the pipe: %s\n", cases[i].name, reached ? "yes" : "no");
        CHECK(reached == (i == 1));
        CHECK(strcmp(out, reached ? "wrote" : "failed") == 0 || !reached);
    }

    /* a read-side descriptor handed to the shell: `read` builtin consumes a line from it */
    int lines[2];
    CHECK(pipe(lines) == 0);
    CHECK(wr_all(lines[1], "first line\nsecond line\n", 23) == 0);
    close(lines[1]);
    snprintf(script, sizeof script, "read a <&%d; read b <&%d; echo \"[$b] [$a]\"", target, target);
    run_sh(script, target, lines[0], 0, out, sizeof out);
    printf("shell read builtin: %s\n", out);
    CHECK(strcmp(out, "[second line] [first line]") == 0);

    /* the parent's own descriptors are untouched by what children did */
    CHECK(fcntl(lines[0], F_GETFD) >= 0);
    CHECK(fcntl(target, F_GETFD) < 0);
    puts("parent descriptors unchanged");
    close(lines[0]);
    close(data[0]);
    close(data[1]);
    return 0;
}
