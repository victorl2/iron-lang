/*
 * title: Iterative TCP server serving queued clients in order
 * topic: networking
 * covers: listen backlog, accept order, one client at a time, request/response, sequence numbers
 * deps: libc, posix, pthread, sockets
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

enum { NCLIENTS = 5 };

typedef struct {
    int ls;
    int served;
    int order_ok;
} Server;

static void *server_main(void *arg) {
    Server *s = arg;
    for (int i = 0; i < NCLIENTS; i++) {
        int c = accept_to(s->ls);
        char req[64];
        ssize_t r = recv(c, req, sizeof req - 1, 0);
        if (r <= 0) {
            close(c);
            continue;
        }
        req[r] = 0;
        /* request is "id:<n>", reply says which position in the service order it got */
        int id = -1;
        if (sscanf(req, "id:%d", &id) != 1 || id != i)
            s->order_ok = 0;
        char rep[64];
        int n = snprintf(rep, sizeof rep, "served #%d for %s", i + 1, req);
        send_all(c, rep, (size_t)n);
        close(c);
        s->served++;
    }
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 8);
    struct sockaddr_in addr = local_addr(ls);
    int cs[NCLIENTS];
    /* all clients connect before the server thread starts: they wait in the backlog */
    for (int i = 0; i < NCLIENTS; i++)
        cs[i] = connect_to(addr);
    Server srv = {ls, 0, 1};
    pthread_t th;
    check(pthread_create(&th, NULL, server_main, &srv) == 0, "pthread_create");
    /* requests go out in reverse order of connection, service order still follows accept order */
    for (int i = NCLIENTS - 1; i >= 0; i--) {
        char req[32];
        int n = snprintf(req, sizeof req, "id:%d", i);
        check(send_all(cs[i], req, (size_t)n) == 0, "send request");
    }
    for (int i = 0; i < NCLIENTS; i++) {
        char rep[80];
        size_t n = recv_all(cs[i], rep, sizeof rep - 1);
        rep[n] = 0;
        printf("client %d got: %s\n", i, rep);
        close(cs[i]);
    }
    pthread_join(th, NULL);
    check(srv.served == NCLIENTS, "all served");
    check(srv.order_ok, "accept order equals connect order");
    printf("served %d clients, order preserved\n", srv.served);
    close(ls);
    return 0;
}
