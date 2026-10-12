/*
 * title: poll readiness on pipes, files and bad descriptors
 * topic: io_files
 * covers: poll, POLLIN, POLLOUT, POLLHUP, POLLNVAL, timeouts, multiplexing several pipes fed by children
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



static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
static void flags(char *out, size_t cap, short ev) {
    out[0] = 0;
    if (ev & POLLIN) strncat(out, "IN ", cap - strlen(out) - 1);
    if (ev & POLLOUT) strncat(out, "OUT ", cap - strlen(out) - 1);
    if (ev & POLLHUP) strncat(out, "HUP ", cap - strlen(out) - 1);
    if (ev & POLLNVAL) strncat(out, "NVAL ", cap - strlen(out) - 1);
    if (!out[0]) strncat(out, "none", cap - 1);
}

static short probe(int fd, short want, int timeout_ms) {
    struct pollfd pf = {fd, want, 0};
    int r = poll(&pf, 1, timeout_ms);
    CHECK(r >= 0);
    return pf.revents;
}

int main(void) {
    char s[64];
    int p[2];
    CHECK(pipe(p) == 0);

    flags(s, sizeof s, probe(p[0], POLLIN, 0));
    printf("empty pipe, read end POLLIN: %s\n", s);
    flags(s, sizeof s, probe(p[1], POLLOUT, 0));
    printf("empty pipe, write end POLLOUT: %s\n", s);

    CHECK(write(p[1], "abc", 3) == 3);
    flags(s, sizeof s, probe(p[0], POLLIN, 0));
    printf("pipe with data, read end: %s\n", s);
    CHECK(probe(p[0], POLLIN, 0) & POLLIN);

    /* consuming makes it quiet again */
    char b[8];
    CHECK(read(p[0], b, 3) == 3);
    short ev = probe(p[0], POLLIN, 10);
    flags(s, sizeof s, ev);
    printf("pipe drained, read end (10ms timeout): %s\n", s);
    CHECK(ev == 0);

    /* writer closes: reader sees HUP once the data is gone; data itself is still delivered first */
    CHECK(write(p[1], "xy", 2) == 2);
    close(p[1]);
    ev = probe(p[0], POLLIN, 0);
    printf("writer closed with data pending: IN=%d\n", (ev & POLLIN) != 0);
    CHECK(ev & POLLIN);
    CHECK(read(p[0], b, 8) == 2);
    ev = probe(p[0], POLLIN, 0);
    printf("writer closed and drained: HUP=%d\n", (ev & POLLHUP) != 0);
    CHECK(ev & POLLHUP);
    CHECK(read(p[0], b, 8) == 0);
    close(p[0]);

    /* bad descriptor */
    ev = probe(p[0], POLLIN, 0);
    flags(s, sizeof s, ev);
    printf("closed descriptor: %s\n", s);
    CHECK(ev & POLLNVAL);

    /* negative descriptors are ignored */
    struct pollfd ign = {-1, POLLIN, 0};
    int r = poll(&ign, 1, 0);
    printf("negative fd ignored: ready=%d revents=%d\n", r, ign.revents);
    CHECK(r == 0 && ign.revents == 0);

    /* regular files are always ready */
    int f = open("ready.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(f >= 0);
    ev = probe(f, POLLIN | POLLOUT, 0);
    flags(s, sizeof s, ev);
    printf("regular file IN|OUT: %s\n", s);
    CHECK((ev & (POLLIN | POLLOUT)) == (POLLIN | POLLOUT));
    close(f);
    unlink("ready.txt");

    /* multiplex four pipes fed by children after different delays */
    enum { N = 4 };
    int pp[N][2];
    pid_t kids[N];
    for (int i = 0; i < N; i++) {
        CHECK(pipe(pp[i]) == 0);
        kids[i] = fork();
        CHECK(kids[i] >= 0);
        if (kids[i] == 0) {
            for (int j = 0; j < N; j++) {
                close(pp[j][0]);
                if (j != i) close(pp[j][1]);
            }
            usleep((unsigned)(1000 * (N - i) * 3));
            for (int k = 0; k <= i; k++) {
                char line[32];
                int len = snprintf(line, sizeof line, "child%d line%d\n", i, k);
                if (write(pp[i][1], line, (size_t)len) != len) _exit(2);
                usleep(500);
            }
            _exit(0);
        }
        close(pp[i][1]);
    }
    int open_count = N;
    int bytes[N] = {0}, lines[N] = {0};
    int done[N] = {0};
    while (open_count > 0) {
        struct pollfd pf[N];
        int idx[N], m = 0;
        for (int i = 0; i < N; i++) {
            if (done[i]) continue;
            pf[m].fd = pp[i][0];
            pf[m].events = POLLIN;
            pf[m].revents = 0;
            idx[m++] = i;
        }
        int pr = poll(pf, (nfds_t)m, 5000);
        CHECK(pr > 0);
        for (int k = 0; k < m; k++) {
            if (!(pf[k].revents & (POLLIN | POLLHUP))) continue;
            char rb[64];
            ssize_t n = read(pf[k].fd, rb, sizeof rb);
            CHECK(n >= 0);
            if (n == 0) {
                done[idx[k]] = 1;
                open_count--;
                close(pf[k].fd);
                continue;
            }
            bytes[idx[k]] += (int)n;
            for (ssize_t q = 0; q < n; q++) lines[idx[k]] += rb[q] == '\n';
        }
    }
    for (int i = 0; i < N; i++) {
        CHECK(wait_exit(kids[i]) == 0);
        printf("pipe %d: lines=%d bytes=%d\n", i, lines[i], bytes[i]);
        CHECK(lines[i] == i + 1 && bytes[i] == (i + 1) * 13);
    }
    return 0;
}
