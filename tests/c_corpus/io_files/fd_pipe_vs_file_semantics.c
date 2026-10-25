/*
 * title: Pipe versus regular file read and write semantics
 * topic: io_files
 * covers: ESPIPE, EOF handling, short reads, files growing under a reader, PIPE_BUF atomic writes, fstat type
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
static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
#define PIPE_ATOMIC 512 /* POSIX guarantees writes up to 512 bytes are atomic */

int main(void) {
    char buf[64];
    signal(SIGPIPE, SIG_IGN);

    /* seekability */
    int f = open("plain.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    int p[2];
    CHECK(f >= 0 && pipe(p) == 0);
    CHECK(wr_all(f, "0123456789", 10) == 0);
    off_t o = lseek(f, 2, SEEK_SET);
    printf("file lseek: %ld\n", (long)o);
    errno = 0;
    o = lseek(p[0], 2, SEEK_SET);
    printf("pipe lseek: %ld %s\n", (long)o, en(errno));
    CHECK(o < 0 && errno == ESPIPE);
    errno = 0;
    ssize_t n = pread(p[0], buf, 1, 0);
    printf("pipe pread: %ld %s\n", (long)n, en(errno));
    CHECK(n < 0 && errno == ESPIPE);
    errno = 0;
    n = pwrite(p[1], "x", 1, 0);
    printf("pipe pwrite: %ld %s\n", (long)n, en(errno));
    CHECK(n < 0 && errno == ESPIPE);

    /* types */
    struct stat st;
    CHECK(fstat(f, &st) == 0);
    printf("file: regular=%d fifo=%d\n", S_ISREG(st.st_mode) != 0, S_ISFIFO(st.st_mode) != 0);
    CHECK(fstat(p[0], &st) == 0);
    printf("pipe: regular=%d fifo=%d\n", S_ISREG(st.st_mode) != 0, S_ISFIFO(st.st_mode) != 0);

    /* short reads: a pipe returns what is there, a file returns what was asked for */
    CHECK(write(p[1], "abc", 3) == 3);
    n = read(p[0], buf, 10);
    printf("pipe read asked 10 with 3 available: %ld\n", (long)n);
    CHECK(n == 3);
    CHECK(lseek(f, 0, SEEK_SET) == 0);
    n = read(f, buf, 10);
    printf("file read asked 10 with 10 available: %ld\n", (long)n);
    CHECK(n == 10);
    n = read(f, buf, 10);
    printf("file read at end: %ld\n", (long)n);
    CHECK(n == 0);

    /* a regular file's EOF is not sticky; new data shows up on the next read */
    CHECK(write(f, "ABC", 3) == 3);
    CHECK(lseek(f, 10, SEEK_SET) == 10);
    n = read(f, buf, 10);
    printf("file read after growth: %ld\n", (long)n);
    CHECK(n == 3 && memcmp(buf, "ABC", 3) == 0);
    n = read(f, buf, 10);
    CHECK(n == 0);

    /* pipe EOF only after every writer is closed */
    int dupw = dup(p[1]);
    close(p[1]);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    errno = 0;
    n = read(p[0], buf, 10);
    printf("empty pipe with a writer still open: %ld %s\n", (long)n, en(errno));
    CHECK(n < 0 && errno == EAGAIN);
    close(dupw);
    n = read(p[0], buf, 10);
    printf("empty pipe with no writers: %ld\n", (long)n);
    CHECK(n == 0);
    n = read(p[0], buf, 10);
    CHECK(n == 0);
    puts("pipe EOF is repeatable");
    close(p[0]);

    /* writing to a pipe with no readers */
    CHECK(pipe(p) == 0);
    close(p[0]);
    errno = 0;
    n = write(p[1], "x", 1);
    printf("write with no reader: %ld %s\n", (long)n, en(errno));
    CHECK(n < 0 && errno == EPIPE);
    close(p[1]);

    /* concurrent writers with records of <= PIPE_BUF bytes never interleave */
    CHECK(pipe(p) == 0);
    pid_t kids[3];
    for (int k = 0; k < 3; k++) {
        kids[k] = fork();
        CHECK(kids[k] >= 0);
        if (kids[k] == 0) {
            close(p[0]);
            unsigned char rec[PIPE_ATOMIC];
            for (int i = 0; i < 100; i++) {
                unsigned len = 16 + (unsigned)((i * 37 + k * 101) % (PIPE_ATOMIC - 16));
                rec[0] = (unsigned char)('A' + k);
                rec[1] = (unsigned char)(len >> 8);
                rec[2] = (unsigned char)(len & 255);
                for (unsigned j = 3; j < len; j++) rec[j] = (unsigned char)('A' + k);
                if (write(p[1], rec, len) != (ssize_t)len) _exit(2);
            }
            _exit(0);
        }
    }
    close(p[1]);
    /* parent reassembles records from a byte stream that arrives in arbitrary pieces */
    unsigned char acc[4096];
    size_t have = 0;
    int counts[3] = {0, 0, 0}, corrupt = 0;
    for (;;) {
        n = read(p[0], acc + have, sizeof acc - have);
        if (n <= 0) break;
        have += (size_t)n;
        for (;;) {
            if (have < 3) break;
            size_t len = ((size_t)acc[1] << 8) | acc[2];
            if (have < len) break;
            int who = acc[0] - 'A';
            if (who < 0 || who > 2) { corrupt++; break; }
            for (size_t j = 3; j < len; j++)
                if (acc[j] != acc[0]) corrupt++;
            counts[who]++;
            memmove(acc, acc + len, have - len);
            have -= len;
        }
        if (corrupt) break;
    }
    close(p[0]);
    for (int k = 0; k < 3; k++) CHECK(wait_exit(kids[k]) == 0);
    printf("records by writer: A=%d B=%d C=%d corrupt=%d leftover=%zu\n", counts[0], counts[1], counts[2], corrupt, have);
    CHECK(counts[0] == 100 && counts[1] == 100 && counts[2] == 100 && corrupt == 0 && have == 0);

    close(f);
    unlink("plain.txt");
    return 0;
}
