/*
 * title: Backlog of pending connections drained by an accept loop
 * topic: networking
 * covers: listen backlog, connections queued before accept, accept loop until no more pending, FIFO order, peer identification
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

enum { NCLIENTS = 12 };

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 16);
    struct sockaddr_in addr = local_addr(ls);
    int cs[NCLIENTS];
    struct sockaddr_in cports[NCLIENTS];
    /* every client connects and announces its id; nobody has been accepted yet */
    for (int i = 0; i < NCLIENTS; i++) {
        cs[i] = connect_to(addr);
        cports[i] = local_addr(cs[i]);
        unsigned char id = (unsigned char)(i * 7 + 3);
        check(send_all(cs[i], &id, 1) == 0, "announce id");
    }
    int accepted = 0;
    int order_ok = 1, peer_ok = 1;
    int sum = 0;
    int sfd[NCLIENTS];
    while (wait_fd(ls, POLLIN, 300)) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int c = accept(ls, (struct sockaddr *)&peer, &pl);
        check(c >= 0, "accept");
        check(accepted < NCLIENTS, "no more than NCLIENTS");
        set_timeout(c, 2000);
        unsigned char id;
        check(recv_all(c, &id, 1) == 1, "read id");
        if (id != (unsigned char)(accepted * 7 + 3))
            order_ok = 0;
        if (peer.sin_port != cports[accepted].sin_port)
            peer_ok = 0;
        sum += id;
        sfd[accepted] = c;
        accepted++;
    }
    printf("accepted %d pending connections\n", accepted);
    printf("accept order equals connect order: %s\n", order_ok ? "yes" : "no");
    printf("peer address matches client local address: %s\n", peer_ok ? "yes" : "no");
    printf("sum of announced ids: %d\n", sum);
    check(accepted == NCLIENTS && order_ok && peer_ok, "accept loop");

    /* answer every client with its accept index, in reverse */
    for (int i = NCLIENTS - 1; i >= 0; i--) {
        unsigned char reply = (unsigned char)(100 + i);
        check(send_all(sfd[i], &reply, 1) == 0, "reply");
    }
    int reply_sum = 0;
    for (int i = 0; i < NCLIENTS; i++) {
        unsigned char r;
        check(recv_all(cs[i], &r, 1) == 1 && r == 100 + i, "client reply");
        reply_sum += r;
    }
    printf("sum of replies: %d\n", reply_sum);
    for (int i = 0; i < NCLIENTS; i++) {
        close(cs[i]);
        close(sfd[i]);
    }
    close(ls);
    return 0;
}
