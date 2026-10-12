/*
 * title: TCP_NODELAY, SO_KEEPALIVE, SO_LINGER and friends
 * topic: networking
 * covers: getsockopt, setsockopt, boolean options, SO_LINGER struct, SO_TYPE
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

static int get_int(int fd, int level, int opt) {
    int v = -12345;
    socklen_t l = sizeof v;
    check(getsockopt(fd, level, opt, &v, &l) == 0, "getsockopt");
    return v;
}

static void set_int(int fd, int level, int opt, int v) {
    check(setsockopt(fd, level, opt, &v, sizeof v) == 0, "setsockopt");
}

static const char *flag(int v) { return v ? "on" : "off"; }

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in addr = local_addr(ls);
    int c = connect_to(addr);
    int s = accept_to(ls);

    printf("client SO_TYPE is stream: %s\n", get_int(c, SOL_SOCKET, SO_TYPE) == SOCK_STREAM ? "yes" : "no");

    printf("TCP_NODELAY default: %s\n", flag(get_int(c, IPPROTO_TCP, TCP_NODELAY)));
    set_int(c, IPPROTO_TCP, TCP_NODELAY, 1);
    printf("TCP_NODELAY after set: %s\n", flag(get_int(c, IPPROTO_TCP, TCP_NODELAY)));
    check(get_int(c, IPPROTO_TCP, TCP_NODELAY) != 0, "nodelay on");
    set_int(c, IPPROTO_TCP, TCP_NODELAY, 0);
    printf("TCP_NODELAY after clear: %s\n", flag(get_int(c, IPPROTO_TCP, TCP_NODELAY)));
    check(get_int(c, IPPROTO_TCP, TCP_NODELAY) == 0, "nodelay off");
    set_int(s, IPPROTO_TCP, TCP_NODELAY, 1);
    printf("server-side TCP_NODELAY: %s\n", flag(get_int(s, IPPROTO_TCP, TCP_NODELAY)));

    printf("SO_KEEPALIVE default: %s\n", flag(get_int(c, SOL_SOCKET, SO_KEEPALIVE)));
    set_int(c, SOL_SOCKET, SO_KEEPALIVE, 1);
    check(get_int(c, SOL_SOCKET, SO_KEEPALIVE) != 0, "keepalive on");
    printf("SO_KEEPALIVE after set: %s\n", flag(get_int(c, SOL_SOCKET, SO_KEEPALIVE)));
    set_int(c, SOL_SOCKET, SO_KEEPALIVE, 0);
    check(get_int(c, SOL_SOCKET, SO_KEEPALIVE) == 0, "keepalive off");
    printf("SO_KEEPALIVE after clear: %s\n", flag(get_int(c, SOL_SOCKET, SO_KEEPALIVE)));

    printf("SO_REUSEADDR default: %s\n", flag(get_int(c, SOL_SOCKET, SO_REUSEADDR)));
    set_int(c, SOL_SOCKET, SO_REUSEADDR, 1);
    printf("SO_REUSEADDR after set: %s\n", flag(get_int(c, SOL_SOCKET, SO_REUSEADDR)));
    check(get_int(c, SOL_SOCKET, SO_REUSEADDR) != 0, "reuse on");

    struct linger lg;
    socklen_t ll = sizeof lg;
    check(getsockopt(c, SOL_SOCKET, SO_LINGER, &lg, &ll) == 0, "get linger");
    printf("SO_LINGER default: %s\n", flag(lg.l_onoff));
    check(lg.l_onoff == 0, "linger default off");
    lg.l_onoff = 1;
    lg.l_linger = 3;
    check(setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof lg) == 0, "set linger");
    memset(&lg, 0, sizeof lg);
    ll = sizeof lg;
    check(getsockopt(c, SOL_SOCKET, SO_LINGER, &lg, &ll) == 0, "get linger again");
    printf("SO_LINGER after set: %s, seconds nonzero %s\n", flag(lg.l_onoff), lg.l_linger > 0 ? "yes" : "no");
    check(lg.l_onoff != 0 && lg.l_linger > 0, "linger set");
    lg.l_onoff = 0;
    check(setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof lg) == 0, "clear linger");

    /* the options do not disturb data transfer */
    set_int(c, IPPROTO_TCP, TCP_NODELAY, 1);
    check(send_all(c, "tiny", 4) == 0, "send");
    char b[4];
    check(wait_fd(s, POLLIN, 2000), "readable");
    check(recv(s, b, 4, MSG_WAITALL) == 4 && memcmp(b, "tiny", 4) == 0, "data");
    printf("data delivered with options applied\n");
    close(c);
    close(s);
    close(ls);
    return 0;
}
