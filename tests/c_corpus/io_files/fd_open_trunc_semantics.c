/*
 * title: O_TRUNC and open modes on existing content
 * topic: io_files
 * covers: O_TRUNC, O_RDWR, O_WRONLY, file offset after open, size tracking
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
static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
static void put(const char *name, const char *s) {
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(wr_all(fd, s, strlen(s)) == 0);
    close(fd);
}

static void slurp(const char *name, char *out, size_t cap) {
    int fd = open(name, O_RDONLY);
    CHECK(fd >= 0);
    long n = rd_full(fd, out, cap - 1);
    CHECK(n >= 0);
    out[n] = 0;
    close(fd);
}

int main(void) {
    char buf[128];
    put("f", "0123456789");
    slurp("f", buf, sizeof buf);
    printf("initial: '%s'\n", buf);

    /* open without O_TRUNC starts at offset 0 and overwrites in place */
    int fd = open("f", O_WRONLY);
    CHECK(fd >= 0);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);
    CHECK(write(fd, "AB", 2) == 2);
    close(fd);
    slurp("f", buf, sizeof buf);
    printf("overwrite head: '%s'\n", buf);
    CHECK(strcmp(buf, "AB23456789") == 0);

    /* O_TRUNC empties the file at open time, before any write */
    fd = open("f", O_WRONLY | O_TRUNC);
    CHECK(fd >= 0);
    printf("size right after O_TRUNC: %ld\n", fsize(fd));
    CHECK(fsize(fd) == 0);
    CHECK(write(fd, "xyz", 3) == 3);
    printf("size after write: %ld\n", fsize(fd));
    close(fd);

    /* O_RDWR|O_TRUNC: read sees EOF, then write, then read back via seek */
    fd = open("f", O_RDWR | O_TRUNC);
    CHECK(fd >= 0);
    CHECK(read(fd, buf, 4) == 0);
    puts("read after truncating open: EOF");
    CHECK(write(fd, "hello world", 11) == 11);
    CHECK(lseek(fd, 6, SEEK_SET) == 6);
    CHECK(read(fd, buf, 5) == 5);
    buf[5] = 0;
    printf("read back at 6: '%s'\n", buf);
    CHECK(strcmp(buf, "world") == 0);
    close(fd);

    /* truncating through one descriptor while another has a large offset */
    fd = open("f", O_RDWR);
    int fd2 = open("f", O_RDWR | O_TRUNC);
    CHECK(fd >= 0 && fd2 >= 0);
    CHECK(lseek(fd, 8, SEEK_SET) == 8);
    printf("size after second open truncates: %ld\n", fsize(fd));
    CHECK(fsize(fd) == 0);
    long n = read(fd, buf, 4);
    printf("read at stale offset 8: %ld\n", n);
    CHECK(n == 0);
    CHECK(write(fd, "Z", 1) == 1);
    printf("size after write at offset 8: %ld\n", fsize(fd));
    CHECK(fsize(fd) == 9);
    CHECK(lseek(fd2, 0, SEEK_SET) == 0);
    unsigned char raw[9];
    CHECK(read(fd2, raw, 9) == 9);
    int zeros = 0;
    for (int i = 0; i < 8; i++) zeros += raw[i] == 0;
    printf("hole bytes zero: %d, last: %c\n", zeros, raw[8]);
    CHECK(zeros == 8 && raw[8] == 'Z');
    close(fd);
    close(fd2);

    /* O_CREAT|O_TRUNC on a nonexistent file creates it empty */
    fd = open("g", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    printf("created empty: %ld\n", fsize(fd));
    close(fd);

    /* many truncate/write rounds: size always equals the last payload */
    long total = 0;
    for (int i = 0; i < 20; i++) {
        unsigned len = rnd_below(200);
        unsigned char data[200];
        for (unsigned j = 0; j < len; j++) data[j] = (unsigned char)xs();
        fd = open("g", O_WRONLY | O_TRUNC);
        CHECK(fd >= 0);
        CHECK(wr_all(fd, data, len) == 0);
        CHECK(fsize(fd) == (long)len);
        close(fd);
        total += len;
    }
    printf("20 rounds ok, bytes written in total: %ld\n", total);
    unlink("f");
    unlink("g");
    return 0;
}
