/*
 * title: errno matrix for operations on the wrong kind of descriptor
 * topic: io_files
 * covers: EBADF, EISDIR, ESPIPE, EINVAL, closed and never-opened descriptors, access mode enforcement, double close
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
typedef struct {
    const char *name;
    int fd;
} Subject;

static const char *outcome(long r, int e) {
    return r >= 0 ? "ok" : en(e);
}

int main(void) {
    int rd = open("data", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(rd >= 0 && wr_all(rd, "0123456789", 10) == 0);
    close(rd);
    rd = open("data", O_RDONLY);
    int wr = open("data", O_WRONLY);
    int dir = open(".", O_RDONLY);
    int gone = open("data", O_RDONLY);
    int p[2];
    CHECK(rd >= 0 && wr >= 0 && dir >= 0 && gone >= 0 && pipe(p) == 0);
    close(gone);
    int never = 900; /* far above anything this process opened */
    CHECK(fcntl(never, F_GETFD) < 0);

    Subject subs[] = {
        {"rdonly file", rd}, {"wronly file", wr}, {"directory", dir},
        {"pipe read end", p[0]}, {"pipe write end", p[1]},
        {"closed fd", gone}, {"never opened fd", never}, {"negative fd", -1},
    };
    const int n = (int)(sizeof subs / sizeof subs[0]);

    printf("%-16s %-8s %-8s %-8s %-8s %-8s\n", "descriptor", "read", "write", "lseek", "fstat", "ftrunc");
    for (int i = 0; i < n; i++) {
        char b[4];
        struct stat st;
        /* the pipe read end is empty: use a nonblocking view so read reports EAGAIN, not a hang */
        if (subs[i].fd == p[0]) CHECK(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);
        errno = 0; long r1 = read(subs[i].fd, b, 1); int e1 = errno;
        errno = 0; long r2 = write(subs[i].fd, "x", 0); int e2 = errno;
        errno = 0; long r3 = lseek(subs[i].fd, 0, SEEK_CUR); int e3 = errno;
        errno = 0; long r4 = fstat(subs[i].fd, &st); int e4 = errno;
        errno = 0; long r5 = ftruncate(subs[i].fd, 10); int e5 = errno;
        /* write of zero bytes is unspecified for some descriptors; report only whether EBADF appears */
        printf("%-16s %-8s %-8s %-8s %-8s %-8s\n", subs[i].name, outcome(r1, e1),
               r2 < 0 && e2 == EBADF ? "EBADF" : "not-EBADF", outcome(r3, e3), outcome(r4, e4),
               r5 < 0 ? (e5 == EBADF ? "EBADF" : "EINVAL/other") : "ok");
        CHECK(r5 < 0 || subs[i].fd == wr || subs[i].fd == rd);
    }

    /* real transfers on mismatched descriptors */
    errno = 0;
    long r = write(rd, "abc", 3);
    printf("write to rdonly: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EBADF);
    char b[16];
    errno = 0;
    r = read(wr, b, 4);
    printf("read from wronly: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EBADF);
    errno = 0;
    r = read(dir, b, 4);
    printf("read from directory: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EISDIR);
    errno = 0;
    r = write(dir, "x", 1);
    printf("write to rdonly directory: %s\n", outcome(r, errno));
    CHECK(r < 0);
    errno = 0;
    r = read(p[1], b, 1);
    printf("read from pipe write end: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EBADF);
    errno = 0;
    r = write(p[0], "x", 1);
    printf("write to pipe read end: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EBADF);
    errno = 0;
    r = pread(p[0], b, 1, 0);
    printf("pread on pipe: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == ESPIPE);
    errno = 0;
    r = read(p[0], b, 1);
    printf("read from empty nonblocking pipe: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EAGAIN);

    /* zero-length reads succeed on valid descriptors */
    r = read(rd, b, 0);
    printf("zero-length read on file: %ld\n", r);
    CHECK(r == 0);

    /* negative offsets and bad whence */
    errno = 0;
    r = lseek(rd, -5, SEEK_SET);
    printf("lseek to -5: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EINVAL);
    errno = 0;
    r = pwrite(wr, "x", 1, -1);
    printf("pwrite at -1: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EINVAL);

    /* closing */
    CHECK(close(rd) == 0);
    errno = 0;
    r = close(rd);
    printf("second close: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EBADF);
    errno = 0;
    r = close(-1);
    printf("close(-1): %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EBADF);
    errno = 0;
    r = dup2(wr, -1);
    printf("dup2 onto -1: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EBADF);
    errno = 0;
    r = flock(rd, LOCK_SH);
    printf("flock on closed fd: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EBADF);
    errno = 0;
    r = fsync(rd);
    printf("fsync on closed fd: %s\n", outcome(r, errno));
    CHECK(r < 0 && errno == EBADF);
    close(wr); close(dir); close(p[0]); close(p[1]);
    unlink("data");
    return 0;
}
