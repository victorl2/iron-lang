/*
 * title: Per-slot counters guarded by blocking record locks
 * topic: io_files
 * covers: fcntl F_SETLKW, byte-range locks, read-modify-write on a shared file, fork, replayed PRNG for expected totals
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
#define SLOTS 8
#define SLOT_BYTES 16
#define KIDS 4
#define OPS 400

static void lock_range(int fd, short type, off_t start, off_t len) {
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    int rc;
    do rc = fcntl(fd, F_SETLKW, &fl); while (rc < 0 && errno == EINTR);
    if (rc < 0) _exit(90);
}

/* Each child picks slots and deltas from its own seeded stream. */
static void plan_op(unsigned long long *st, unsigned *slot, unsigned *delta) {
    unsigned long long saved = xs_state;
    xs_state = *st;
    unsigned s = rnd_below(SLOTS);
    unsigned d = 1 + rnd_below(9);
    *st = xs_state;
    xs_state = saved;
    *slot = s;
    *delta = d;
}

static void child(int id) {
    int fd = open("counters.dat", O_RDWR);
    if (fd < 0) _exit(2);
    unsigned long long st = 0x1234567ULL * (unsigned long long)(id + 1) + 99;
    for (int i = 0; i < OPS; i++) {
        unsigned slot, delta;
        plan_op(&st, &slot, &delta);
        off_t off = (off_t)slot * SLOT_BYTES;
        lock_range(fd, F_WRLCK, off, SLOT_BYTES);
        char buf[SLOT_BYTES + 1];
        if (pread(fd, buf, SLOT_BYTES, off) != SLOT_BYTES) _exit(3);
        buf[SLOT_BYTES] = 0;
        long v = atol(buf) + delta;
        usleep(20); /* hold the lock across a scheduling point */
        int n = snprintf(buf, sizeof buf, "%-15ld\n", v);
        if (pwrite(fd, buf, (size_t)n, off) != n) _exit(4);
        lock_range(fd, F_UNLCK, off, SLOT_BYTES);
    }
    close(fd);
    _exit(0);
}

int main(void) {
    int fd = open("counters.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    for (int s = 0; s < SLOTS; s++) CHECK(wr_all(fd, "0              \n", SLOT_BYTES) == 0);
    close(fd);

    pid_t kids[KIDS];
    for (int k = 0; k < KIDS; k++) {
        kids[k] = fork();
        CHECK(kids[k] >= 0);
        if (kids[k] == 0) child(k);
    }
    for (int k = 0; k < KIDS; k++) CHECK(wait_exit(kids[k]) == 0);

    /* replay every child's stream to know the exact expected totals */
    long expect[SLOTS] = {0};
    long ops_on[SLOTS] = {0};
    for (int k = 0; k < KIDS; k++) {
        unsigned long long st = 0x1234567ULL * (unsigned long long)(k + 1) + 99;
        for (int i = 0; i < OPS; i++) {
            unsigned slot, delta;
            plan_op(&st, &slot, &delta);
            expect[slot] += delta;
            ops_on[slot]++;
        }
    }

    fd = open("counters.dat", O_RDONLY);
    CHECK(fd >= 0);
    CHECK(fsize(fd) == SLOTS * SLOT_BYTES);
    long total = 0, total_ops = 0;
    for (int s = 0; s < SLOTS; s++) {
        char buf[SLOT_BYTES + 1];
        CHECK(pread(fd, buf, SLOT_BYTES, (off_t)s * SLOT_BYTES) == SLOT_BYTES);
        buf[SLOT_BYTES] = 0;
        long v = atol(buf);
        printf("slot %d: ops=%ld value=%ld expected=%ld\n", s, ops_on[s], v, expect[s]);
        CHECK(v == expect[s]);
        total += v;
        total_ops += ops_on[s];
    }
    printf("total value=%ld over %ld locked updates\n", total, total_ops);
    CHECK(total_ops == KIDS * OPS);
    close(fd);
    unlink("counters.dat");
    return 0;
}
