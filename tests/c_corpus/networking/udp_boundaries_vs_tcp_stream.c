/*
 * title: UDP message boundaries versus TCP stream coalescing
 * topic: networking
 * covers: datagram boundaries, byte stream semantics, MSG_WAITALL chunking, recv sizes, framing necessity
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

static const char *parts[] = {"aa", "bbbb", "c", "dddddd", "eee", "ffffffff", "g"};
enum { NPARTS = 7 };

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    size_t total = 0;
    for (int i = 0; i < NPARTS; i++)
        total += strlen(parts[i]);

    /* UDP: every send is one receive, sizes come back exactly */
    int rx = make_socket(SOCK_DGRAM, 0);
    int tx = make_socket(SOCK_DGRAM, 0);
    struct sockaddr_in rxa = local_addr(rx);
    set_timeout(rx, 2000);
    for (int i = 0; i < NPARTS; i++)
        check(sendto(tx, parts[i], strlen(parts[i]), 0, (struct sockaddr *)&rxa, sizeof rxa) ==
                  (ssize_t)strlen(parts[i]), "udp send");
    printf("udp receive sizes:");
    for (int i = 0; i < NPARTS; i++) {
        char buf[64];
        check(wait_fd(rx, POLLIN, 2000), "udp readable");
        ssize_t r = recv(rx, buf, sizeof buf, 0);
        check(r == (ssize_t)strlen(parts[i]), "udp boundary preserved");
        check(memcmp(buf, parts[i], (size_t)r) == 0, "udp content");
        printf(" %zd", r);
    }
    printf("\n");
    close(rx);
    close(tx);

    /* TCP: the same sends form one stream; read it in fixed 5 byte chunks */
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(ls);
    int c = connect_to(la);
    int s = accept_to(ls);
    for (int i = 0; i < NPARTS; i++)
        check(send_all(c, parts[i], strlen(parts[i])) == 0, "tcp send");
    shutdown(c, SHUT_WR);
    char stream[64];
    size_t got = 0;
    int chunks = 0;
    printf("tcp chunks of 5:");
    for (;;) {
        char chunk[6];
        size_t n = recv_all(s, chunk, 5);
        if (n == 0)
            break;
        chunk[n] = 0;
        printf(" [%s]", chunk);
        memcpy(stream + got, chunk, n);
        got += n;
        chunks++;
    }
    printf("\n");
    stream[got] = 0;
    check(got == total, "stream total");
    printf("tcp stream (%zu bytes in %d reads): %s\n", got, chunks, stream);
    char expect[64] = "";
    for (int i = 0; i < NPARTS; i++)
        strcat(expect, parts[i]);
    check(strcmp(expect, stream) == 0, "stream equals concatenation");
    close(c);
    close(s);
    close(ls);
    return 0;
}
