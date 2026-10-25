/*
 * title: Retrying interrupted system calls after signals
 * topic: io_files
 * covers: EINTR, sigaction without SA_RESTART, pselect wait for pipe data, blocked signals, handler counting, retry loops for read and write
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
#include <sys/select.h>

static volatile sig_atomic_t hits = 0;

static void on_usr1(int sig) {
    (void)sig;
    hits++;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* deliberately no SA_RESTART */
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);

    sigset_t block, orig, empty;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigemptyset(&empty);
    CHECK(sigprocmask(SIG_BLOCK, &block, &orig) == 0);

    int data[2], ack[2], go[2];
    CHECK(pipe(data) == 0 && pipe(ack) == 0 && pipe(go) == 0);
    pid_t parent = getpid();
    enum { ROUNDS = 4 };
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        close(data[0]); close(ack[1]); close(go[1]);
        sigprocmask(SIG_SETMASK, &orig, NULL);
        for (int i = 0; i < ROUNDS; i++) {
            char t;
            if (read(go[0], &t, 1) != 1) _exit(2);   /* parent says: I am about to wait */
            if (kill(parent, SIGUSR1) != 0) _exit(3);
            if (read(ack[0], &t, 1) != 1) _exit(4);  /* parent says: handler ran */
            char payload = (char)('a' + i);
            if (write(data[1], &payload, 1) != 1) _exit(5);
        }
        _exit(0);
    }
    close(data[1]); close(ack[0]); close(go[0]);

    int eintr = 0, ready = 0;
    char got[ROUNDS + 1];
    for (int i = 0; i < ROUNDS; i++) {
        int before = hits;
        CHECK(write(go[1], "g", 1) == 1);
        int saw_eintr = 0;
        for (;;) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(data[0], &rf);
            /* pselect unblocks SIGUSR1 atomically while waiting, so the wakeup cannot be lost */
            int r = pselect(data[0] + 1, &rf, NULL, NULL, NULL, &empty);
            if (r < 0) {
                CHECK(errno == EINTR);
                saw_eintr = 1;
                eintr++;
                CHECK(hits == before + 1);
                CHECK(write(ack[1], "k", 1) == 1);
                continue;
            }
            ready++;
            break;
        }
        CHECK(saw_eintr); /* data cannot arrive before the acknowledgement */
        char b;
        ssize_t n;
        do n = read(data[0], &b, 1); while (n < 0 && errno == EINTR);
        CHECK(n == 1);
        got[i] = b;
    }
    got[ROUNDS] = 0;
    CHECK(wait_exit(c) == 0);
    printf("handler ran %d times, pselect interrupted %d times, became ready %d times\n", (int)hits, eintr, ready);
    printf("payload received in order: %s\n", got);
    CHECK(hits == ROUNDS && eintr == ROUNDS && ready == ROUNDS && strcmp(got, "abcd") == 0);
    close(data[0]); close(ack[1]); close(go[1]);

    /* a blocked signal stays pending and is delivered exactly once when unblocked */
    hits = 0;
    CHECK(kill(parent, SIGUSR1) == 0);
    CHECK(kill(parent, SIGUSR1) == 0); /* coalesced with the first */
    sigset_t pend;
    CHECK(sigpending(&pend) == 0);
    printf("pending while blocked: %d, handler count: %d\n", sigismember(&pend, SIGUSR1) == 1, (int)hits);
    CHECK(sigismember(&pend, SIGUSR1) == 1 && hits == 0);
    CHECK(sigprocmask(SIG_SETMASK, &orig, NULL) == 0);
    printf("after unblocking: handler count %d (two sends coalesced)\n", (int)hits);
    CHECK(hits == 1);

    /* a write loop that survives interruption: child fills a pipe while we get signalled */
    int p[2];
    CHECK(pipe(p) == 0);
    hits = 0;
    pid_t w = fork();
    CHECK(w >= 0);
    if (w == 0) {
        close(p[0]);
        static unsigned char big[200000];
        for (unsigned i = 0; i < sizeof big; i++) big[i] = (unsigned char)(i * 7);
        _exit(wr_all(p[1], big, sizeof big) == 0 ? 0 : 1);
    }
    close(p[1]);
    unsigned long total = 0, bad = 0;
    unsigned char buf[3000];
    int signalled = 0;
    for (;;) {
        ssize_t n = read(p[0], buf, sizeof buf);
        if (n < 0) { CHECK(errno == EINTR); continue; }
        if (n == 0) break;
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] != (unsigned char)((total + (unsigned long)i) * 7)) bad++;
        total += (unsigned long)n;
        if (!signalled && total > 50000) { CHECK(kill(parent, SIGUSR1) == 0); signalled = 1; }
    }
    close(p[0]);
    CHECK(wait_exit(w) == 0);
    printf("read loop through a signal: %lu bytes, corrupt=%lu, handler ran %d time\n", total, bad, (int)hits);
    CHECK(total == 200000 && bad == 0 && hits == 1);
    return 0;
}
