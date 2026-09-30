/*
 * title: Nonblocking accept and EAGAIN on an empty queue
 * topic: networking
 * covers: O_NONBLOCK listener, accept returning EAGAIN, poll readiness, draining the queue, pending count changes, client close
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
static int connect_to(struct sockaddr_in a) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    check(fd >= 0, "client socket");
    set_timeout(fd, 2000);
    check(connect(fd, (struct sockaddr *)&a, sizeof a) == 0, "connect");
    return fd;
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

static int would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK; }

/* accept all connections that are ready right now */
static int drain_accepts(int ls, int *out, int cap) {
    int n = 0;
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) {
            check(would_block(errno), "accept stops with EAGAIN");
            return n;
        }
        check(n < cap, "capacity");
        /* accepted sockets may inherit O_NONBLOCK on BSD systems, so set the mode explicitly */
        set_nonblock(c, 0);
        set_timeout(c, 2000);
        out[n++] = c;
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 8);
    struct sockaddr_in la = local_addr(ls);
    set_nonblock(ls, 1);

    int c = accept(ls, NULL, NULL);
    printf("accept with empty queue: %s\n", c < 0 && would_block(errno) ? "EAGAIN" : "unexpected");
    check(c < 0 && would_block(errno), "empty accept");
    printf("poll says listener readable: %s\n", wait_fd(ls, POLLIN, 20) ? "yes" : "no");
    check(!wait_fd(ls, POLLIN, 0), "not readable when empty");

    int clients[6];
    int accepted[8];
    int total_accepted = 0;
    for (int wave = 0; wave < 3; wave++) {
        int n = wave + 1;
        for (int i = 0; i < n; i++)
            clients[i] = connect_to(la);
        check(wait_fd(ls, POLLIN, 2000), "listener readable with pending clients");
        int got = drain_accepts(ls, accepted, 8);
        printf("wave %d: %d clients connected, accept loop got %d\n", wave, n, got);
        check(got == n, "accept count");
        total_accepted += got;
        /* each accepted socket is paired with the matching client through payload */
        for (int i = 0; i < n; i++) {
            char msg[8];
            int m = snprintf(msg, sizeof msg, "w%dc%d", wave, i);
            check(send_all(clients[i], msg, (size_t)m) == 0, "client send");
        }
        for (int i = 0; i < n; i++) {
            char msg[8], want[8];
            int m = snprintf(want, sizeof want, "w%dc%d", wave, i);
            check(recv_all(accepted[i], msg, (size_t)m) == (size_t)m && memcmp(msg, want, (size_t)m) == 0, "payload pairing");
        }
        for (int i = 0; i < n; i++) {
            close(clients[i]);
            close(accepted[i]);
        }
        c = accept(ls, NULL, NULL);
        check(c < 0 && would_block(errno), "queue empty again");
        printf("wave %d: queue empty again: yes\n", wave);
    }
    printf("total accepted %d\n", total_accepted);
    check(total_accepted == 6, "total");

    /* a client that connects and closes before accept still shows up in the queue */
    int gone = connect_to(la);
    close(gone);
    check(wait_fd(ls, POLLIN, 2000), "readable for closed client");
    int q[2];
    int n = drain_accepts(ls, q, 2);
    printf("client that already closed was accepted: %s\n", n == 1 ? "yes" : "no");
    check(n == 1, "closed client accepted");
    char b;
    check(recv(q[0], &b, 1, 0) == 0, "EOF from closed client");
    printf("accepted socket reads EOF immediately: yes\n");
    close(q[0]);
    close(ls);
    return 0;
}
