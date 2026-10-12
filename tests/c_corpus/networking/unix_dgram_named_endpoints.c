/*
 * title: AF_UNIX datagram sockets with named endpoints
 * topic: networking
 * covers: SOCK_DGRAM AF_UNIX, bind paths, recvfrom sender path, sendto, ENOENT, ping-pong counter
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

static struct sockaddr_un named(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    check(strlen(path) < sizeof a.sun_path, "path fits");
    strcpy(a.sun_path, path);
    return a;
}

static int bound_dgram(const char *path) {
    unlink(path);
    struct sockaddr_un a = named(path);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    check(fd >= 0, "socket");
    check(bind(fd, (struct sockaddr *)&a, sizeof a) == 0, "bind");
    set_timeout(fd, 2000);
    return fd;
}

int main(void) {
    int a = bound_dgram("iron_a.sock");
    int b = bound_dgram("iron_b.sock");
    struct sockaddr_un aa = named("iron_a.sock"), ba = named("iron_b.sock");

    /* a and b bounce a counter, each side learns the peer path from recvfrom */
    int value = 1;
    for (int round = 0; round < 6; round++) {
        int from_fd = (round % 2 == 0) ? a : b;
        int to_fd = (round % 2 == 0) ? b : a;
        struct sockaddr_un *dest = (round % 2 == 0) ? &ba : &aa;
        char msg[32];
        int n = snprintf(msg, sizeof msg, "count=%d", value);
        check(sendto(from_fd, msg, (size_t)n, 0, (struct sockaddr *)dest, sizeof *dest) == n, "sendto");
        char buf[64];
        struct sockaddr_un src;
        socklen_t sl = sizeof src;
        memset(&src, 0, sizeof src);
        check(wait_fd(to_fd, POLLIN, 2000), "readable");
        ssize_t r = recvfrom(to_fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&src, &sl);
        check(r == n, "length");
        buf[r] = 0;
        const char *expect_src = (round % 2 == 0) ? "iron_a.sock" : "iron_b.sock";
        check(src.sun_family == AF_UNIX, "family");
        check(strcmp(src.sun_path, expect_src) == 0, "sender path");
        int got = 0;
        check(sscanf(buf, "count=%d", &got) == 1, "parse");
        value = got * 3 + 1;
        printf("round %d: %s received from %s\n", round, buf, expect_src);
    }
    printf("final value %d\n", value);

    /* sending to a path nobody bound fails cleanly */
    struct sockaddr_un none = named("iron_missing.sock");
    ssize_t r = sendto(a, "x", 1, 0, (struct sockaddr *)&none, sizeof none);
    int e = errno;
    printf("send to missing path: %s\n", r < 0 ? (e == ENOENT ? "ENOENT" : "other error") : "sent");
    check(r < 0 && e == ENOENT, "ENOENT");

    /* queue several datagrams then drain: order preserved */
    for (int i = 0; i < 4; i++) {
        char m[8];
        int n = snprintf(m, sizeof m, "m%d", i);
        check(sendto(a, m, (size_t)n, 0, (struct sockaddr *)&ba, sizeof ba) == n, "queue");
    }
    printf("drained:");
    for (int i = 0; i < 4; i++) {
        char m[16];
        check(wait_fd(b, POLLIN, 2000), "readable");
        ssize_t k = recv(b, m, sizeof m - 1, 0);
        m[k] = 0;
        printf(" %s", m);
    }
    printf("\n");
    close(a);
    close(b);
    check(unlink("iron_a.sock") == 0 && unlink("iron_b.sock") == 0, "unlink");
    return 0;
}
