/*
 * title: dup2 redirection of stdout and restore
 * topic: io_files
 * covers: dup2, dup, redirecting fd 1, stdio flush ordering, dup2 onto itself, dup2 target replacement
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

static inline const char *en(int e) {
    switch (e) {
    case 0: return "OK";
    case EEXIST: return "EEXIST";
    case ENOENT: return "ENOENT";
    case EBADF: return "EBADF";
    case EINVAL: return "EINVAL";
    case EISDIR: return "EISDIR";
    case ENOTDIR: return "ENOTDIR";
    case EAGAIN: return "EAGAIN";
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case EMFILE: return "EMFILE";
    case ESPIPE: return "ESPIPE";
    case EPIPE: return "EPIPE";
    case EINTR: return "EINTR";
    case EFBIG: return "EFBIG";
    case ENXIO: return "ENXIO";
    case ELOOP: return "ELOOP";
    case ENOTEMPTY: return "ENOTEMPTY";
    case EXDEV: return "EXDEV";
    case ECHILD: return "ECHILD";
    case ESRCH: return "ESRCH";
    default: return "EOTHER";
    }
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
static void dump(const char *name) {
    char b[256];
    int fd = open(name, O_RDONLY);
    CHECK(fd >= 0);
    long n = rd_full(fd, b, sizeof b - 1);
    CHECK(n >= 0);
    b[n] = 0;
    close(fd);
    printf("%s (%ld bytes):", name, n);
    for (long i = 0; i < n; i++) {
        if (b[i] == '\n') fputs(" |", stdout);
        else putchar(b[i]);
    }
    putchar('\n');
}

int main(void) {
    fflush(stdout);
    int saved = dup(1);
    CHECK(saved >= 0);

    /* capture stdio output into a file by pointing fd 1 at it */
    int f = open("cap1.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(f >= 0);
    CHECK(dup2(f, 1) == 1);
    close(f);
    printf("captured line %d\n", 1);
    puts("captured line 2");
    fflush(stdout);
    /* raw write through fd 1 lands in the same file */
    CHECK(write(1, "raw write\n", 10) == 10);
    CHECK(dup2(saved, 1) == 1);
    dump("cap1.txt");

    /* dup2(fd, fd) is a successful no-op */
    int t = open("cap2.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(t >= 0);
    CHECK(dup2(t, t) == t);
    CHECK(fcntl(t, F_GETFL) >= 0);
    puts("dup2(fd, fd) returns fd and leaves it open");

    /* dup2 onto an open descriptor silently closes the old target first */
    int victim = open("cap3.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(victim >= 0);
    CHECK(write(victim, "victim-", 7) == 7);
    CHECK(dup2(t, victim) == victim);
    CHECK(write(victim, "new-target", 10) == 10);
    close(victim);
    close(t);
    dump("cap2.txt");
    dump("cap3.txt");

    /* dup2 to a descriptor number well above any in use */
    int hi = 0;
    for (int cand = 40; cand < 60; cand++)
        if (fcntl(cand, F_GETFD) < 0 && errno == EBADF) { hi = cand; break; }
    CHECK(hi != 0);
    int g = open("cap4.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(g >= 0);
    CHECK(dup2(g, hi) == hi);
    close(g);
    CHECK(write(hi, "via high fd", 11) == 11);
    close(hi);
    dump("cap4.txt");

    /* invalid sources and closed targets */
    errno = 0;
    int r = dup2(hi, 1);
    printf("dup2 from closed fd: %d %s\n", r, en(errno));
    CHECK(r < 0 && errno == EBADF);
    errno = 0;
    r = dup(hi);
    printf("dup of closed fd: %d %s\n", r, en(errno));
    CHECK(r < 0 && errno == EBADF);

    /* redirect stdout for a child: /bin/echo writes into a file, parent output unaffected */
    fflush(stdout);
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        int o = open("cap5.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (o < 0 || dup2(o, 1) != 1) _exit(2);
        close(o);
        execl("/bin/echo", "echo", "from", "child", (char *)NULL);
        _exit(127);
    }
    CHECK(wait_exit(c) == 0);
    dump("cap5.txt");

    close(saved);
    unlink("cap1.txt"); unlink("cap2.txt"); unlink("cap3.txt");
    unlink("cap4.txt"); unlink("cap5.txt");
    return 0;
}
