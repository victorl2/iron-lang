/*
 * title: Lowest-free descriptor allocation and F_DUPFD
 * topic: io_files
 * covers: lowest available fd rule, F_DUPFD minimum, F_DUPFD_CLOEXEC, closing stdin and reopening as fd 0, gaps, relative numbering
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


/* lowest descriptor number that is currently unused */
static int lowest_free(void) {
    for (int i = 0; i < 1000; i++)
        if (fcntl(i, F_GETFD) < 0 && errno == EBADF) return i;
    return -1;
}

int main(void) {
    int base = lowest_free();
    CHECK(base >= 3);

    /* open() hands out exactly the lowest free number */
    int a = open("/dev/null", O_RDONLY);
    printf("open returns the lowest free number: %d\n", a == base);
    CHECK(a == base);
    int b = open("/dev/null", O_RDONLY);
    CHECK(b > a && b == lowest_free() - 1);

    /* closing a lower one makes it the next candidate */
    close(a);
    int c = open("/dev/null", O_RDONLY);
    printf("closed number reused first: %d\n", c == a);
    CHECK(c == a);
    close(b);
    close(c);
    CHECK(lowest_free() == base);

    /* dup and pipe take lowest numbers too */
    int p[2];
    CHECK(pipe(p) == 0);
    printf("pipe takes two lowest numbers in order: %d\n", p[0] < p[1] && p[0] == base);
    CHECK(p[0] == base && p[1] > p[0]);
    int d = dup(p[1]);
    CHECK(d > p[1]);
    close(p[0]);
    int d2 = dup(d);
    printf("dup fills the hole left by close: %d\n", d2 == p[0]);
    CHECK(d2 == p[0]);
    close(d); close(d2); close(p[1]);

    /* F_DUPFD returns the lowest free number >= the minimum */
    int f = open("/dev/null", O_RDONLY);
    int hi = fcntl(f, F_DUPFD, 100);
    printf("F_DUPFD with minimum 100 lands exactly there: %d\n", hi == 100);
    CHECK(hi == 100);
    int hi2 = fcntl(f, F_DUPFD, 100);
    printf("second F_DUPFD with minimum 100 lands one higher: %d\n", hi2 == 101);
    CHECK(hi2 == 101);
    close(hi);
    int hi3 = fcntl(f, F_DUPFD, 100);
    CHECK(hi3 == 100);
    printf("after closing 100 it is handed out again: %d\n", hi3 == 100);
    int low = fcntl(f, F_DUPFD, 0);
    CHECK(low > f && low < 100);
    printf("F_DUPFD with minimum 0 behaves like dup: %d\n", low == lowest_free() - 1 || low < 100);
    close(hi2); close(hi3); close(low);

    /* F_DUPFD_CLOEXEC sets close-on-exec on the new descriptor only */
    int cx = fcntl(f, F_DUPFD_CLOEXEC, 50);
    CHECK(cx == 50);
    printf("F_DUPFD_CLOEXEC: new fd cloexec=%d, original cloexec=%d\n",
           (fcntl(cx, F_GETFD) & FD_CLOEXEC) != 0, (fcntl(f, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(cx, F_GETFD) & FD_CLOEXEC) && !(fcntl(f, F_GETFD) & FD_CLOEXEC));
    close(cx);

    /* a plain F_DUPFD result has close-on-exec cleared even if the source has it */
    CHECK(fcntl(f, F_SETFD, FD_CLOEXEC) == 0);
    int plain = fcntl(f, F_DUPFD, 60);
    printf("F_DUPFD from a cloexec source: cloexec=%d\n", (fcntl(plain, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(!(fcntl(plain, F_GETFD) & FD_CLOEXEC));
    close(plain);
    close(f);

    /* invalid minimum */
    f = open("/dev/null", O_RDONLY);
    errno = 0;
    int bad = fcntl(f, F_DUPFD, -1);
    printf("F_DUPFD with negative minimum: %d %s\n", bad < 0, en(errno));
    CHECK(bad < 0 && errno == EINVAL);
    close(f);

    /* closing stdin makes fd 0 the next descriptor: save and restore it around the experiment */
    int saved = dup(0);
    CHECK(saved > 2);
    close(0);
    int z = open("/dev/null", O_RDONLY);
    printf("after close(0) the next open returns 0: %d\n", z == 0);
    CHECK(z == 0);
    CHECK(dup2(saved, 0) == 0);
    close(saved);
    CHECK(fcntl(0, F_GETFD) >= 0);
    CHECK(lowest_free() == base);
    puts("descriptor table back to its starting shape");
    return 0;
}
