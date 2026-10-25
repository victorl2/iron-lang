/*
 * title: O_APPEND writes ignore lseek
 * topic: io_files
 * covers: O_APPEND, lseek, offset after append write, reads through append descriptor, F_SETFL
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
int main(void) {
    char buf[64];
    int fd = open("log", O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0644);
    CHECK(fd >= 0);

    CHECK(write(fd, "one\n", 4) == 4);
    printf("offset after first write: %ld\n", (long)lseek(fd, 0, SEEK_CUR));

    /* seek to the start, then write: data still lands at the end */
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(write(fd, "two\n", 4) == 4);
    long off = (long)lseek(fd, 0, SEEK_CUR);
    printf("offset after write at seek 0: %ld\n", off);
    CHECK(off == 8);

    /* reading uses the offset normally */
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    long n = rd_full(fd, buf, sizeof buf - 1);
    buf[n] = 0;
    printf("content (%ld bytes): %s", n, buf);
    CHECK(strcmp(buf, "one\ntwo\n") == 0);

    /* seeking past the end then appending does not create a hole */
    CHECK(lseek(fd, 1000, SEEK_SET) == 1000);
    CHECK(write(fd, "three\n", 6) == 6);
    printf("size after write with seek 1000: %ld\n", fsize(fd));
    CHECK(fsize(fd) == 14);
    close(fd);

    /* a second descriptor without O_APPEND overwrites in place */
    fd = open("log", O_WRONLY);
    CHECK(fd >= 0);
    CHECK(write(fd, "ONE", 3) == 3);
    close(fd);

    /* switch append mode on and off with F_SETFL */
    fd = open("log", O_WRONLY);
    CHECK(fd >= 0);
    int fl = fcntl(fd, F_GETFL);
    CHECK((fl & O_APPEND) == 0);
    CHECK(fcntl(fd, F_SETFL, fl | O_APPEND) == 0);
    CHECK(fcntl(fd, F_GETFL) & O_APPEND);
    CHECK(write(fd, "four\n", 5) == 5);
    CHECK(fcntl(fd, F_SETFL, fl) == 0);
    CHECK((fcntl(fd, F_GETFL) & O_APPEND) == 0);
    CHECK(lseek(fd, 4, SEEK_SET) == 4);
    CHECK(write(fd, "TWO", 3) == 3);
    close(fd);

    fd = open("log", O_RDONLY);
    n = rd_full(fd, buf, sizeof buf - 1);
    buf[n] = 0;
    close(fd);
    printf("final (%ld bytes): ", n);
    for (long i = 0; i < n; i++) putchar(buf[i] == '\n' ? '|' : buf[i]);
    putchar('\n');
    CHECK(strcmp(buf, "ONE\nTWO\nthree\nfour\n") == 0);

    /* interleaved appenders through two descriptors keep every record */
    int a = open("log2", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    int b = open("log2", O_WRONLY | O_APPEND);
    CHECK(a >= 0 && b >= 0);
    for (int i = 0; i < 10; i++) {
        char rec[8];
        int len = snprintf(rec, sizeof rec, "%c%d;", (i & 1) ? 'b' : 'a', i);
        CHECK(wr_all((i & 1) ? b : a, rec, (size_t)len) == 0);
    }
    close(a);
    close(b);
    fd = open("log2", O_RDONLY);
    n = rd_full(fd, buf, sizeof buf - 1);
    buf[n] = 0;
    close(fd);
    printf("interleaved: %s\n", buf);
    CHECK(strcmp(buf, "a0;b1;a2;b3;a4;b5;a6;b7;a8;b9;") == 0);
    unlink("log");
    unlink("log2");
    return 0;
}
