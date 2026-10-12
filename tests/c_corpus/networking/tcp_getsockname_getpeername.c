/*
 * title: getsockname and getpeername consistency
 * topic: networking
 * covers: getsockname, getpeername, ENOTCONN, unbound sockets, port pairing, address family checks
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

static struct sockaddr_in peer_of(int fd, int *err) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    memset(&a, 0, sizeof a);
    *err = 0;
    if (getpeername(fd, (struct sockaddr *)&a, &l) != 0)
        *err = errno;
    return a;
}

static const char *yesno(int v) { return v ? "yes" : "no"; }

int main(void) {
    /* an unbound socket reports the wildcard address and port zero */
    int fresh = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in u = local_addr(fresh);
    printf("unbound: family AF_INET %s, wildcard addr %s, port zero %s\n", yesno(u.sin_family == AF_INET),
           yesno(u.sin_addr.s_addr == htonl(INADDR_ANY)), yesno(u.sin_port == 0));
    int err;
    peer_of(fresh, &err);
    printf("getpeername on unconnected: %s\n", err == ENOTCONN ? "ENOTCONN" : "other");
    check(err == ENOTCONN, "ENOTCONN");
    close(fresh);

    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(ls);
    printf("listener: loopback %s, port assigned %s\n", yesno(la.sin_addr.s_addr == htonl(INADDR_LOOPBACK)),
           yesno(la.sin_port != 0));
    int c = connect_to(la);
    int s = accept_to(ls);

    struct sockaddr_in c_local = local_addr(c);
    struct sockaddr_in c_peer = peer_of(c, &err);
    check(err == 0, "client getpeername");
    struct sockaddr_in s_local = local_addr(s);
    struct sockaddr_in s_peer = peer_of(s, &err);
    check(err == 0, "server getpeername");

    printf("client peer port == listener port: %s\n", yesno(c_peer.sin_port == la.sin_port));
    printf("server-side local port == listener port: %s\n", yesno(s_local.sin_port == la.sin_port));
    printf("client local port == server-side peer port: %s\n", yesno(c_local.sin_port == s_peer.sin_port));
    printf("client ephemeral port differs from listener: %s\n", yesno(c_local.sin_port != la.sin_port));
    printf("all four addresses are loopback: %s\n",
           yesno(c_local.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && c_peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
              s_local.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && s_peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK)));
    check(c_peer.sin_port == la.sin_port && s_local.sin_port == la.sin_port, "server port");
    check(c_local.sin_port == s_peer.sin_port && c_local.sin_port != la.sin_port, "client port");

    /* a connected UDP socket reports its peer as well */
    int r = make_socket(SOCK_DGRAM, 0);
    int t = make_socket(SOCK_DGRAM, 0);
    struct sockaddr_in ra = local_addr(r);
    check(connect(t, (struct sockaddr *)&ra, sizeof ra) == 0, "udp connect");
    struct sockaddr_in tp = peer_of(t, &err);
    check(err == 0, "udp getpeername");
    printf("udp peer port matches receiver: %s\n", yesno(tp.sin_port == ra.sin_port));
    check(tp.sin_port == ra.sin_port, "udp peer");
    peer_of(r, &err);
    printf("unconnected udp receiver has a peer: %s\n", yesno(err == 0));
    check(err == ENOTCONN, "udp receiver ENOTCONN");

    close(r);
    close(t);
    close(c);
    close(s);
    close(ls);
    return 0;
}
