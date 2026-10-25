/*
 * title: Concurrent TCP server with a worker thread per connection
 * topic: networking
 * covers: thread per connection, out-of-order replies, per-worker state, join, string transform
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

enum { NCONN = 6 };

typedef struct {
    int fd;
    int index;
    long in_bytes;
    int status;
} Worker;

/* worker reads one request to EOF-of-request (newline), replies with reversed uppercase text */
static void *worker_main(void *arg) {
    Worker *w = arg;
    char buf[128];
    size_t len = 0;
    while (len < sizeof buf) {
        ssize_t r = recv(w->fd, buf + len, sizeof buf - len, 0);
        if (r <= 0)
            break;
        len += (size_t)r;
        if (buf[len - 1] == '\n')
            break;
    }
    if (len == 0 || buf[len - 1] != '\n') {
        w->status = -1;
        close(w->fd);
        return NULL;
    }
    len--;
    char out[130];
    for (size_t i = 0; i < len; i++) {
        char ch = buf[len - 1 - i];
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');
        out[i] = ch;
    }
    out[len] = '\n';
    w->in_bytes = (long)len + 1;
    w->status = send_all(w->fd, out, len + 1);
    close(w->fd);
    return NULL;
}

typedef struct {
    int ls;
    Worker workers[NCONN];
    pthread_t th[NCONN];
} Server;

static void *acceptor_main(void *arg) {
    Server *s = arg;
    for (int i = 0; i < NCONN; i++) {
        s->workers[i].fd = accept_to(s->ls);
        s->workers[i].index = i;
        if (pthread_create(&s->th[i], NULL, worker_main, &s->workers[i]) != 0)
            exit(1);
    }
    for (int i = 0; i < NCONN; i++)
        pthread_join(s->th[i], NULL);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    static Server srv;
    srv.ls = make_socket(SOCK_STREAM, 8);
    struct sockaddr_in addr = local_addr(srv.ls);
    pthread_t acceptor;
    check(pthread_create(&acceptor, NULL, acceptor_main, &srv) == 0, "acceptor");

    static const char *words[NCONN] = {"alpha", "bravo charlie", "delta", "echo-foxtrot", "golf 123", "hotel"};
    int cs[NCONN];
    for (int i = 0; i < NCONN; i++)
        cs[i] = connect_to(addr);
    for (int i = 0; i < NCONN; i++) {
        char req[64];
        int n = snprintf(req, sizeof req, "%s\n", words[i]);
        check(send_all(cs[i], req, (size_t)n) == 0, "send");
    }
    /* collect replies in reverse order: workers run independently of each other */
    for (int i = NCONN - 1; i >= 0; i--) {
        char rep[80];
        size_t n = recv_all(cs[i], rep, sizeof rep - 1);
        check(n == strlen(words[i]) + 1, "reply length");
        rep[n - 1] = 0;
        printf("conn %d: %s -> %s\n", i, words[i], rep);
        close(cs[i]);
    }
    pthread_join(acceptor, NULL);
    long total = 0;
    for (int i = 0; i < NCONN; i++) {
        check(srv.workers[i].status == 0, "worker status");
        total += srv.workers[i].in_bytes;
    }
    printf("%d workers handled %ld request bytes\n", NCONN, total);
    close(srv.ls);
    return 0;
}
