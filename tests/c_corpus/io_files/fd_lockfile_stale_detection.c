/*
 * title: O_EXCL lockfile with stale lock detection
 * topic: io_files
 * covers: O_EXCL lockfile, owner pid inside the file, kill(pid, 0) liveness probe, stale lock stealing, corrupt lock files, contention with fork
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
#define LOCK "app.lock"

typedef enum { ACQUIRED, BUSY_LIVE, STOLE_STALE, STOLE_CORRUPT } Result;

static const char *rname(Result r) {
    switch (r) {
    case ACQUIRED: return "acquired";
    case BUSY_LIVE: return "busy (owner alive)";
    case STOLE_STALE: return "stole stale lock";
    default: return "stole corrupt lock";
    }
}

static int try_create(pid_t me) {
    int fd = open(LOCK, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return -1;
    char b[32];
    int n = snprintf(b, sizeof b, "%ld\n", (long)me);
    CHECK(wr_all(fd, b, (size_t)n) == 0);
    close(fd);
    return 0;
}

/* read the owner pid; returns -1 when the file is empty or malformed */
static long lock_owner(void) {
    char b[32];
    int fd = open(LOCK, O_RDONLY);
    if (fd < 0) return -2;
    long n = rd_full(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    char *end;
    long v = strtol(b, &end, 10);
    if (end == b || *end != '\n' || v <= 0) return -1;
    return v;
}

static Result acquire(pid_t me, long *owner_out) {
    if (try_create(me) == 0) return ACQUIRED;
    CHECK(errno == EEXIST);
    long owner = lock_owner();
    *owner_out = owner;
    if (owner == -1) {
        CHECK(unlink(LOCK) == 0);
        CHECK(try_create(me) == 0);
        return STOLE_CORRUPT;
    }
    if (owner > 0 && (kill((pid_t)owner, 0) == 0 || errno == EPERM)) return BUSY_LIVE;
    CHECK(unlink(LOCK) == 0);
    CHECK(try_create(me) == 0);
    return STOLE_STALE;
}

static int release(pid_t me) {
    if (lock_owner() != (long)me) return -1;
    return unlink(LOCK);
}

int main(void) {
    pid_t me = getpid();
    long owner = 0;

    Result r = acquire(me, &owner);
    printf("1. fresh acquire: %s\n", rname(r));
    CHECK(r == ACQUIRED);
    CHECK(lock_owner() == (long)me);

    /* a second attempt from a child sees a live owner (the parent) */
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        long o = 0;
        Result cr = acquire(getpid(), &o);
        _exit(cr == BUSY_LIVE && o == (long)getppid() ? 0 : 1);
    }
    CHECK(wait_exit(c) == 0);
    puts("2. child sees live owner (the parent): busy");

    /* only the owner may release */
    c = fork();
    CHECK(c >= 0);
    if (c == 0) _exit(release(getpid()) == -1 ? 0 : 1);
    CHECK(wait_exit(c) == 0);
    puts("3. non-owner release refused");
    CHECK(release(me) == 0);
    puts("4. owner release ok");

    /* a child takes the lock and dies without releasing it */
    c = fork();
    CHECK(c >= 0);
    if (c == 0) _exit(try_create(getpid()) == 0 ? 0 : 1);
    CHECK(wait_exit(c) == 0);
    CHECK(lock_owner() == (long)c);
    r = acquire(me, &owner);
    printf("5. lock left by dead child: %s (owner was the dead child: %d)\n", rname(r), owner == (long)c);
    CHECK(r == STOLE_STALE && owner == (long)c);
    CHECK(lock_owner() == (long)me);
    CHECK(release(me) == 0);

    /* a live child holds the lock until told to release */
    int go[2], held[2];
    CHECK(pipe(go) == 0 && pipe(held) == 0);
    c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        if (try_create(getpid()) != 0) _exit(2);
        if (write(held[1], "h", 1) != 1) _exit(3);
        char t;
        if (read(go[0], &t, 1) != 1) _exit(4);
        _exit(release(getpid()) == 0 ? 0 : 5);
    }
    char t;
    CHECK(read(held[0], &t, 1) == 1);
    r = acquire(me, &owner);
    printf("6. child holds lock: %s (owner is the child: %d)\n", rname(r), owner == (long)c);
    CHECK(r == BUSY_LIVE && owner == (long)c);
    CHECK(write(go[1], "g", 1) == 1);
    CHECK(wait_exit(c) == 0);
    CHECK(lock_owner() == -2);
    r = acquire(me, &owner);
    printf("7. after the child released: %s\n", rname(r));
    CHECK(r == ACQUIRED);
    CHECK(release(me) == 0);
    close(go[0]); close(go[1]); close(held[0]); close(held[1]);

    /* corrupt or empty lock files are reclaimed */
    int fd = open(LOCK, O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0);
    close(fd);
    r = acquire(me, &owner);
    printf("8. empty lock file: %s\n", rname(r));
    CHECK(r == STOLE_CORRUPT);
    CHECK(release(me) == 0);
    fd = open(LOCK, O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0 && wr_all(fd, "garbage\n", 8) == 0);
    close(fd);
    r = acquire(me, &owner);
    printf("9. garbage lock file: %s\n", rname(r));
    CHECK(r == STOLE_CORRUPT);
    CHECK(release(me) == 0);

    /* contention: children spin on the lock and do a non-atomic read-modify-write in the critical section */
    fd = open("shared.cnt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && wr_all(fd, "0       ", 8) == 0);
    close(fd);
    enum { KIDS = 5, ROUNDS = 25 };
    pid_t kids[KIDS];
    for (int k = 0; k < KIDS; k++) {
        kids[k] = fork();
        CHECK(kids[k] >= 0);
        if (kids[k] == 0) {
            pid_t self = getpid();
            for (int i = 0; i < ROUNDS; i++) {
                while (try_create(self) != 0) {
                    if (errno != EEXIST) _exit(2);
                    usleep(100);
                }
                int f = open("shared.cnt", O_RDWR);
                char b[16];
                if (f < 0 || pread(f, b, 8, 0) != 8) _exit(3);
                b[8] = 0;
                int v = atoi(b) + 1;
                usleep(50); /* widen the race window */
                int n = snprintf(b, sizeof b, "%-8d", v);
                if (pwrite(f, b, (size_t)n, 0) != n) _exit(4);
                close(f);
                if (release(self) != 0) _exit(5);
            }
            _exit(0);
        }
    }
    for (int k = 0; k < KIDS; k++) CHECK(wait_exit(kids[k]) == 0);
    fd = open("shared.cnt", O_RDONLY);
    char b[16] = {0};
    CHECK(pread(fd, b, 8, 0) == 8);
    close(fd);
    printf("10. counter after %d guarded increments: %d\n", KIDS * ROUNDS, atoi(b));
    CHECK(atoi(b) == KIDS * ROUNDS);
    CHECK(lock_owner() == -2);
    puts("11. no lock file left behind");
    unlink("shared.cnt");
    return 0;
}
