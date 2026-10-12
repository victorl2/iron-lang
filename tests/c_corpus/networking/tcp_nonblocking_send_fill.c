/*
 * title: Filling a socket buffer until send would block
 * topic: networking
 * covers: nonblocking send, EAGAIN on full buffer, draining with a reader, POLLOUT after drain, resumed sending, total byte accounting
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

static unsigned char pattern(size_t i) { return (unsigned char)((i * 7 + (i >> 8) * 13) & 0xff); }

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    check(fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0, "nonblock");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(ls);
    int c = connect_to(la);
    int s = accept_to(ls);
    int small = 16384;
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    set_nonblock(c);

    size_t sent = 0, received = 0;
    uint32_t sh = 2166136261u, rh = 2166136261u;
    for (int cycle = 0; cycle < 4; cycle++) {
        /* fill: keep sending until the kernel refuses */
        int hit = 0;
        size_t before = sent;
        for (int iter = 0; iter < 100000; iter++) {
            unsigned char buf[2048];
            for (size_t k = 0; k < sizeof buf; k++)
                buf[k] = pattern(sent + k);
            ssize_t n = send(c, buf, sizeof buf, 0);
            if (n < 0) {
                check(errno == EAGAIN || errno == EWOULDBLOCK, "send error kind");
                hit = 1;
                break;
            }
            for (ssize_t k = 0; k < n; k++) {
                sh ^= buf[k];
                sh *= 16777619u;
            }
            sent += (size_t)n;
            check(sent - before < (size_t)64 << 20, "buffer bound");
        }
        check(hit, "hit EAGAIN");
        check(sent > before, "some bytes fit before blocking");
        printf("cycle %d: send reported EAGAIN after buffering some bytes: yes\n", cycle);

        /* drain: read everything currently available, verifying order */
        while (received < sent) {
            check(wait_fd(s, POLLIN, 2000), "data readable");
            unsigned char buf[3000];
            ssize_t n = recv(s, buf, sizeof buf, 0);
            check(n > 0, "recv");
            for (ssize_t k = 0; k < n; k++) {
                check(buf[k] == pattern(received + (size_t)k), "byte order");
                rh ^= buf[k];
                rh *= 16777619u;
            }
            received += (size_t)n;
        }
        check(wait_fd(c, POLLOUT, 2000), "writable again after drain");
        printf("cycle %d: writable again after the reader drained: yes\n", cycle);
    }
    check(sent == received, "byte totals");
    check(sh == rh, "hash totals");
    printf("all cycles: sent equals received: yes, hashes equal: yes\n");
    /* a final small write always fits once the buffer is empty */
    check(send(c, "end", 3, 0) == 3, "final send");
    char b[3];
    check(recv(s, b, 3, MSG_WAITALL) == 3 && memcmp(b, "end", 3) == 0, "final recv");
    close(c);
    close(s);
    close(ls);
    return 0;
}
