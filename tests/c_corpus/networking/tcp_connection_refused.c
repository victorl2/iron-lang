/*
 * title: Connection refused on closed ports
 * topic: networking
 * covers: ECONNREFUSED, bind then close then connect, repeated attempts, connected UDP ICMP error, EAGAIN on idle
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

static const char *ename(int e) {
    if (e == ECONNREFUSED)
        return "ECONNREFUSED";
    if (e == 0)
        return "none";
    return "other";
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < 4; i++) {
        int tmp = make_socket(SOCK_STREAM, 1);
        struct sockaddr_in dead = local_addr(tmp);
        close(tmp);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        set_timeout(fd, 2000);
        int rc = connect(fd, (struct sockaddr *)&dead, sizeof dead);
        int e = rc == 0 ? 0 : errno;
        printf("attempt %d: %s\n", i, ename(e));
        check(rc != 0 && e == ECONNREFUSED, "refused");
        close(fd);
    }

    /* a socket that is bound and then starts listening accepts connections at once */
    int b = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(b);
    int fd = connect_to(la);
    int s = accept_to(b);
    printf("live listener: connected\n");
    close(fd);
    close(s);
    close(b);

    /* connected UDP: a datagram to a closed port reports ECONNREFUSED on the next receive */
    int probe = make_socket(SOCK_DGRAM, 0);
    struct sockaddr_in dead = local_addr(probe);
    close(probe);
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    set_timeout(u, 2000);
    check(connect(u, (struct sockaddr *)&dead, sizeof dead) == 0, "udp connect");
    check(send(u, "anyone?", 7, 0) == 7, "udp send");
    check(wait_fd(u, POLLIN | POLLERR, 2000), "error becomes readable");
    char buf[16];
    ssize_t r = recv(u, buf, sizeof buf, 0);
    int e = r < 0 ? errno : 0;
    printf("udp to closed port: %s\n", r < 0 ? ename(e) : "data");
    check(r < 0 && e == ECONNREFUSED, "udp refused");
    close(u);
    return 0;
}
