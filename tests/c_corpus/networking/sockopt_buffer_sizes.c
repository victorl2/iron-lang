/*
 * title: SO_RCVBUF and SO_SNDBUF sizing
 * topic: networking
 * covers: setsockopt, getsockopt, buffer size lower bounds, TCP UDP and AF_UNIX sockets, bulk transfer with resized buffers
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

/* kernels may round or double the request, so only "at least what we asked" is portable */
static int get_opt(int fd, int opt) {
    int v = -1;
    socklen_t l = sizeof v;
    check(getsockopt(fd, SOL_SOCKET, opt, &v, &l) == 0, "getsockopt");
    return v;
}

static void try_sizes(const char *label, int fd) {
    static const int wanted[] = {8192, 32768, 65536, 131072};
    int rcv_ok = 1, snd_ok = 1, positive = 1;
    check(get_opt(fd, SO_RCVBUF) > 0 && get_opt(fd, SO_SNDBUF) > 0, "default sizes positive");
    for (int i = 0; i < 4; i++) {
        int w = wanted[i];
        check(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &w, sizeof w) == 0, "set SO_RCVBUF");
        check(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &w, sizeof w) == 0, "set SO_SNDBUF");
        int r = get_opt(fd, SO_RCVBUF);
        int s = get_opt(fd, SO_SNDBUF);
        if (r < w)
            rcv_ok = 0;
        if (s < w)
            snd_ok = 0;
        if (r <= 0 || s <= 0)
            positive = 0;
    }
    printf("%s: rcvbuf>=requested %s, sndbuf>=requested %s, positive %s\n", label, rcv_ok ? "yes" : "no",
           snd_ok ? "yes" : "no", positive ? "yes" : "no");
    check(rcv_ok && snd_ok && positive, label);
}

typedef struct {
    int fd;
    size_t total;
    uint32_t hash;
} Reader;

static void *reader_main(void *arg) {
    Reader *r = arg;
    unsigned char buf[4096];
    uint32_t h = 2166136261u;
    for (;;) {
        ssize_t n = recv(r->fd, buf, sizeof buf, 0);
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 16777619u;
        }
        r->total += (size_t)n;
    }
    r->hash = h;
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int t = socket(AF_INET, SOCK_STREAM, 0);
    try_sizes("tcp", t);
    close(t);
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    try_sizes("udp", u);
    close(u);
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    try_sizes("unix", sv[0]);
    close(sv[0]);
    close(sv[1]);

    /* move 600 KB through a connection whose buffers are set small */
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in addr = local_addr(ls);
    int small = 8192;
    check(setsockopt(ls, SOL_SOCKET, SO_RCVBUF, &small, sizeof small) == 0, "listener rcvbuf");
    int c = connect_to(addr);
    int s = accept_to(ls);
    check(setsockopt(c, SOL_SOCKET, SO_SNDBUF, &small, sizeof small) == 0, "client sndbuf");
    Reader rd = {s, 0, 0};
    pthread_t th;
    check(pthread_create(&th, NULL, reader_main, &rd) == 0, "thread");
    size_t total = 600000;
    uint32_t h = 2166136261u;
    unsigned char chunk[1000];
    size_t sent = 0;
    while (sent < total) {
        size_t n = total - sent < sizeof chunk ? total - sent : sizeof chunk;
        for (size_t i = 0; i < n; i++) {
            chunk[i] = (unsigned char)((sent + i) * 2654435761u >> 13);
            h ^= chunk[i];
            h *= 16777619u;
        }
        check(send_all(c, chunk, n) == 0, "send chunk");
        sent += n;
    }
    close(c);
    pthread_join(th, NULL);
    printf("transferred %zu bytes, checksum %s\n", rd.total, rd.hash == h ? "matches" : "differs");
    check(rd.total == total && rd.hash == h, "transfer");
    printf("fnv %08x\n", (unsigned)h);
    close(s);
    close(ls);
    return 0;
}
