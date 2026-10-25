/*
 * title: Nonblocking connect completed with poll and SO_ERROR
 * topic: networking
 * covers: O_NONBLOCK, EINPROGRESS, poll POLLOUT, SO_ERROR, ECONNREFUSED, restoring blocking mode
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

static void set_nonblock(int fd, int on) {
    int fl = fcntl(fd, F_GETFL, 0);
    check(fl >= 0, "F_GETFL");
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    check(fcntl(fd, F_SETFL, fl) == 0, "F_SETFL");
}

/* returns 0 on success or the errno reported for the connection attempt */
static int connect_nb(struct sockaddr_in addr, int *out_fd) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    check(fd >= 0, "socket");
    set_nonblock(fd, 1);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
    int err = 0;
    if (rc != 0) {
        int e = errno;
        if (e != EINPROGRESS) {
            err = e;
        } else {
            check(wait_fd(fd, POLLOUT, 2000), "connect completes");
            socklen_t l = sizeof err;
            check(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) == 0, "SO_ERROR");
        }
    }
    *out_fd = fd;
    return err;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 4);
    struct sockaddr_in addr = local_addr(ls);

    int fd;
    int err = connect_nb(addr, &fd);
    printf("connect to listener: %s\n", err == 0 ? "connected" : "failed");
    check(err == 0, "connect ok");
    int srv = accept_to(ls);
    set_nonblock(fd, 0);
    set_timeout(fd, 2000);
    check(send_all(fd, "hi", 2) == 0, "send after connect");
    char b[4];
    check(recv_all(srv, b, 2) == 2 && memcmp(b, "hi", 2) == 0, "data flows");
    printf("data flows on the nonblocking-connected socket\n");
    close(fd);
    close(srv);

    /* a port that was bound and closed refuses connections */
    int tmp = make_socket(SOCK_STREAM, 1);
    struct sockaddr_in dead = local_addr(tmp);
    close(tmp);
    err = connect_nb(dead, &fd);
    printf("connect to closed port: %s\n", err == ECONNREFUSED ? "ECONNREFUSED" : (err == 0 ? "connected" : "other"));
    check(err == ECONNREFUSED, "refused");
    close(fd);

    /* SO_ERROR is cleared by reading it */
    int fd2;
    err = connect_nb(addr, &fd2);
    check(err == 0, "second connect");
    int again = -1;
    socklen_t l = sizeof again;
    check(getsockopt(fd2, SOL_SOCKET, SO_ERROR, &again, &l) == 0 && again == 0, "SO_ERROR clear");
    printf("SO_ERROR after success: 0\n");
    int srv2 = accept_to(ls);
    close(srv2);
    close(fd2);
    close(ls);
    return 0;
}
