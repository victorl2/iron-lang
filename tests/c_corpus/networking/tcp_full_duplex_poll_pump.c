/*
 * title: Full duplex transfer pumped by one poll loop
 * topic: networking
 * covers: nonblocking sockets, poll POLLIN and POLLOUT, partial send and recv, simultaneous bidirectional streams, avoiding write-write deadlock
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
static int accept_to(int ls) {
    check(wait_fd(ls, POLLIN, 2000), "accept ready");
    int c = accept(ls, NULL, NULL);
    check(c >= 0, "accept");
    set_timeout(c, 2000);
    return c;
}

enum { AMOUNT = 300000 };

typedef struct {
    int fd;
    size_t sent, got;
    uint32_t out_hash, in_hash;
    unsigned char (*gen)(size_t);
    unsigned wouldblock_send;
} End;

static unsigned char gen_a(size_t i) { return (unsigned char)((i * 2654435761u) >> 11); }
static unsigned char gen_b(size_t i) { return (unsigned char)((i * 40503u + 17u) ^ (i >> 3)); }

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    check(fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0, "nonblock");
}

static uint32_t step(uint32_t h, unsigned char b) {
    h ^= b;
    return h * 16777619u;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(ls);
    int c = connect_to(la);
    int s = accept_to(ls);
    int small = 16384;
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    set_nonblock(c);
    set_nonblock(s);

    End e[2];
    memset(e, 0, sizeof e);
    e[0].fd = c;
    e[0].gen = gen_a;
    e[1].fd = s;
    e[1].gen = gen_b;
    e[0].out_hash = e[1].out_hash = e[0].in_hash = e[1].in_hash = 2166136261u;
    int wshut[2] = {0, 0};
    unsigned iterations = 0;

    /* both peers write at once and only read when the pipe is full: one thread pumps them both */
    while (e[0].got < AMOUNT || e[1].got < AMOUNT) {
        check(++iterations < 1000000, "loop bound");
        struct pollfd p[2];
        for (int i = 0; i < 2; i++) {
            p[i].fd = e[i].fd;
            p[i].events = 0;
            if (e[i].got < AMOUNT)
                p[i].events |= POLLIN;
            if (e[i].sent < AMOUNT)
                p[i].events |= POLLOUT;
            p[i].revents = 0;
        }
        int r = poll(p, 2, 3000);
        check(r > 0, "poll progress");
        for (int i = 0; i < 2; i++) {
            End *me = &e[i];
            End *peer = &e[1 - i];
            if (e[i].got < AMOUNT && (p[i].revents & (POLLIN | POLLHUP))) {
                unsigned char buf[3000];
                ssize_t n = recv(me->fd, buf, sizeof buf, 0);
                if (n < 0) {
                    check(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR, "recv error");
                } else if (n == 0) {
                    check(me->got == AMOUNT, "EOF only after all data");
                } else {
                    for (ssize_t k = 0; k < n; k++) {
                        check(buf[k] == peer->gen(me->got + (size_t)k), "stream content");
                        me->in_hash = step(me->in_hash, buf[k]);
                    }
                    me->got += (size_t)n;
                }
            }
            if ((p[i].revents & POLLOUT) && me->sent < AMOUNT) {
                unsigned char buf[4096];
                size_t want = AMOUNT - me->sent < sizeof buf ? AMOUNT - me->sent : sizeof buf;
                for (size_t k = 0; k < want; k++)
                    buf[k] = me->gen(me->sent + k);
                ssize_t n = send(me->fd, buf, want, 0);
                if (n < 0) {
                    check(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR, "send error");
                    me->wouldblock_send++;
                } else {
                    for (ssize_t k = 0; k < n; k++)
                        me->out_hash = step(me->out_hash, buf[k]);
                    me->sent += (size_t)n;
                }
            }
            if (me->sent >= AMOUNT && !wshut[i]) {
                shutdown(me->fd, SHUT_WR);
                wshut[i] = 1;
            }
        }
    }
    printf("side A sent %zu received %zu\n", e[0].sent, e[0].got);
    printf("side B sent %zu received %zu\n", e[1].sent, e[1].got);
    check(e[0].out_hash == e[1].in_hash && e[1].out_hash == e[0].in_hash, "hashes cross-match");
    printf("A->B hash %08x, B->A hash %08x\n", (unsigned)e[0].out_hash, (unsigned)e[1].out_hash);
    printf("streams verified against generators while in flight: yes\n");
    close(c);
    close(s);
    close(ls);
    return 0;
}
