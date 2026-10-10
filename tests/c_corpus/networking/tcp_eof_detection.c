/*
 * title: End of stream detection on several socket kinds
 * topic: networking
 * covers: recv returning zero, repeated EOF, POLLIN at EOF, buffered data before EOF, TCP unix and socketpair, close versus shutdown
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

/* drain everything up to EOF, returning the byte count and the number of recv calls that returned 0 */
static size_t drain(int fd, int *zero_reads) {
    size_t total = 0;
    unsigned char buf[16];
    *zero_reads = 0;
    for (int i = 0; i < 3; i++) {
        for (;;) {
            check(wait_fd(fd, POLLIN, 2000), "readable");
            ssize_t r = recv(fd, buf, sizeof buf, 0);
            check(r >= 0, "recv");
            if (r == 0) {
                (*zero_reads)++;
                break;
            }
            total += (size_t)r;
        }
    }
    return total;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int zr;

    /* TCP: data sent before close is still delivered, then EOF forever */
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(ls);
    int c = connect_to(la);
    int s = accept_to(ls);
    unsigned char blob[100];
    for (size_t i = 0; i < sizeof blob; i++)
        blob[i] = (unsigned char)(i * 3);
    check(send_all(s, blob, sizeof blob) == 0, "send");
    close(s);
    size_t n = drain(c, &zr);
    printf("tcp: %zu bytes then EOF, %d consecutive zero reads\n", n, zr);
    check(n == 100 && zr == 3, "tcp eof");
    close(c);
    close(ls);

    /* unix socketpair: close one end */
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    set_timeout(sv[0], 2000);
    check(send_all(sv[1], "abc", 3) == 0, "send");
    close(sv[1]);
    n = drain(sv[0], &zr);
    printf("socketpair: %zu bytes then EOF, %d zero reads\n", n, zr);
    check(n == 3 && zr == 3, "pair eof");
    close(sv[0]);

    /* shutdown(SHUT_WR) gives the peer EOF while the socket stays open in the other direction */
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 2");
    set_timeout(sv[0], 2000);
    set_timeout(sv[1], 2000);
    check(send_all(sv[0], "request", 7) == 0, "send request");
    check(shutdown(sv[0], SHUT_WR) == 0, "shutdown");
    n = drain(sv[1], &zr);
    printf("after SHUT_WR: peer read %zu bytes then %d EOFs\n", n, zr);
    check(send_all(sv[1], "response", 8) == 0, "reverse direction still open");
    char r8[8];
    check(recv_all(sv[0], r8, 8) == 8 && memcmp(r8, "response", 8) == 0, "response");
    printf("reverse direction still delivers data\n");
    close(sv[0]);
    close(sv[1]);

    /* an unix stream server: several clients each connect and close at different times */
    int total_eof = 0;
    ls = make_socket(SOCK_STREAM, 4);
    la = local_addr(ls);
    for (int i = 0; i < 4; i++) {
        c = connect_to(la);
        s = accept_to(ls);
        int len = i * 5;
        unsigned char tmp[32];
        memset(tmp, 'a' + i, sizeof tmp);
        if (len > 0)
            check(send_all(c, tmp, (size_t)len) == 0, "client send");
        close(c);
        n = drain(s, &zr);
        printf("client %d: server read %zu bytes then EOF\n", i, n);
        check(n == (size_t)len && zr == 3, "per-client eof");
        total_eof += zr;
        close(s);
    }
    printf("total EOF observations: %d\n", total_eof);
    close(ls);
    return 0;
}
