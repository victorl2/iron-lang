/*
 * title: Nonblocking pipe reads and writes with EAGAIN
 * topic: io_files
 * covers: O_NONBLOCK, EAGAIN on empty read and full write, partial nonblocking writes, poll wait, drain and refill, EOF vs EAGAIN
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

static inline unsigned long long fnv(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}
#define FNV0 14695981039346656037ULL

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
static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    CHECK(fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
}

static unsigned char stream_byte(unsigned long i) {
    return (unsigned char)((i * 2654435761UL) >> 11);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int p[2];
    CHECK(pipe(p) == 0);
    set_nonblock(p[0]);
    set_nonblock(p[1]);

    unsigned char buf[4096];
    errno = 0;
    ssize_t n = read(p[0], buf, sizeof buf);
    printf("read from empty pipe: %ld %s\n", (long)n, en(errno));
    CHECK(n < 0 && errno == EAGAIN);

    /* fill the pipe with 100-byte writes until it refuses */
    unsigned long written = 0;
    int refused = 0;
    for (;;) {
        unsigned char chunk[100];
        for (int i = 0; i < 100; i++) chunk[i] = stream_byte(written + (unsigned long)i);
        n = write(p[1], chunk, sizeof chunk);
        if (n < 0) {
            refused = errno == EAGAIN;
            break;
        }
        CHECK(n > 0);
        written += (unsigned long)n;
        if (n < 100) {
            /* a partial write: the remainder must be retried after some draining; stop here */
            refused = 1;
            break;
        }
    }
    printf("write eventually refused with EAGAIN or a short write: %d\n", refused);
    printf("pipe accepted at least PIPE_BUF (512) bytes: %d\n", written >= 512);
    CHECK(refused && written >= 512);

    /* drain in odd-sized pieces, verifying the byte stream */
    unsigned long got = 0;
    unsigned bad = 0, reads = 0;
    unsigned sz = 1;
    for (;;) {
        n = read(p[0], buf, sz);
        if (n < 0) {
            CHECK(errno == EAGAIN);
            break;
        }
        CHECK(n > 0);
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] != stream_byte(got + (unsigned long)i)) bad++;
        got += (unsigned long)n;
        reads++;
        sz = sz * 3 + 1;
        if (sz > sizeof buf) sz = 1;
    }
    printf("drained everything written: %d, corrupt bytes: %u\n", got == written, bad);
    CHECK(got == written && bad == 0 && reads > 1);

    /* writable again after draining */
    CHECK(write(p[1], "again", 5) == 5);
    CHECK(read(p[0], buf, 5) == 5 && memcmp(buf, "again", 5) == 0);
    puts("pipe usable again after drain");
    close(p[0]);
    close(p[1]);

    /* producer/consumer with poll: parent writes 300000 bytes nonblocking, child consumes */
    int data[2], res[2];
    CHECK(pipe(data) == 0 && pipe(res) == 0);
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        close(data[1]); close(res[0]);
        unsigned long long h = FNV0;
        unsigned long total = 0;
        unsigned char cb[777];
        for (;;) {
            ssize_t r = read(data[0], cb, sizeof cb);
            if (r < 0) { if (errno == EINTR) continue; _exit(2); }
            if (r == 0) break;
            h = fnv(h, cb, (size_t)r);
            total += (unsigned long)r;
            if (total % 5000 < 777) usleep(200); /* be slow now and then */
        }
        unsigned long long out[2] = {h, total};
        if (write(res[1], out, sizeof out) != (ssize_t)sizeof out) _exit(3);
        _exit(0);
    }
    close(data[0]); close(res[1]);
    set_nonblock(data[1]);
    enum { TOTAL = 300000 };
    unsigned char *src = malloc(TOTAL);
    CHECK(src != NULL);
    for (unsigned long i = 0; i < TOTAL; i++) src[i] = stream_byte(i + 7);
    unsigned long long want = fnv(FNV0, src, TOTAL);
    size_t off = 0;
    while (off < TOTAL) {
        n = write(data[1], src + off, TOTAL - off > 8192 ? 8192 : TOTAL - off);
        if (n > 0) { off += (size_t)n; continue; }
        CHECK(n < 0 && errno == EAGAIN);
        struct pollfd pf = {data[1], POLLOUT, 0};
        CHECK(poll(&pf, 1, 5000) == 1 && (pf.revents & POLLOUT));
    }
    close(data[1]);
    unsigned long long out[2];
    CHECK(rd_full(res[0], out, sizeof out) == (long)sizeof out);
    CHECK(wait_exit(c) == 0);
    printf("consumer received %llu bytes, checksum %s\n", out[1], out[0] == want ? "match" : "MISMATCH");
    CHECK(out[1] == TOTAL && out[0] == want);
    free(src);
    close(res[0]);
    return 0;
}
