/*
 * title: Datagram socketpair message boundaries
 * topic: networking
 * covers: socketpair AF_UNIX SOCK_DGRAM, ordering, bidirectional, short buffer truncation, SO_TYPE
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
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 16);
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

static const char *type_name(int t) {
    if (t == SOCK_STREAM)
        return "SOCK_STREAM";
    if (t == SOCK_DGRAM)
        return "SOCK_DGRAM";
    return "other";
}

int main(void) {
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair");
    set_timeout(sv[0], 2000);
    set_timeout(sv[1], 2000);
    int ty = 0;
    socklen_t tl = sizeof ty;
    check(getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &ty, &tl) == 0, "SO_TYPE");
    printf("socket type: %s\n", type_name(ty));

    /* five datagrams of different sizes each keep their boundary */
    size_t sizes[5] = {1, 7, 30, 3, 12};
    unsigned char sent[5][32];
    for (int i = 0; i < 5; i++) {
        for (size_t k = 0; k < sizes[i]; k++)
            sent[i][k] = (unsigned char)('a' + (rnd() % 26));
        check(send(sv[0], sent[i], sizes[i], 0) == (ssize_t)sizes[i], "send");
    }
    printf("received sizes:");
    for (int i = 0; i < 5; i++) {
        unsigned char buf[64];
        check(wait_fd(sv[1], POLLIN, 2000), "readable");
        ssize_t r = recv(sv[1], buf, sizeof buf, 0);
        check(r == (ssize_t)sizes[i], "boundary");
        check(memcmp(buf, sent[i], sizes[i]) == 0, "content and order");
        printf(" %zd", r);
    }
    printf("\n");

    /* the other direction works independently */
    check(send(sv[1], "pong-one", 8, 0) == 8, "send back 1");
    check(send(sv[1], "pong-two!", 9, 0) == 9, "send back 2");
    check(send(sv[0], "ping", 4, 0) == 4, "send fwd");
    char b[32];
    ssize_t r = recv(sv[0], b, sizeof b, 0);
    b[r] = 0;
    printf("side0 first: %s\n", b);
    r = recv(sv[0], b, sizeof b, 0);
    b[r] = 0;
    printf("side0 second: %s\n", b);
    r = recv(sv[1], b, sizeof b, 0);
    b[r] = 0;
    printf("side1: %s\n", b);

    /* a small receive buffer takes the head of a datagram; the tail is discarded */
    check(send(sv[0], "0123456789", 10, 0) == 10, "send ten");
    check(send(sv[0], "tail", 4, 0) == 4, "send tail");
    char small[3];
    r = recv(sv[1], small, sizeof small, 0);
    check(r == 3 && memcmp(small, "012", 3) == 0, "truncated head");
    r = recv(sv[1], b, sizeof b, 0);
    b[r] = 0;
    printf("after truncation the next message is: %s\n", b);
    check(strcmp(b, "tail") == 0, "next intact");
    check(!wait_fd(sv[1], POLLIN, 20), "drained");
    close(sv[0]);
    close(sv[1]);
    return 0;
}
