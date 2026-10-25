/*
 * title: TCP half-close with shutdown SHUT_WR
 * topic: networking
 * covers: shutdown SHUT_WR, EOF as end of request, server drains then replies, EPIPE after own shutdown, checksum
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
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 16);
}
static uint32_t fnv1a(const unsigned char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
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

typedef struct {
    int ls;
    size_t received;
    uint32_t hash;
} Server;

/* reads the whole request until the client half-closes, then reports a summary */
static void *server_main(void *arg) {
    Server *s = arg;
    int c = accept_to(s->ls);
    unsigned char buf[300];
    unsigned char all[4096];
    size_t total = 0;
    for (;;) {
        ssize_t r = recv(c, buf, sizeof buf, 0);
        if (r <= 0)
            break;
        if (total + (size_t)r <= sizeof all)
            memcpy(all + total, buf, (size_t)r);
        total += (size_t)r;
    }
    s->received = total;
    s->hash = fnv1a(all, total);
    char rep[64];
    int n = snprintf(rep, sizeof rep, "got %zu bytes hash %08x", total, (unsigned)s->hash);
    send_all(c, rep, (size_t)n);
    close(c);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in addr = local_addr(ls);
    Server srv = {ls, 0, 0};
    pthread_t th;
    check(pthread_create(&th, NULL, server_main, &srv) == 0, "thread");

    int c = connect_to(addr);
    unsigned char req[1500];
    for (size_t i = 0; i < sizeof req; i++)
        req[i] = (unsigned char)rnd();
    /* send in uneven pieces */
    size_t off = 0, piece = 1;
    while (off < sizeof req) {
        size_t n = piece;
        if (n > sizeof req - off)
            n = sizeof req - off;
        check(send_all(c, req + off, n) == 0, "send piece");
        off += n;
        piece = piece * 3 + 1;
    }
    /* without a half-close the server would wait forever: it has not seen EOF yet */
    check(shutdown(c, SHUT_WR) == 0, "shutdown");
    /* our sending direction is closed */
    ssize_t w = send(c, "x", 1, 0);
    int e = errno;
    printf("send after SHUT_WR: %s\n", w < 0 && e == EPIPE ? "EPIPE" : "unexpected");
    check(w < 0 && e == EPIPE, "EPIPE");

    /* but we can still read the reply, then see EOF */
    char rep[80];
    size_t n = recv_all(c, rep, sizeof rep - 1);
    rep[n] = 0;
    printf("reply: %s\n", rep);
    char extra;
    ssize_t r = recv(c, &extra, 1, 0);
    printf("after reply recv returns %zd (EOF)\n", r);
    check(r == 0, "EOF after reply");
    pthread_join(th, NULL);

    check(srv.received == sizeof req, "server saw all bytes");
    check(srv.hash == fnv1a(req, sizeof req), "hash matches");
    char want[64];
    int wn = snprintf(want, sizeof want, "got %zu bytes hash %08x", sizeof req, (unsigned)fnv1a(req, sizeof req));
    check((size_t)wn == n && memcmp(want, rep, n) == 0, "reply text");
    close(c);
    close(ls);
    return 0;
}
