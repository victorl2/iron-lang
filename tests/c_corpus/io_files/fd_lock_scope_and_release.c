/*
 * title: Record lock ownership, splitting and implicit release
 * topic: io_files
 * covers: fcntl locks are per process, close of any fd drops all locks, range splitting by unlock, upgrade and downgrade over sub-ranges, child probes via GETLK
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
typedef struct { int type; long start; long len; } Q;   /* type: 0 read 1 write */
typedef struct { int type; long start; long len; } A;   /* type: 0 read 1 write 2 none */

static void child_loop(int fd, int rq, int rs) {
    Q q;
    while (rd_full(rq, &q, sizeof q) == (long)sizeof q) {
        struct flock fl;
        memset(&fl, 0, sizeof fl);
        fl.l_type = q.type == 0 ? F_RDLCK : F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = q.start;
        fl.l_len = q.len;
        if (fcntl(fd, F_GETLK, &fl) != 0) _exit(2);
        A a = {fl.l_type == F_RDLCK ? 0 : fl.l_type == F_WRLCK ? 1 : 2, (long)fl.l_start, (long)fl.l_len};
        if (wr_all(rs, &a, sizeof a) != 0) _exit(3);
    }
    _exit(0);
}

static int qfd, afd;

static A probe(int type, long start, long len) {
    Q q = {type, start, len};
    A a;
    CHECK(wr_all(qfd, &q, sizeof q) == 0 && rd_full(afd, &a, sizeof a) == (long)sizeof a);
    return a;
}

static void lockp(int fd, short type, long start, long len) {
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    CHECK(fcntl(fd, F_SETLK, &fl) == 0);
}

static const char *tn(int t) { return t == 0 ? "read" : t == 1 ? "write" : "none"; }

/* describe what a child sees when it asks for a write lock on each 10-byte cell of [0,100) */
static void map(const char *title, char *marks) {
    for (int i = 0; i < 10; i++) {
        A a = probe(1, i * 10, 10);
        marks[i] = a.type == 2 ? '.' : a.type == 0 ? 'r' : 'W';
    }
    marks[10] = 0;
    printf("%-46s [%s]\n", title, marks);
}

int main(void) {
    int fd = open("lockmap.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    unsigned char z[200] = {0};
    CHECK(wr_all(fd, z, sizeof z) == 0);

    int a[2], b[2];
    CHECK(pipe(a) == 0 && pipe(b) == 0);
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        close(a[1]); close(b[0]);
        child_loop(fd, a[0], b[1]);
    }
    close(a[0]); close(b[1]);
    qfd = a[1];
    afd = b[0];
    char m[11];

    map("no locks", m);
    CHECK(strcmp(m, "..........") == 0);

    lockp(fd, F_WRLCK, 20, 50);
    map("write [20,70)", m);
    CHECK(strcmp(m, "..WWWWW...") == 0);

    lockp(fd, F_UNLCK, 40, 10);
    map("unlock [40,50) splits the lock", m);
    CHECK(strcmp(m, "..WW.WW...") == 0);

    lockp(fd, F_RDLCK, 20, 20);
    map("downgrade [20,40) to read", m);
    CHECK(strcmp(m, "..rr.WW...") == 0);

    lockp(fd, F_RDLCK, 40, 10);
    lockp(fd, F_RDLCK, 50, 20);
    map("read-lock the rest: one shared region", m);
    CHECK(strcmp(m, "..rrrrr...") == 0);

    lockp(fd, F_WRLCK, 30, 20);
    map("upgrade [30,50) to write", m);
    CHECK(strcmp(m, "..rWWrr...") == 0);

    A r = probe(1, 32, 4);
    printf("child asks about [32,36): %s\n", tn(r.type));
    CHECK(r.type == 1);
    r = probe(0, 32, 4);
    printf("child asks read about [32,36): %s\n", tn(r.type));
    CHECK(r.type == 1);
    r = probe(0, 55, 4);
    printf("child asks read about [55,59): %s\n", tn(r.type));
    CHECK(r.type == 2);

    /* POSIX quirk: closing ANY descriptor of the file drops every lock the process holds on it */
    int other = open("lockmap.dat", O_RDONLY);
    CHECK(other >= 0);
    map("before closing an unrelated descriptor", m);
    close(other);
    map("after closing a second descriptor of the file", m);
    CHECK(strcmp(m, "..........") == 0);

    /* locks are lost too when the same descriptor is dup'ed and one alias closes */
    lockp(fd, F_WRLCK, 0, 10);
    int d = dup(fd);
    CHECK(d >= 0);
    close(d);
    map("after closing a dup of the locking descriptor", m);
    CHECK(m[0] == '.');

    /* re-lock, then the process holding it exits: locks vanish with it */
    lockp(fd, F_WRLCK, 90, 10);
    map("re-locked [90,100)", m);
    CHECK(m[9] == 'W');
    close(qfd);
    CHECK(wait_exit(c) == 0);
    close(afd);
    puts("probe child exited");

    /* a forked child does not inherit locks, but sees the parent's as conflicts (done above) */
    pid_t k = fork();
    CHECK(k >= 0);
    if (k == 0) {
        struct flock fl;
        memset(&fl, 0, sizeof fl);
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 90;
        fl.l_len = 10;
        _exit(fcntl(fd, F_SETLK, &fl) == -1 && (errno == EAGAIN || errno == EACCES) ? 0 : 1);
    }
    CHECK(wait_exit(k) == 0);
    puts("a forked child cannot take the parent's locked range");
    close(fd);
    unlink("lockmap.dat");
    return 0;
}
