/*
 * title: writev and readv scatter-gather with iovec arrays
 * topic: io_files
 * covers: writev, readv, struct iovec, zero-length entries, partial vector advance, framing over pipes
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

static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
/* writev until everything is out, advancing through the array after short writes */
static int writev_all(int fd, struct iovec *iov, int cnt, int *calls) {
    while (cnt > 0) {
        ssize_t w = writev(fd, iov, cnt);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        (*calls)++;
        size_t left = (size_t)w;
        while (cnt > 0 && left >= iov->iov_len) {
            left -= iov->iov_len;
            iov++;
            cnt--;
        }
        if (cnt > 0 && left > 0) {
            iov->iov_base = (char *)iov->iov_base + left;
            iov->iov_len -= left;
        }
    }
    return 0;
}

static void put_u32(unsigned char *p, unsigned v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static unsigned get_u32(const unsigned char *p) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) v |= (unsigned)p[i] << (8 * i);
    return v;
}

int main(void) {
    int fd = open("gather.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);

    /* one writev: header, empty piece, body, trailer */
    unsigned char hdr[8];
    memcpy(hdr, "FRM1", 4);
    put_u32(hdr + 4, 11);
    struct iovec out[4] = {
        {hdr, 8}, {(void *)"", 0}, {(void *)"hello world", 11}, {(void *)"\n", 1},
    };
    ssize_t w = writev(fd, out, 4);
    printf("writev of 4 pieces wrote %ld bytes\n", (long)w);
    CHECK(w == 20);

    /* scatter the same bytes into differently sized buffers */
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    unsigned char a[4], b[4], c[5], d[32];
    struct iovec in[4] = {{a, 4}, {b, 4}, {c, 5}, {d, sizeof d}};
    ssize_t r = readv(fd, in, 4);
    printf("readv into 4/4/5/32 buffers read %ld bytes\n", (long)r);
    CHECK(r == 20);
    printf("magic=%.4s len=%u first5=%.5s rest=%.6s\n", (char *)a, get_u32(b), (char *)c, (char *)d);
    CHECK(memcmp(a, "FRM1", 4) == 0 && get_u32(b) == 11);
    CHECK(memcmp(c, "hello", 5) == 0 && memcmp(d, " world\n", 7) == 0);
    r = readv(fd, in, 4);
    printf("readv at EOF: %ld\n", (long)r);
    CHECK(r == 0);

    /* framed messages: write many with writev, parse with readv into header+payload */
    CHECK(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0);
    enum { MSGS = 60 };
    unsigned lens[MSGS];
    unsigned long long expect = FNV0;
    long total = 0;
    for (int i = 0; i < MSGS; i++) {
        unsigned n = rnd_below(90);
        unsigned char payload[90], h[4];
        for (unsigned k = 0; k < n; k++) payload[k] = (unsigned char)xs();
        put_u32(h, n);
        struct iovec v[2] = {{h, 4}, {payload, n}};
        CHECK(writev(fd, v, n ? 2 : 1) == (ssize_t)(4 + n));
        lens[i] = n;
        expect = fnv(expect, payload, n);
        total += 4 + n;
    }
    printf("wrote %d frames, %ld bytes\n", MSGS, total);
    CHECK(fsize(fd) == total);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    unsigned long long got = FNV0;
    for (int i = 0; i < MSGS; i++) {
        unsigned char h[4], payload[90];
        struct iovec v[2] = {{h, 4}, {payload, 90}};
        /* read only the header first via a single-entry vector */
        CHECK(readv(fd, v, 1) == 4);
        unsigned n = get_u32(h);
        CHECK(n == lens[i]);
        struct iovec p[1] = {{payload, n}};
        if (n) CHECK(readv(fd, p, 1) == (ssize_t)n);
        got = fnv(got, payload, n);
    }
    printf("payload checksum %s\n", got == expect ? "match" : "MISMATCH");
    CHECK(got == expect);
    close(fd);
    unlink("gather.bin");

    /* writev into a pipe whose capacity is smaller than the vector: reader drains in pieces */
    int p[2];
    CHECK(pipe(p) == 0);
    pid_t kid = fork();
    CHECK(kid >= 0);
    if (kid == 0) {
        close(p[0]);
        static unsigned char big[3][40000];
        struct iovec v[3];
        for (int i = 0; i < 3; i++) {
            memset(big[i], 'a' + i, sizeof big[i]);
            v[i].iov_base = big[i];
            v[i].iov_len = sizeof big[i];
        }
        int calls = 0;
        if (writev_all(p[1], v, 3, &calls) != 0) _exit(2);
        close(p[1]);
        _exit(0);
    }
    close(p[1]);
    long counts[3] = {0, 0, 0};
    unsigned char buf[1000];
    long n, sum = 0;
    while ((n = read(p[0], buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) counts[buf[i] - 'a']++;
        sum += n;
    }
    close(p[0]);
    CHECK(wait_exit(kid) == 0);
    printf("pipe writev_all delivered %ld bytes: a=%ld b=%ld c=%ld\n", sum, counts[0], counts[1], counts[2]);
    CHECK(sum == 120000 && counts[0] == 40000 && counts[1] == 40000 && counts[2] == 40000);
    return 0;
}
