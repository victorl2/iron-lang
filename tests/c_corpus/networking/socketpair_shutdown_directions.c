/*
 * title: shutdown directions on a stream socketpair
 * topic: networking
 * covers: SHUT_RD, SHUT_WR, SHUT_RDWR, per-direction EOF, POLLIN at EOF, independent duplex channels
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

static const char *rc_name(ssize_t r, int e) {
    if (r == 0)
        return "EOF";
    if (r > 0)
        return "data";
    if (e == EPIPE)
        return "EPIPE";
    if (e == EAGAIN || e == EWOULDBLOCK)
        return "EAGAIN";
    return "error";
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    char buf[16];

    /* SHUT_WR on side A: B sees EOF but can still write back */
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    set_timeout(sv[0], 300);
    set_timeout(sv[1], 300);
    check(send_all(sv[0], "last words", 10) == 0, "send");
    check(shutdown(sv[0], SHUT_WR) == 0, "SHUT_WR");
    check(recv_all(sv[1], buf, 10) == 10, "B reads pending data");
    ssize_t r = recv(sv[1], buf, sizeof buf, 0);
    printf("A shutdown(WR): B recv after data -> %s\n", rc_name(r, errno));
    check(r == 0, "B EOF");
    check(send_all(sv[1], "ack", 3) == 0, "B still writes");
    r = recv(sv[0], buf, sizeof buf, 0);
    printf("A recv after B writes -> %s\n", rc_name(r, errno));
    check(r == 3, "A reads");
    r = send(sv[0], "x", 1, 0);
    printf("A send after own SHUT_WR -> %s\n", rc_name(r, errno));
    check(r < 0 && errno == EPIPE, "A EPIPE");
    close(sv[0]);
    close(sv[1]);

    /* SHUT_RD on side A: A reads EOF at once, its own writes still go through */
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 2");
    set_timeout(sv[0], 300);
    set_timeout(sv[1], 300);
    check(shutdown(sv[0], SHUT_RD) == 0, "SHUT_RD");
    r = recv(sv[0], buf, sizeof buf, 0);
    printf("A shutdown(RD): A recv -> %s\n", rc_name(r, errno));
    check(r == 0, "A reads EOF");
    check(wait_fd(sv[0], POLLIN, 500), "A reported readable");
    printf("A poll POLLIN after SHUT_RD: yes\n");
    check(send_all(sv[0], "still talking", 13) == 0, "A writes");
    check(recv_all(sv[1], buf, 13) == 13 && memcmp(buf, "still talking", 13) == 0, "B reads");
    printf("B received A's data after A's SHUT_RD: yes\n");
    close(sv[0]);
    close(sv[1]);

    /* SHUT_RDWR on side A: both directions end */
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 3");
    set_timeout(sv[0], 300);
    set_timeout(sv[1], 300);
    check(shutdown(sv[0], SHUT_RDWR) == 0, "SHUT_RDWR");
    r = recv(sv[0], buf, sizeof buf, 0);
    printf("A shutdown(RDWR): A recv -> %s\n", rc_name(r, errno));
    check(r == 0, "A EOF");
    r = recv(sv[1], buf, sizeof buf, 0);
    printf("B recv -> %s\n", rc_name(r, errno));
    check(r == 0, "B EOF");
    r = send(sv[0], "x", 1, 0);
    printf("A send -> %s\n", rc_name(r, errno));
    check(r < 0 && errno == EPIPE, "A send EPIPE");
    close(sv[0]);
    close(sv[1]);

    /* both sides half-close in turn: a complete orderly close */
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 4");
    set_timeout(sv[0], 300);
    set_timeout(sv[1], 300);
    check(send_all(sv[0], "req", 3) == 0 && shutdown(sv[0], SHUT_WR) == 0, "A request and half close");
    size_t got = 0;
    while ((r = recv(sv[1], buf + got, sizeof buf - got, 0)) > 0)
        got += (size_t)r;
    check(r == 0 && got == 3, "B drains request");
    check(send_all(sv[1], "response", 8) == 0 && shutdown(sv[1], SHUT_WR) == 0, "B response and half close");
    got = 0;
    while ((r = recv(sv[0], buf + got, sizeof buf - got, 0)) > 0)
        got += (size_t)r;
    check(r == 0 && got == 8 && memcmp(buf, "response", 8) == 0, "A drains response");
    printf("orderly close: request 3 bytes, response 8 bytes, both sides saw EOF\n");
    close(sv[0]);
    close(sv[1]);
    return 0;
}
