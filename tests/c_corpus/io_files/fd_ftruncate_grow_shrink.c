/*
 * title: ftruncate growing and shrinking with a model check
 * topic: io_files
 * covers: ftruncate, truncate, zero fill on grow, offset unchanged, random pwrite/ftruncate fuzz against a memory model
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
static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
#define MAXSZ 6000

static unsigned char model[MAXSZ];
static long model_len = 0;

static void model_write(long off, const unsigned char *p, long n) {
    if (off + n > model_len) {
        memset(model + model_len, 0, (size_t)(off + n - model_len));
        model_len = off + n;
    }
    memcpy(model + off, p, (size_t)n);
}

static void model_trunc(long n) {
    if (n > model_len) memset(model + model_len, 0, (size_t)(n - model_len));
    model_len = n;
}

static void compare(int fd, const char *when) {
    CHECK(fsize(fd) == model_len);
    unsigned char *buf = malloc((size_t)model_len + 1);
    CHECK(buf != NULL);
    CHECK(pread(fd, buf, (size_t)model_len, 0) == model_len);
    if (memcmp(buf, model, (size_t)model_len) != 0) {
        fprintf(stderr, "content mismatch %s\n", when);
        exit(1);
    }
    free(buf);
}

int main(void) {
    int fd = open("t.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(wr_all(fd, "0123456789", 10) == 0);
    printf("size after write: %ld, offset %ld\n", fsize(fd), (long)lseek(fd, 0, SEEK_CUR));

    CHECK(ftruncate(fd, 4) == 0);
    printf("shrink to 4: size=%ld offset=%ld\n", fsize(fd), (long)lseek(fd, 0, SEEK_CUR));
    CHECK(fsize(fd) == 4 && lseek(fd, 0, SEEK_CUR) == 10);

    /* growing again must expose zeros, never the old bytes */
    CHECK(ftruncate(fd, 12) == 0);
    unsigned char b[12];
    CHECK(pread(fd, b, 12, 0) == 12);
    printf("regrown content:");
    for (int i = 0; i < 12; i++) printf(" %02x", b[i]);
    putchar('\n');
    CHECK(memcmp(b, "0123", 4) == 0);
    for (int i = 4; i < 12; i++) CHECK(b[i] == 0);

    /* writing at the stale offset after shrinking creates a hole */
    CHECK(ftruncate(fd, 2) == 0);
    CHECK(write(fd, "X", 1) == 1); /* offset was 10 */
    printf("write at stale offset: size=%ld\n", fsize(fd));
    CHECK(fsize(fd) == 11);
    CHECK(pread(fd, b, 11, 0) == 11);
    CHECK(b[0] == '0' && b[1] == '1' && b[2] == 0 && b[9] == 0 && b[10] == 'X');

    /* same size is a no-op; zero empties the file */
    CHECK(ftruncate(fd, 11) == 0 && fsize(fd) == 11);
    CHECK(ftruncate(fd, 0) == 0 && fsize(fd) == 0);
    puts("truncate to 0: empty");

    errno = 0;
    int r = ftruncate(fd, -1);
    printf("ftruncate(-1): %d %s\n", r, en(errno));
    CHECK(r < 0 && errno == EINVAL);

    /* path-based truncate works on closed files too */
    CHECK(truncate("t.bin", 100) == 0);
    CHECK(fsize(fd) == 100);
    CHECK(truncate("t.bin", 30) == 0);
    printf("truncate() by path: size=%ld\n", fsize(fd));
    errno = 0;
    r = truncate("no-such-file", 5);
    printf("truncate missing: %d %s\n", r, en(errno));
    CHECK(r < 0 && errno == ENOENT);

    /* fuzz: random pwrite and ftruncate against a memory model */
    CHECK(ftruncate(fd, 0) == 0);
    model_len = 0;
    int grows = 0, shrinks = 0, writes = 0;
    for (int i = 0; i < 400; i++) {
        unsigned op = rnd_below(10);
        if (op < 5) {
            long off = rnd_below(3000);
            long n = 1 + rnd_below(400);
            unsigned char data[400];
            for (long k = 0; k < n; k++) data[k] = (unsigned char)(1 + rnd_below(255));
            CHECK(pwrite(fd, data, (size_t)n, off) == n);
            model_write(off, data, n);
            writes++;
        } else {
            long nl = rnd_below(4000);
            if (nl > model_len) grows++;
            else if (nl < model_len) shrinks++;
            CHECK(ftruncate(fd, nl) == 0);
            model_trunc(nl);
        }
        if (i % 20 == 19) compare(fd, "mid-run");
    }
    compare(fd, "final");
    unsigned long long h = fnv(FNV0, model, (size_t)model_len);
    printf("fuzz: writes=%d grows=%d shrinks=%d final size=%ld hash=%016llx\n", writes, grows, shrinks, model_len, h);
    close(fd);
    unlink("t.bin");
    return 0;
}
