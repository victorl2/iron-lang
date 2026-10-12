/*
 * title: lseek whence matrix and pread offset independence
 * topic: io_files
 * covers: lseek SEEK_SET/SEEK_CUR/SEEK_END, EINVAL on negative, seek past EOF, pread keeps offset
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

static unsigned long long xs_state = 88172645463325252ULL;
static inline unsigned long long xs(void) {
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return xs_state;
}
static inline unsigned rnd_below(unsigned n) {
    unsigned v = (unsigned)(xs() >> 33);
    return v % n;
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
static void show(const char *what, off_t r) {
    if (r < 0) printf("%-28s -> error %s\n", what, en(errno));
    else printf("%-28s -> %ld\n", what, (long)r);
}

int main(void) {
    int fd = open("seek.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    unsigned char data[100];
    for (int i = 0; i < 100; i++) data[i] = (unsigned char)(i * 3 + 1);
    CHECK(wr_all(fd, data, sizeof data) == 0);

    show("tell after write", lseek(fd, 0, SEEK_CUR));
    show("SEEK_SET 10", lseek(fd, 10, SEEK_SET));
    show("SEEK_CUR +5", lseek(fd, 5, SEEK_CUR));
    show("SEEK_CUR -12", lseek(fd, -12, SEEK_CUR));
    show("SEEK_END 0", lseek(fd, 0, SEEK_END));
    show("SEEK_END -1", lseek(fd, -1, SEEK_END));
    show("SEEK_END -100", lseek(fd, -100, SEEK_END));

    errno = 0;
    off_t r = lseek(fd, -101, SEEK_END);
    show("SEEK_END -101", r);
    CHECK(r < 0 && errno == EINVAL);
    errno = 0;
    r = lseek(fd, -1, SEEK_SET);
    show("SEEK_SET -1", r);
    CHECK(r < 0 && errno == EINVAL);
    /* a failed seek leaves the offset alone */
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);
    puts("offset unchanged after failed seeks: 0");

    errno = 0;
    r = lseek(fd, 5, 77);
    show("bad whence", r);
    CHECK(r < 0 && errno == EINVAL);

    /* seeking past EOF is allowed and reads there return 0 */
    show("SEEK_SET 250", lseek(fd, 250, SEEK_SET));
    unsigned char tmp[8];
    ssize_t n = read(fd, tmp, sizeof tmp);
    printf("read past EOF -> %ld, size still %ld\n", (long)n, fsize(fd));
    CHECK(n == 0 && fsize(fd) == 100);

    /* pread/pwrite do not move the offset */
    CHECK(lseek(fd, 33, SEEK_SET) == 33);
    CHECK(pread(fd, tmp, 4, 90) == 4);
    CHECK(memcmp(tmp, data + 90, 4) == 0);
    CHECK(pwrite(fd, "ZZ", 2, 0) == 2);
    show("offset after pread+pwrite", lseek(fd, 0, SEEK_CUR));
    CHECK(lseek(fd, 0, SEEK_CUR) == 33);
    CHECK(pread(fd, tmp, 4, 100) == 0);
    puts("pread at EOF -> 0");
    CHECK(pread(fd, tmp, 4, 98) == 2);
    puts("pread straddling EOF -> 2");
    errno = 0;
    n = pread(fd, tmp, 4, -1);
    printf("pread at -1 -> %s\n", en(errno));
    CHECK(n < 0 && errno == EINVAL);

    /* whole-file position sweep: read 1 byte at random positions via each whence */
    unsigned bad = 0, probes = 0;
    for (int i = 0; i < 300; i++) {
        unsigned pos = rnd_below(100);
        off_t got = 0;
        switch (rnd_below(3)) {
        case 0: got = lseek(fd, (off_t)pos, SEEK_SET); break;
        case 1:
            CHECK(lseek(fd, 50, SEEK_SET) == 50);
            got = lseek(fd, (off_t)pos - 50, SEEK_CUR);
            break;
        default: got = lseek(fd, (off_t)pos - 100, SEEK_END); break;
        }
        CHECK(got == (off_t)pos);
        unsigned char b;
        CHECK(read(fd, &b, 1) == 1);
        unsigned char want = pos < 2 ? (unsigned char)'Z' : data[pos];
        probes++;
        if (b != want) bad++;
    }
    printf("probes=%u mismatches=%u\n", probes, bad);
    CHECK(bad == 0);

    /* lseek on a pipe fails with ESPIPE */
    int p[2];
    CHECK(pipe(p) == 0);
    errno = 0;
    r = lseek(p[0], 0, SEEK_CUR);
    show("lseek on pipe", r);
    CHECK(r < 0 && errno == ESPIPE);
    close(p[0]);
    close(p[1]);
    close(fd);
    unlink("seek.bin");
    return 0;
}
