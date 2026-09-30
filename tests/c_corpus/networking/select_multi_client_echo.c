/*
 * title: Single-threaded select echo server for several clients
 * topic: networking
 * covers: select, fd_set, FD_SETSIZE bookkeeping, multiplexed accept and read, per-client byte counts, client thread
 * deps: libc, posix, pthread, sockets
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}
static void set_timeout(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    check(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0, "SO_RCVTIMEO");
    check(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0, "SO_SNDTIMEO");
}
/* socket bound to 127.0.0.1:0; stream sockets also listen */
static int make_socket(int type, int backlog) {
    int fd = socket(AF_INET, type, 0);
    check(fd >= 0, "socket");
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    check(bind(fd, (struct sockaddr *)&a, sizeof a) == 0, "bind");
    if (type == SOCK_STREAM)
        check(listen(fd, backlog) == 0, "listen");
    return fd;
}
static struct sockaddr_in local_addr(int fd) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    memset(&a, 0, sizeof a);
    check(getsockname(fd, (struct sockaddr *)&a, &l) == 0, "getsockname");
    check(l == sizeof a && a.sin_family == AF_INET, "sockname family");
    return a;
}
static int send_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
/* reads until n bytes, EOF, or error; returns bytes read */
static size_t recv_all(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    return got;
}

enum { NCLI = 5, ROUNDS = 4 };

typedef struct {
    struct sockaddr_in addr;
    size_t bytes[NCLI];
    int ok;
} Clients;

/* client thread: opens all connections first, then does interleaved request rounds */
static void *clients_main(void *arg) {
    Clients *cl = arg;
    int fd[NCLI];
    cl->ok = 1;
    for (int i = 0; i < NCLI; i++) {
        fd[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (fd[i] < 0)
            exit(1);
        set_timeout(fd[i], 3000);
        if (connect(fd[i], (struct sockaddr *)&cl->addr, sizeof cl->addr) != 0)
            exit(1);
    }
    for (int round = 0; round < ROUNDS; round++) {
        for (int i = 0; i < NCLI; i++) {
            char msg[64];
            int n = snprintf(msg, sizeof msg, "c%d/r%d:%*s", i, round, (i + round) * 3, "");
            if (send_all(fd[i], msg, (size_t)n) < 0)
                cl->ok = 0;
            char back[64];
            if (recv_all(fd[i], back, (size_t)n) != (size_t)n || memcmp(back, msg, (size_t)n) != 0)
                cl->ok = 0;
            cl->bytes[i] += (size_t)n;
        }
    }
    for (int i = 0; i < NCLI; i++)
        close(fd[i]);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 8);
    Clients cl;
    memset(&cl, 0, sizeof cl);
    cl.addr = local_addr(ls);
    pthread_t th;
    check(pthread_create(&th, NULL, clients_main, &cl) == 0, "thread");

    int fds[NCLI];
    size_t served[NCLI];
    int nfds = 0, closed = 0, iterations = 0;
    while (closed < NCLI) {
        check(++iterations < 2000, "loop bound");
        fd_set rs;
        FD_ZERO(&rs);
        int maxfd = ls;
        if (nfds < NCLI)
            FD_SET(ls, &rs);
        for (int i = 0; i < nfds; i++) {
            if (fds[i] >= 0) {
                FD_SET(fds[i], &rs);
                if (fds[i] > maxfd)
                    maxfd = fds[i];
            }
        }
        struct timeval tv = {3, 0};
        int n = select(maxfd + 1, &rs, NULL, NULL, &tv);
        check(n > 0, "select made progress");
        if (nfds < NCLI && FD_ISSET(ls, &rs)) {
            int c = accept(ls, NULL, NULL);
            check(c >= 0 && c < FD_SETSIZE, "accept");
            fds[nfds] = c;
            served[nfds] = 0;
            nfds++;
        }
        for (int i = 0; i < nfds; i++) {
            if (fds[i] < 0 || !FD_ISSET(fds[i], &rs))
                continue;
            unsigned char buf[128];
            ssize_t r = recv(fds[i], buf, sizeof buf, 0);
            if (r <= 0) {
                close(fds[i]);
                fds[i] = -1;
                closed++;
            } else {
                check(send_all(fds[i], buf, (size_t)r) == 0, "echo");
                served[i] += (size_t)r;
            }
        }
    }
    pthread_join(th, NULL);
    check(cl.ok, "clients verified echoes");
    size_t total = 0;
    for (int i = 0; i < NCLI; i++) {
        printf("client %d: %zu bytes sent, %zu echoed\n", i, cl.bytes[i], served[i]);
        check(cl.bytes[i] == served[i], "byte counts");
        total += served[i];
    }
    printf("total %zu bytes over %d connections\n", total, NCLI);
    close(ls);
    return 0;
}
