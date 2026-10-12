/*
 * title: fcntl F_GETFL, F_SETFL and F_GETFD flag behaviour
 * topic: io_files
 * covers: fcntl, O_ACCMODE, O_APPEND, O_NONBLOCK, FD_CLOEXEC, flags shared across dup, EBADF
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
static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
static const char *acc(int fl) {
    switch (fl & O_ACCMODE) {
    case O_RDONLY: return "rdonly";
    case O_WRONLY: return "wronly";
    case O_RDWR: return "rdwr";
    default: return "?";
    }
}

static void describe(const char *label, int fd) {
    int fl = fcntl(fd, F_GETFL);
    int fdfl = fcntl(fd, F_GETFD);
    CHECK(fl >= 0 && fdfl >= 0);
    printf("%-22s acc=%-6s append=%d nonblock=%d cloexec=%d\n", label, acc(fl),
           (fl & O_APPEND) != 0, (fl & O_NONBLOCK) != 0, (fdfl & FD_CLOEXEC) != 0);
}

int main(void) {
    int fd = open("flags.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    describe("fresh O_RDWR", fd);

    int fl = fcntl(fd, F_GETFL);
    CHECK(fcntl(fd, F_SETFL, fl | O_APPEND | O_NONBLOCK) == 0);
    describe("set append+nonblock", fd);

    /* attempting to change the access mode through F_SETFL is ignored */
    CHECK(fcntl(fd, F_SETFL, O_RDONLY | O_APPEND) == 0);
    describe("F_SETFL O_RDONLY", fd);
    CHECK((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR);
    CHECK(wr_all(fd, "ok", 2) == 0);
    puts("write still allowed after attempted access change");

    /* O_CREAT / O_TRUNC are open-time flags and never show up in F_GETFL state */
    CHECK(fcntl(fd, F_SETFL, 0) == 0);
    describe("F_SETFL 0", fd);
    CHECK(fsize(fd) == 2);

    /* status flags live in the open file description, so a dup shares them */
    int d = dup(fd);
    CHECK(d >= 0);
    CHECK(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
    describe("dup after setting on fd", d);
    CHECK(fcntl(d, F_GETFL) & O_NONBLOCK);

    /* descriptor flags are per descriptor: FD_CLOEXEC is not shared and dup clears it */
    CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    describe("fd with cloexec", fd);
    describe("dup (not shared)", d);
    CHECK((fcntl(d, F_GETFD) & FD_CLOEXEC) == 0);
    int d2 = dup(fd);
    CHECK(d2 >= 0);
    describe("dup of cloexec fd", d2);
    CHECK((fcntl(d2, F_GETFD) & FD_CLOEXEC) == 0);
    CHECK(fcntl(fd, F_SETFD, 0) == 0);
    describe("cloexec cleared", fd);

    /* read-only and write-only descriptors report their modes and enforce them */
    int r = open("flags.txt", O_RDONLY);
    int w = open("flags.txt", O_WRONLY | O_APPEND);
    CHECK(r >= 0 && w >= 0);
    describe("O_RDONLY", r);
    describe("O_WRONLY|O_APPEND", w);
    errno = 0;
    ssize_t n = write(r, "x", 1);
    printf("write on rdonly: %ld %s\n", (long)n, en(errno));
    CHECK(n < 0 && errno == EBADF);
    char c;
    errno = 0;
    n = read(w, &c, 1);
    printf("read on wronly: %ld %s\n", (long)n, en(errno));
    CHECK(n < 0 && errno == EBADF);

    /* pipes: nonblock is per end because each end is its own description */
    int p[2];
    CHECK(pipe(p) == 0);
    CHECK(fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK) == 0);
    describe("pipe read end", p[0]);
    describe("pipe write end", p[1]);
    CHECK((fcntl(p[1], F_GETFL) & O_NONBLOCK) == 0);
    CHECK(fcntl(p[0], F_GETFL) & O_NONBLOCK);
    CHECK((fcntl(p[0], F_GETFL) & O_ACCMODE) == O_RDONLY);
    CHECK((fcntl(p[1], F_GETFL) & O_ACCMODE) == O_WRONLY);

    /* errors */
    errno = 0;
    int res = fcntl(p[1] + 100, F_GETFL);
    printf("F_GETFL on unused fd: %d %s\n", res, en(errno));
    CHECK(res < 0 && errno == EBADF);
    errno = 0;
    close(p[0]);
    res = fcntl(p[0], F_GETFD);
    printf("F_GETFD on closed fd: %d %s\n", res, en(errno));
    CHECK(res < 0 && errno == EBADF);

    close(p[1]); close(r); close(w); close(d); close(d2); close(fd);
    unlink("flags.txt");
    return 0;
}
