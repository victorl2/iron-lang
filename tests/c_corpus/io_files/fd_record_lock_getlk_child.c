/*
 * title: fcntl record locks probed from a child process
 * topic: io_files
 * covers: fcntl F_SETLK, F_GETLK, F_WRLCK, F_RDLCK, F_UNLCK, lock ownership per process, request/response over pipes
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
typedef struct {
    int cmd;   /* 0 = F_GETLK, 1 = F_SETLK, 9 = quit */
    int type;  /* 0 = read, 1 = write, 2 = unlock */
    long start;
    long len;
} Req;

typedef struct {
    int ok;        /* SETLK: 1 if granted; GETLK: 1 if a conflict was reported */
    int busy_errno_ok; /* SETLK failure had errno EAGAIN or EACCES */
    int ltype;     /* GETLK: 0 read, 1 write, 2 none */
    long start;
    long len;
    int owner_is_parent;
} Resp;

static short to_ltype(int t) { return t == 0 ? F_RDLCK : t == 1 ? F_WRLCK : F_UNLCK; }
static int from_ltype(short t) { return t == F_RDLCK ? 0 : t == F_WRLCK ? 1 : 2; }

static void child_loop(int fd, int rq, int rs, pid_t parent) {
    Req q;
    for (;;) {
        if (rd_full(rq, &q, sizeof q) != (long)sizeof q) _exit(2);
        if (q.cmd == 9) _exit(0);
        struct flock fl;
        memset(&fl, 0, sizeof fl);
        fl.l_type = to_ltype(q.type);
        fl.l_whence = SEEK_SET;
        fl.l_start = q.start;
        fl.l_len = q.len;
        Resp r;
        memset(&r, 0, sizeof r);
        if (q.cmd == 0) {
            if (fcntl(fd, F_GETLK, &fl) != 0) _exit(3);
            r.ltype = from_ltype(fl.l_type);
            r.ok = fl.l_type != F_UNLCK;
            r.start = (long)fl.l_start;
            r.len = (long)fl.l_len;
            r.owner_is_parent = fl.l_pid == parent;
        } else {
            int rc = fcntl(fd, F_SETLK, &fl);
            r.ok = rc == 0;
            r.busy_errno_ok = rc != 0 && (errno == EAGAIN || errno == EACCES);
        }
        if (wr_all(rs, &r, sizeof r) != 0) _exit(4);
    }
}

static int rq_fd, rs_fd;

static Resp ask(int cmd, int type, long start, long len) {
    Req q = {cmd, type, start, len};
    Resp r;
    CHECK(wr_all(rq_fd, &q, sizeof q) == 0);
    CHECK(rd_full(rs_fd, &r, sizeof r) == (long)sizeof r);
    return r;
}

static void parent_lock(int fd, int type, long start, long len) {
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = to_ltype(type);
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    CHECK(fcntl(fd, F_SETLK, &fl) == 0);
}

static const char *tn(int t) { return t == 0 ? "read" : t == 1 ? "write" : "none"; }

static void probe_get(const char *what, int type, long start, long len) {
    Resp r = ask(0, type, start, len);
    if (r.ok) printf("%-40s conflict: %s lock at %ld len %ld, owner is parent: %d\n", what, tn(r.ltype), r.start, r.len, r.owner_is_parent);
    else printf("%-40s no conflict\n", what);
}

static int probe_set(const char *what, int type, long start, long len) {
    Resp r = ask(1, type, start, len);
    printf("%-40s %s\n", what, r.ok ? "granted" : (r.busy_errno_ok ? "denied (EAGAIN/EACCES)" : "denied (unexpected)"));
    CHECK(r.ok || r.busy_errno_ok);
    return r.ok;
}

int main(void) {
    int fd = open("locked.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    unsigned char z[256];
    memset(z, 0, sizeof z);
    CHECK(wr_all(fd, z, sizeof z) == 0);

    int a[2], b[2];
    CHECK(pipe(a) == 0 && pipe(b) == 0);
    pid_t me = getpid();
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        close(a[1]); close(b[0]);
        child_loop(fd, a[0], b[1], me);
    }
    close(a[0]); close(b[1]);
    rq_fd = a[1];
    rs_fd = b[0];

    /* the child inherits the descriptor but not the parent's locks */
    parent_lock(fd, 1, 0, 100);
    puts("parent holds write lock on [0,100)");
    probe_get("child GETLK write [50,60)", 1, 50, 10);
    probe_get("child GETLK read [90,120)", 0, 90, 30);
    probe_get("child GETLK write [100,110)", 1, 100, 10);
    CHECK(!probe_set("child SETLK write [10,20)", 1, 10, 10));
    CHECK(!probe_set("child SETLK read [99,100)", 0, 99, 1));
    CHECK(probe_set("child SETLK write [100,150)", 1, 100, 50));
    probe_get("child GETLK after its own lock [120,130)", 1, 120, 10);

    /* parent downgrades to a shared lock: readers may now share */
    parent_lock(fd, 0, 0, 100);
    puts("parent downgrades to read lock on [0,100)");
    probe_get("child GETLK read [0,10)", 0, 0, 10);
    probe_get("child GETLK write [0,10)", 1, 0, 10);
    CHECK(probe_set("child SETLK read [20,30)", 0, 20, 10));
    CHECK(!probe_set("child SETLK write [25,26)", 1, 25, 1));
    CHECK(probe_set("child unlock [20,30)", 2, 20, 10));

    /* the parent probes the child's remaining write lock on [100,150) itself */
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 140;
    fl.l_len = 20;
    CHECK(fcntl(fd, F_GETLK, &fl) == 0);
    printf("parent GETLK write [140,160): %s at %ld len %ld, owner is child: %d\n",
           tn(from_ltype(fl.l_type)), (long)fl.l_start, (long)fl.l_len, fl.l_pid == c);
    CHECK(fl.l_type == F_WRLCK && fl.l_pid == c);

    /* unlocking releases the range for the other process */
    parent_lock(fd, 2, 0, 100);
    puts("parent unlocks [0,100)");
    probe_get("child GETLK write [0,100)", 1, 0, 100);
    CHECK(probe_set("child SETLK write [0,100)", 1, 0, 100));

    /* lock to end of file (len 0) */
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 140;
    fl.l_len = 0;
    CHECK(fcntl(fd, F_GETLK, &fl) == 0);
    printf("parent GETLK read [140,EOF): %s\n", tn(from_ltype(fl.l_type)));
    CHECK(fl.l_type == F_WRLCK);
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 150;
    fl.l_len = 0;
    CHECK(fcntl(fd, F_GETLK, &fl) == 0);
    printf("parent GETLK read [150,EOF): %s\n", tn(from_ltype(fl.l_type)));
    CHECK(fl.l_type == F_UNLCK);
    CHECK(probe_set("child releases everything", 2, 0, 0));
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    CHECK(fcntl(fd, F_GETLK, &fl) == 0);
    printf("parent GETLK write [0,EOF) after release: %s\n", tn(from_ltype(fl.l_type)));
    CHECK(fl.l_type == F_UNLCK);

    Req quit = {9, 0, 0, 0};
    CHECK(wr_all(rq_fd, &quit, sizeof quit) == 0);
    CHECK(wait_exit(c) == 0);
    close(a[1]); close(b[0]); close(fd);
    unlink("locked.dat");
    return 0;
}
