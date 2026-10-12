/*
 * title: flock whole-file locks across descriptions and processes
 * topic: io_files
 * covers: flock, LOCK_SH, LOCK_EX, LOCK_NB, LOCK_UN, dup shares lock, fork inherits lock, blocking waiter
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
static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
static const char *try(int fd, int op) {
    if (flock(fd, op | LOCK_NB) == 0) return "granted";
    return errno == EWOULDBLOCK ? "would block" : "error";
}

int main(void) {
    int f1 = open("lockme", O_RDWR | O_CREAT | O_TRUNC, 0644);
    int f2 = open("lockme", O_RDWR);
    CHECK(f1 >= 0 && f2 >= 0);

    /* two separate opens of one file conflict even inside one process */
    printf("f1 LOCK_EX: %s\n", try(f1, LOCK_EX));
    printf("f2 LOCK_EX: %s\n", try(f2, LOCK_EX));
    printf("f2 LOCK_SH: %s\n", try(f2, LOCK_SH));
    CHECK(flock(f1, LOCK_UN) == 0);
    printf("f2 LOCK_EX after f1 unlock: %s\n", try(f2, LOCK_EX));

    /* downgrade to shared and let others in */
    CHECK(flock(f2, LOCK_SH) == 0);
    printf("f1 LOCK_SH while f2 is shared: %s\n", try(f1, LOCK_SH));
    printf("f1 LOCK_EX upgrade while f2 shared: %s\n", try(f1, LOCK_EX));
    CHECK(flock(f2, LOCK_UN) == 0);
    printf("f1 LOCK_EX upgrade once alone: %s\n", try(f1, LOCK_EX));
    CHECK(flock(f1, LOCK_UN) == 0);

    /* a dup shares the description, hence the lock; closing one alias keeps it */
    CHECK(flock(f1, LOCK_EX) == 0);
    int d = dup(f1);
    CHECK(d >= 0);
    printf("dup relocks (same description): %s\n", try(d, LOCK_EX));
    close(f1);
    printf("f2 after closing f1 (dup alive): %s\n", try(f2, LOCK_EX));
    close(d);
    printf("f2 after closing last alias: %s\n", try(f2, LOCK_EX));
    CHECK(flock(f2, LOCK_UN) == 0);

    /* fork shares the description; the child can release the parent's lock */
    f1 = open("lockme", O_RDWR);
    CHECK(f1 >= 0);
    CHECK(flock(f1, LOCK_EX) == 0);
    int go[2], done[2];
    CHECK(pipe(go) == 0 && pipe(done) == 0);
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        char t;
        if (read(go[0], &t, 1) != 1) _exit(2);
        if (flock(f1, LOCK_UN) != 0) _exit(3); /* unlocks the shared description */
        if (write(done[1], "u", 1) != 1) _exit(4);
        _exit(0);
    }
    printf("f2 while parent holds: %s\n", try(f2, LOCK_EX));
    CHECK(write(go[1], "g", 1) == 1);
    char t;
    CHECK(read(done[0], &t, 1) == 1);
    CHECK(wait_exit(c) == 0);
    printf("f2 after the child's LOCK_UN: %s\n", try(f2, LOCK_EX));
    CHECK(flock(f2, LOCK_UN) == 0);
    close(f1);
    close(go[0]); close(go[1]); close(done[0]); close(done[1]);

    /* a separate process blocks until the holder releases */
    CHECK(flock(f2, LOCK_EX) == 0);
    int ready[2], acq[2];
    CHECK(pipe(ready) == 0 && pipe(acq) == 0);
    c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        int fc = open("lockme", O_RDWR);
        if (fc < 0) _exit(2);
        if (write(ready[1], "r", 1) != 1) _exit(3);
        if (flock(fc, LOCK_EX) != 0) _exit(4); /* blocks */
        if (write(acq[1], "a", 1) != 1) _exit(5);
        _exit(0);
    }
    CHECK(read(ready[0], &t, 1) == 1);
    /* the waiter cannot have acquired while we hold the lock: nothing is on the pipe */
    struct pollfd pf = {acq[0], POLLIN, 0};
    int pr = poll(&pf, 1, 150);
    printf("waiter acquired while held: %s\n", pr == 0 ? "no" : "yes");
    CHECK(pr == 0);
    CHECK(flock(f2, LOCK_UN) == 0);
    CHECK(read(acq[0], &t, 1) == 1);
    puts("waiter acquired after release: yes");
    CHECK(wait_exit(c) == 0);
    /* the waiter exited, which drops its lock */
    printf("f2 after waiter exit: %s\n", try(f2, LOCK_EX));

    /* many processes take turns incrementing a text counter under LOCK_EX */
    CHECK(flock(f2, LOCK_UN) == 0);
    int cf = open("counter", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(cf >= 0 && wr_all(cf, "0       \n", 9) == 0);
    close(cf);
    pid_t kids[5];
    for (int k = 0; k < 5; k++) {
        kids[k] = fork();
        CHECK(kids[k] >= 0);
        if (kids[k] == 0) {
            int fc = open("counter", O_RDWR);
            if (fc < 0) _exit(2);
            for (int i = 0; i < 40; i++) {
                if (flock(fc, LOCK_EX) != 0) _exit(3);
                char buf[16];
                if (pread(fc, buf, 8, 0) != 8) _exit(4);
                buf[8] = 0;
                int v = atoi(buf) + 1;
                int n = snprintf(buf, sizeof buf, "%-8d", v);
                if (pwrite(fc, buf, (size_t)n, 0) != n) _exit(5);
                if (flock(fc, LOCK_UN) != 0) _exit(6);
            }
            _exit(0);
        }
    }
    for (int k = 0; k < 5; k++) CHECK(wait_exit(kids[k]) == 0);
    cf = open("counter", O_RDONLY);
    char buf[16] = {0};
    CHECK(pread(cf, buf, 8, 0) == 8);
    close(cf);
    printf("counter after 5 x 40 locked increments: %d\n", atoi(buf));
    CHECK(atoi(buf) == 200);

    close(f2);
    close(ready[0]); close(ready[1]); close(acq[0]); close(acq[1]);
    unlink("lockme");
    unlink("counter");
    return 0;
}
