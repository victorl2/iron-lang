/*
 * title: Connected UDP sockets filter datagram senders
 * topic: networking
 * covers: connect on SOCK_DGRAM, send without address, sender filtering, reconnect to another peer, getpeername
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

static int try_recv_ms(int fd, char *buf, size_t cap, int ms) {
    if (!wait_fd(fd, POLLIN, ms))
        return -1;
    ssize_t r = recv(fd, buf, cap - 1, 0);
    if (r < 0)
        return -1;
    buf[r] = 0;
    return (int)r;
}

static int try_recv(int fd, char *buf, size_t cap) { return try_recv_ms(fd, buf, cap, 2000); }
static int try_none(int fd, char *buf, size_t cap) { return try_recv_ms(fd, buf, cap, 60); }

int main(void) {
    int hub = make_socket(SOCK_DGRAM, 0);
    int p1 = make_socket(SOCK_DGRAM, 0);
    int p2 = make_socket(SOCK_DGRAM, 0);
    struct sockaddr_in hub_a = local_addr(hub);
    struct sockaddr_in a1 = local_addr(p1);
    struct sockaddr_in a2 = local_addr(p2);
    set_timeout(hub, 2000);
    set_timeout(p1, 2000);
    set_timeout(p2, 2000);

    /* unconnected: the hub receives from anyone */
    check(sendto(p1, "from-1", 6, 0, (struct sockaddr *)&hub_a, sizeof hub_a) == 6, "p1 send");
    check(sendto(p2, "from-2", 6, 0, (struct sockaddr *)&hub_a, sizeof hub_a) == 6, "p2 send");
    char buf[32];
    int n1 = try_recv(hub, buf, sizeof buf);
    printf("unconnected first: %s\n", n1 > 0 ? buf : "nothing");
    check(n1 == 6 && strcmp(buf, "from-1") == 0, "unconnected first");
    n1 = try_recv(hub, buf, sizeof buf);
    printf("unconnected second: %s\n", n1 > 0 ? buf : "nothing");
    check(n1 == 6 && strcmp(buf, "from-2") == 0, "unconnected second");

    /* connect to p1: p2 is filtered out even though it sends first */
    check(connect(hub, (struct sockaddr *)&a1, sizeof a1) == 0, "connect hub to p1");
    check(sendto(p2, "intruder", 8, 0, (struct sockaddr *)&hub_a, sizeof hub_a) == 8, "p2 send filtered");
    check(sendto(p1, "friend", 6, 0, (struct sockaddr *)&hub_a, sizeof hub_a) == 6, "p1 send");
    int n = try_recv(hub, buf, sizeof buf);
    printf("connected to p1 receives: %s\n", n > 0 ? buf : "nothing");
    check(n == 6 && strcmp(buf, "friend") == 0, "only p1 accepted");
    n = try_none(hub, buf, sizeof buf);
    printf("second datagram pending: %s\n", n > 0 ? "yes" : "no");
    check(n < 0, "filtered datagram dropped");

    /* send() without an address goes to the connected peer */
    check(send(hub, "to-p1", 5, 0) == 5, "send connected");
    n = try_recv(p1, buf, sizeof buf);
    printf("p1 got: %s\n", n > 0 ? buf : "nothing");
    check(n == 5 && strcmp(buf, "to-p1") == 0, "p1 receives");
    n = try_none(p2, buf, sizeof buf);
    check(n < 0, "p2 receives nothing");
    printf("p2 got nothing: yes\n");

    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    check(getpeername(hub, (struct sockaddr *)&peer, &pl) == 0, "getpeername");
    printf("peer port is p1: %s\n", peer.sin_port == a1.sin_port ? "yes" : "no");
    check(peer.sin_port == a1.sin_port, "peer p1");

    /* reconnect to p2: now p1 is the one filtered out */
    check(connect(hub, (struct sockaddr *)&a2, sizeof a2) == 0, "reconnect to p2");
    check(sendto(p1, "old-friend", 10, 0, (struct sockaddr *)&hub_a, sizeof hub_a) == 10, "p1 filtered");
    check(sendto(p2, "new-friend", 10, 0, (struct sockaddr *)&hub_a, sizeof hub_a) == 10, "p2 send");
    n = try_recv(hub, buf, sizeof buf);
    printf("connected to p2 receives: %s\n", n > 0 ? buf : "nothing");
    check(n == 10 && strcmp(buf, "new-friend") == 0, "only p2 accepted");
    n = try_none(hub, buf, sizeof buf);
    check(n < 0, "p1 datagram dropped");
    printf("queue is empty afterwards: yes\n");
    close(hub);
    close(p1);
    close(p2);
    return 0;
}
