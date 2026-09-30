/*
 * title: UDP echo with sendto and recvfrom
 * topic: networking
 * covers: SOCK_DGRAM, sendto, recvfrom, source address, empty datagram, truncation, poll
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

static void reverse_into(unsigned char *dst, const unsigned char *src, size_t n) {
    for (size_t i = 0; i < n; i++)
        dst[i] = src[n - 1 - i];
}

int main(void) {
    int srv = make_socket(SOCK_DGRAM, 0);
    int cli = make_socket(SOCK_DGRAM, 0);
    struct sockaddr_in saddr = local_addr(srv);
    struct sockaddr_in caddr = local_addr(cli);
    set_timeout(srv, 2000);
    set_timeout(cli, 2000);
    check(saddr.sin_port != caddr.sin_port, "distinct ports");

    static const char *msgs[] = {"ping", "a longer datagram payload", "", "z", "0123456789"};
    for (int i = 0; i < 5; i++) {
        size_t n = strlen(msgs[i]);
        check(sendto(cli, msgs[i], n, 0, (struct sockaddr *)&saddr, sizeof saddr) == (ssize_t)n, "client sendto");
        /* server side: receive, learn the sender, reply reversed */
        unsigned char buf[128];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        check(wait_fd(srv, POLLIN, 2000), "server readable");
        ssize_t r = recvfrom(srv, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        check(r == (ssize_t)n, "server datagram length");
        check(from.sin_family == AF_INET && from.sin_port == caddr.sin_port, "sender identified");
        check(from.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "sender is loopback");
        unsigned char rev[128];
        reverse_into(rev, buf, (size_t)r);
        check(sendto(srv, rev, (size_t)r, 0, (struct sockaddr *)&from, fl) == r, "server sendto");
        /* client side */
        unsigned char back[128];
        struct sockaddr_in src;
        socklen_t sl = sizeof src;
        check(wait_fd(cli, POLLIN, 2000), "client readable");
        ssize_t b = recvfrom(cli, back, sizeof back, 0, (struct sockaddr *)&src, &sl);
        check(b == r, "reply length");
        check(src.sin_port == saddr.sin_port, "reply comes from server port");
        back[b] = 0;
        printf("datagram %d: sent %zu bytes, reply '%s' (%zd bytes)\n", i, n, (const char *)back, b);
    }
    /* a datagram larger than the receive buffer is truncated, the rest is discarded */
    const char *big = "ABCDEFGHIJKLMNOP";
    check(sendto(cli, big, 16, 0, (struct sockaddr *)&saddr, sizeof saddr) == 16, "send big");
    check(sendto(cli, "next", 4, 0, (struct sockaddr *)&saddr, sizeof saddr) == 4, "send next");
    char small[4];
    check(wait_fd(srv, POLLIN, 2000), "readable");
    ssize_t r = recvfrom(srv, small, sizeof small, 0, NULL, NULL);
    check(r == 4 && memcmp(small, "ABCD", 4) == 0, "truncated to 4");
    char nxt[16];
    r = recvfrom(srv, nxt, sizeof nxt, 0, NULL, NULL);
    check(r == 4 && memcmp(nxt, "next", 4) == 0, "next datagram intact");
    printf("truncated read kept 4 bytes, following datagram intact\n");
    /* nothing left: a receive with a short timeout fails with EAGAIN/EWOULDBLOCK */
    check(!wait_fd(srv, POLLIN, 30), "queue empty");
    close(srv);
    close(cli);
    return 0;
}
