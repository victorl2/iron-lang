/*
 * title: SO_REUSEADDR rebinding after close
 * topic: networking
 * covers: SO_REUSEADDR, TIME_WAIT, EADDRINUSE with live listener, rebind same port, accept on rebound socket
 * deps: libc, posix, sockets
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
static int wait_fd(int fd, short ev, int ms) {
    struct pollfd p;
    p.fd = fd;
    p.events = ev;
    p.revents = 0;
    int r;
    do {
        r = poll(&p, 1, ms);
    } while (r < 0 && errno == EINTR);
    return r > 0;
}
static struct sockaddr_in local_addr(int fd) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    memset(&a, 0, sizeof a);
    check(getsockname(fd, (struct sockaddr *)&a, &l) == 0, "getsockname");
    check(l == sizeof a && a.sin_family == AF_INET, "sockname family");
    return a;
}
static int connect_to(struct sockaddr_in a) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    check(fd >= 0, "client socket");
    set_timeout(fd, 2000);
    check(connect(fd, (struct sockaddr *)&a, sizeof a) == 0, "connect");
    return fd;
}
static int accept_to(int ls) {
    check(wait_fd(ls, POLLIN, 2000), "accept ready");
    int c = accept(ls, NULL, NULL);
    check(c >= 0, "accept");
    set_timeout(c, 2000);
    return c;
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

static int reuse_listener(struct sockaddr_in *want, int *err) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    check(fd >= 0, "socket");
    int one = 1;
    check(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0, "SO_REUSEADDR");
    *err = 0;
    if (bind(fd, (struct sockaddr *)want, sizeof *want) != 0 || listen(fd, 4) != 0) {
        *err = errno;
        close(fd);
        return -1;
    }
    return fd;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int first_err;
    struct sockaddr_in any;
    memset(&any, 0, sizeof any);
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ls = reuse_listener(&any, &first_err);
    check(ls >= 0, "first listener");
    struct sockaddr_in addr = local_addr(ls);

    /* a second listener on the same live port is refused */
    int err;
    int dup = reuse_listener(&addr, &err);
    printf("second listener while first is live: %s\n", dup >= 0 ? "bound" : (err == EADDRINUSE ? "EADDRINUSE" : "other"));
    check(dup < 0 && err == EADDRINUSE, "EADDRINUSE");

    for (int round = 0; round < 3; round++) {
        int c = connect_to(addr);
        int s = accept_to(ls);
        char msg[32];
        int n = snprintf(msg, sizeof msg, "round %d", round);
        check(send_all(c, msg, (size_t)n) == 0, "send");
        char back[32];
        check(recv_all(s, back, (size_t)n) == (size_t)n && memcmp(back, msg, (size_t)n) == 0, "data");
        /* server side closes first, leaving TIME_WAIT state on the listening port */
        close(s);
        char eof;
        check(recv(c, &eof, 1, 0) == 0, "client sees EOF");
        close(c);
        /* tear down the listener and start over on the very same port */
        close(ls);
        ls = reuse_listener(&addr, &err);
        printf("round %d: rebind same port %s\n", round, ls >= 0 ? "succeeded" : "failed");
        check(ls >= 0, "rebind");
        struct sockaddr_in again = local_addr(ls);
        check(again.sin_port == addr.sin_port, "same port");
    }
    /* final proof: the new listener still accepts */
    int c = connect_to(addr);
    int s = accept_to(ls);
    check(send_all(c, "ok", 2) == 0, "final send");
    char b[2];
    check(recv_all(s, b, 2) == 2 && memcmp(b, "ok", 2) == 0, "final data");
    printf("rebound listener serves clients\n");
    close(c);
    close(s);
    close(ls);
    return 0;
}
