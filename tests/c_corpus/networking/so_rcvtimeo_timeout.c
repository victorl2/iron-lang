/*
 * title: Receive timeouts with SO_RCVTIMEO
 * topic: networking
 * covers: SO_RCVTIMEO, EAGAIN on timeout, TCP UDP and unix sockets, late data after a timeout, minimum elapsed time
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

#include <time.h>

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int timed_out(ssize_t r, int e) { return r < 0 && (e == EAGAIN || e == EWOULDBLOCK); }

typedef struct {
    int fd;
} Late;

static void *late_sender(void *arg) {
    Late *l = arg;
    usleep(120000);
    send_all(l->fd, "late", 4);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(ls);
    int c = connect_to(la);
    int s = accept_to(ls);

    /* idle TCP connection: three consecutive timeouts */
    set_timeout(s, 40);
    char buf[16];
    for (int i = 0; i < 3; i++) {
        long t0 = now_ms();
        ssize_t r = recv(s, buf, sizeof buf, 0);
        int e = errno;
        long dt = now_ms() - t0;
        printf("tcp recv %d: %s, waited at least 30ms: %s\n", i, timed_out(r, e) ? "timed out" : "unexpected",
               dt >= 30 ? "yes" : "no");
        check(timed_out(r, e) && dt >= 30, "tcp timeout");
    }

    /* the timeout value can be read back */
    struct timeval tv;
    socklen_t tl = sizeof tv;
    memset(&tv, 0, sizeof tv);
    check(getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, &tl) == 0, "get SO_RCVTIMEO");
    printf("timeout reads back as a sub-second value: %s\n", tv.tv_sec == 0 && tv.tv_usec > 0 ? "yes" : "no");
    check(tv.tv_sec == 0 && tv.tv_usec > 0, "timeval value");

    /* data that arrives after a timeout is still received on the next call */
    set_timeout(s, 60);
    Late late = {c};
    pthread_t th;
    check(pthread_create(&th, NULL, late_sender, &late) == 0, "thread");
    ssize_t r = recv(s, buf, sizeof buf, 0);
    int e = errno;
    printf("before the sender wakes: %s\n", timed_out(r, e) ? "timed out" : "got data");
    check(timed_out(r, e), "first attempt times out");
    set_timeout(s, 2000);
    r = recv(s, buf, sizeof buf, 0);
    pthread_join(th, NULL);
    check(r == 4 && memcmp(buf, "late", 4) == 0, "late data");
    printf("second attempt received %zd bytes: late\n", r);

    close(c);
    close(s);
    close(ls);

    /* UDP and socketpair sockets behave the same */
    int u = make_socket(SOCK_DGRAM, 0);
    set_timeout(u, 40);
    r = recvfrom(u, buf, sizeof buf, 0, NULL, NULL);
    e = errno;
    printf("udp recvfrom: %s\n", timed_out(r, e) ? "timed out" : "unexpected");
    check(timed_out(r, e), "udp timeout");
    close(u);
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    set_timeout(sv[0], 40);
    r = recv(sv[0], buf, sizeof buf, 0);
    e = errno;
    printf("socketpair recv: %s\n", timed_out(r, e) ? "timed out" : "unexpected");
    check(timed_out(r, e), "pair timeout");
    close(sv[0]);
    close(sv[1]);
    return 0;
}
