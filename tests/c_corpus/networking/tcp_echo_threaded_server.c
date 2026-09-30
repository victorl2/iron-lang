/*
 * title: TCP echo server thread with a client
 * topic: networking
 * covers: socket, bind, listen, accept, connect, send, recv, pthread server, byte accounting
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
    long bytes;
    int accepted;
} Server;

static void *server_main(void *arg) {
    Server *s = arg;
    int c = accept_to(s->ls);
    s->accepted = 1;
    unsigned char buf[512];
    for (;;) {
        ssize_t r = recv(c, buf, sizeof buf, 0);
        if (r <= 0)
            break;
        if (send_all(c, buf, (size_t)r) < 0)
            break;
        s->bytes += r;
    }
    close(c);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 4);
    struct sockaddr_in addr = local_addr(ls);
    Server srv = {ls, 0, 0};
    pthread_t th;
    check(pthread_create(&th, NULL, server_main, &srv) == 0, "pthread_create");

    int c = connect_to(addr);
    static const char *texts[] = {"hello", "x", "The quick brown fox jumps over the lazy dog",
                                  "line one\nline two\n", "\x01\x02\x03\xff\xfe"};
    long total = 0;
    for (int i = 0; i < 5; i++) {
        size_t n = strlen(texts[i]);
        unsigned char echo[128];
        check(send_all(c, texts[i], n) == 0, "send text");
        check(recv_all(c, echo, n) == n, "echo length");
        check(memcmp(echo, texts[i], n) == 0, "echo content");
        printf("text %d: %zu bytes echoed, fnv=%08x\n", i, n, (unsigned)fnv1a(echo, n));
        total += (long)n;
    }
    /* random binary blobs of growing size */
    for (int i = 0; i < 6; i++) {
        size_t n = 1u << (i + 3);
        unsigned char out[512], in[512];
        for (size_t k = 0; k < n; k++)
            out[k] = (unsigned char)rnd();
        check(send_all(c, out, n) == 0, "send blob");
        check(recv_all(c, in, n) == n, "blob length");
        check(memcmp(in, out, n) == 0, "blob content");
        printf("blob %zu bytes: fnv=%08x\n", n, (unsigned)fnv1a(in, n));
        total += (long)n;
    }
    close(c);
    pthread_join(th, NULL);
    check(srv.accepted == 1, "server accepted");
    check(srv.bytes == total, "server byte count");
    printf("server echoed %ld bytes in total\n", srv.bytes);
    close(ls);
    return 0;
}
