/*
 * title: dup shares the offset, separate open does not
 * topic: io_files
 * covers: dup, open file description, shared offset, independent opens, fork inheritance of offsets
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
static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
static long tell(int fd) { return (long)lseek(fd, 0, SEEK_CUR); }

static void read_str(int fd, char *out, size_t n) {
    CHECK(rd_full(fd, out, n) == (long)n);
    out[n] = 0;
}

int main(void) {
    int fd = open("alpha.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(wr_all(fd, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);

    int d = dup(fd);
    CHECK(d >= 0 && d != fd);
    int o = open("alpha.txt", O_RDWR);
    CHECK(o >= 0);

    char b[16];
    read_str(fd, b, 3);
    printf("fd read 3: %s\n", b);
    printf("dup offset now: %ld\n", tell(d));
    CHECK(tell(d) == 3);
    printf("separate open offset: %ld\n", tell(o));
    CHECK(tell(o) == 0);

    read_str(d, b, 4);
    printf("dup read 4: %s\n", b);
    printf("fd offset now: %ld\n", tell(fd));
    CHECK(tell(fd) == 7);

    read_str(o, b, 2);
    printf("separate open read 2: %s\n", b);
    CHECK(tell(fd) == 7 && tell(o) == 2);

    /* seeking through one alias moves the other */
    CHECK(lseek(d, 20, SEEK_SET) == 20);
    read_str(fd, b, 3);
    printf("fd after seek via dup: %s\n", b);
    CHECK(strcmp(b, "UVW") == 0);

    /* closing one alias does not close the description */
    close(d);
    read_str(fd, b, 2);
    printf("after closing dup, fd reads: %s\n", b);
    CHECK(strcmp(b, "XY") == 0);

    /* a write through the separate open is visible to the aliased reader */
    CHECK(pwrite(o, "z", 1, 25) == 1);
    read_str(fd, b, 1);
    printf("last byte after write via other open: %s\n", b);
    CHECK(b[0] == 'z');

    /* fork: parent and child share the description, so reads interleave */
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    int sync[2];
    CHECK(pipe(sync) == 0);
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        char cb[8];
        if (rd_full(fd, cb, 5) != 5) _exit(2);
        if (memcmp(cb, "ABCDE", 5) != 0) _exit(3);
        if (write(sync[1], "k", 1) != 1) _exit(4);
        _exit(0);
    }
    char k;
    CHECK(read(sync[0], &k, 1) == 1);
    CHECK(wait_exit(c) == 0);
    printf("parent offset after child read 5: %ld\n", tell(fd));
    CHECK(tell(fd) == 5);
    read_str(fd, b, 3);
    printf("parent continues with: %s\n", b);
    CHECK(strcmp(b, "FGH") == 0);
    close(sync[0]);
    close(sync[1]);

    /* three-way alias chain: offset is one shared variable */
    int a1 = dup(fd), a2 = dup(a1);
    CHECK(a1 >= 0 && a2 >= 0);
    read_str(a1, b, 2);
    read_str(a2, b + 2, 2);
    b[4] = 0;
    printf("interleaved via aliases: %s\n", b);
    CHECK(strcmp(b, "IJKL") == 0 && tell(fd) == 12);
    close(a1);
    close(a2);
    close(o);
    close(fd);
    unlink("alpha.txt");
    return 0;
}
