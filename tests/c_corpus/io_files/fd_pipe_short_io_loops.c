/*
 * title: Short reads and writes through a pipe
 * topic: io_files
 * covers: read/write loops, partial transfers, pipe, fork, checksum, EOF detection
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
static inline unsigned long long fnv(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}
#define FNV0 14695981039346656037ULL

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
#define TOTAL 200000u

/* child: emit TOTAL pseudo-random bytes in irregular chunk sizes */
static void producer(int wfd, unsigned long long seed) {
    unsigned char chunk[997];
    xs_state = seed;
    unsigned sent = 0;
    while (sent < TOTAL) {
        unsigned want = 1 + rnd_below(sizeof chunk);
        if (want > TOTAL - sent) want = TOTAL - sent;
        for (unsigned i = 0; i < want; i++) chunk[i] = (unsigned char)(xs() >> 24);
        if (wr_all(wfd, chunk, want) != 0) _exit(2);
        sent += want;
    }
    close(wfd);
    _exit(0);
}

/* consume with a fixed buffer size, counting how many read() calls returned less than asked */
static void consume(int rfd, size_t bufsz, unsigned long long *sum, unsigned *reads, unsigned *shorts, unsigned long *total) {
    unsigned char *buf = malloc(bufsz);
    CHECK(buf != NULL);
    *sum = FNV0;
    *reads = *shorts = 0;
    *total = 0;
    for (;;) {
        ssize_t r = read(rfd, buf, bufsz);
        if (r < 0 && errno == EINTR) continue;
        CHECK(r >= 0);
        if (r == 0) break;
        (*reads)++;
        if ((size_t)r < bufsz) (*shorts)++;
        *sum = fnv(*sum, buf, (size_t)r);
        *total += (unsigned long)r;
    }
    free(buf);
}

int main(void) {
    /* reference checksum computed locally from the same generator */
    unsigned long long seed = 0x9e3779b97f4a7c15ULL;
    unsigned long long ref;
    {
        int p[2];
        CHECK(pipe(p) == 0);
        pid_t c = fork();
        CHECK(c >= 0);
        if (c == 0) { close(p[0]); producer(p[1], seed); }
        close(p[1]);
        unsigned reads, shorts;
        unsigned long total;
        consume(p[0], 4096, &ref, &reads, &shorts, &total);
        close(p[0]);
        CHECK(wait_exit(c) == 0);
        CHECK(total == TOTAL);
        printf("reference: total=%lu sum=%016llx\n", total, ref);
    }

    static const size_t sizes[] = {1, 7, 64, 511, 4096, 65536};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int p[2];
        CHECK(pipe(p) == 0);
        pid_t c = fork();
        CHECK(c >= 0);
        if (c == 0) { close(p[0]); producer(p[1], seed); }
        close(p[1]);
        unsigned long long sum;
        unsigned reads, shorts;
        unsigned long total;
        consume(p[0], sizes[k], &sum, &reads, &shorts, &total);
        close(p[0]);
        CHECK(wait_exit(c) == 0);
        CHECK(total == TOTAL);
        CHECK(sum == ref);
        /* a 1-byte buffer can never see a short read */
        if (sizes[k] == 1) CHECK(shorts == 0 && reads == TOTAL);
        printf("bufsz=%-6zu total=%lu checksum %s\n", sizes[k], total, sum == ref ? "match" : "MISMATCH");
    }

    /* writing to a pipe whose reader is gone gives EPIPE (SIGPIPE ignored) */
    signal(SIGPIPE, SIG_IGN);
    int p[2];
    CHECK(pipe(p) == 0);
    close(p[0]);
    ssize_t w = write(p[1], "x", 1);
    int e = errno;
    printf("write with no reader: %ld %s\n", (long)w, en(e));
    CHECK(w < 0 && e == EPIPE);
    close(p[1]);

    /* read_full helper returns fewer bytes only at EOF */
    CHECK(pipe(p) == 0);
    CHECK(write(p[1], "abcde", 5) == 5);
    close(p[1]);
    char b[16];
    long n = rd_full(p[0], b, 10);
    printf("rd_full asked 10, got %ld\n", n);
    CHECK(n == 5);
    close(p[0]);
    return 0;
}
