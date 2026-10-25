/*
 * title: Generic address handling with sockaddr_storage
 * topic: networking
 * covers: sockaddr_storage, ss_family dispatch, AF_INET AF_INET6 AF_UNIX descriptions, size checks, accept and recvfrom into storage
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

/* describes any address without printing ports of live sockets */
static void describe(const struct sockaddr_storage *ss, int show_port, char *out, size_t cap) {
    char text[INET6_ADDRSTRLEN + 8];
    switch (ss->ss_family) {
    case AF_INET: {
        struct sockaddr_in a;
        memcpy(&a, ss, sizeof a);
        inet_ntop(AF_INET, &a.sin_addr, text, sizeof text);
        if (show_port)
            snprintf(out, cap, "inet %s port %u", text, (unsigned)ntohs(a.sin_port));
        else
            snprintf(out, cap, "inet %s", text);
        break;
    }
    case AF_INET6: {
        struct sockaddr_in6 a;
        memcpy(&a, ss, sizeof a);
        inet_ntop(AF_INET6, &a.sin6_addr, text, sizeof text);
        if (show_port)
            snprintf(out, cap, "inet6 [%s] port %u", text, (unsigned)ntohs(a.sin6_port));
        else
            snprintf(out, cap, "inet6 [%s]", text);
        break;
    }
    case AF_UNIX: {
        struct sockaddr_un a;
        memcpy(&a, ss, sizeof a);
        snprintf(out, cap, "unix path %.*s", (int)sizeof a.sun_path, a.sun_path);
        break;
    }
    default:
        snprintf(out, cap, "unknown family");
    }
}

static int same_endpoint(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family)
        return 0;
    if (a->ss_family == AF_INET) {
        struct sockaddr_in x, y;
        memcpy(&x, a, sizeof x);
        memcpy(&y, b, sizeof y);
        return x.sin_port == y.sin_port && x.sin_addr.s_addr == y.sin_addr.s_addr;
    }
    return 0;
}

int main(void) {
    printf("storage holds a sockaddr_in: %s\n", sizeof(struct sockaddr_storage) >= sizeof(struct sockaddr_in) ? "yes" : "no");
    printf("storage holds a sockaddr_in6: %s\n", sizeof(struct sockaddr_storage) >= sizeof(struct sockaddr_in6) ? "yes" : "no");
    printf("storage holds a sockaddr_un: %s\n", sizeof(struct sockaddr_storage) >= sizeof(struct sockaddr_un) ? "yes" : "no");
    check(sizeof(struct sockaddr_storage) >= sizeof(struct sockaddr_in6), "storage size in6");
    check(sizeof(struct sockaddr_storage) >= sizeof(struct sockaddr_un), "storage size un");

    struct sockaddr_storage list[4];
    memset(list, 0, sizeof list);
    struct sockaddr_in a4;
    memset(&a4, 0, sizeof a4);
    a4.sin_family = AF_INET;
    a4.sin_port = htons(4242);
    inet_pton(AF_INET, "203.0.113.9", &a4.sin_addr);
    memcpy(&list[0], &a4, sizeof a4);
    struct sockaddr_in6 a6;
    memset(&a6, 0, sizeof a6);
    a6.sin6_family = AF_INET6;
    a6.sin6_port = htons(53);
    inet_pton(AF_INET6, "2001:db8::35", &a6.sin6_addr);
    memcpy(&list[1], &a6, sizeof a6);
    struct sockaddr_un au;
    memset(&au, 0, sizeof au);
    au.sun_family = AF_UNIX;
    strcpy(au.sun_path, "service.sock");
    memcpy(&list[2], &au, sizeof au);
    list[3].ss_family = AF_UNSPEC;

    for (int i = 0; i < 4; i++) {
        char d[128];
        describe(&list[i], 1, d, sizeof d);
        printf("entry %d: %s\n", i, d);
    }

    /* live sockets: the kernel fills a storage with the right family and a shorter length */
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(ls);
    int c = connect_to(la);
    struct sockaddr_storage peer;
    socklen_t plen = sizeof peer;
    memset(&peer, 0xAB, sizeof peer);
    check(wait_fd(ls, POLLIN, 2000), "accept ready");
    int s = accept(ls, (struct sockaddr *)&peer, &plen);
    check(s >= 0, "accept");
    set_timeout(s, 2000);
    printf("accept peer length equals sizeof sockaddr_in: %s\n", plen == sizeof(struct sockaddr_in) ? "yes" : "no");
    check(plen == sizeof(struct sockaddr_in), "accept length");
    char d[128];
    describe(&peer, 0, d, sizeof d);
    printf("accepted peer: %s\n", d);
    struct sockaddr_in cl = local_addr(c);
    struct sockaddr_storage want;
    memset(&want, 0, sizeof want);
    memcpy(&want, &cl, sizeof cl);
    printf("peer equals the client's own address: %s\n", same_endpoint(&peer, &want) ? "yes" : "no");
    check(same_endpoint(&peer, &want), "peer identity");

    /* recvfrom into storage on a datagram socket */
    int r = make_socket(SOCK_DGRAM, 0);
    int t = make_socket(SOCK_DGRAM, 0);
    struct sockaddr_in ra = local_addr(r);
    struct sockaddr_in ta = local_addr(t);
    set_timeout(r, 2000);
    check(sendto(t, "who", 3, 0, (struct sockaddr *)&ra, sizeof ra) == 3, "sendto");
    struct sockaddr_storage from;
    socklen_t fl = sizeof from;
    char b[8];
    check(wait_fd(r, POLLIN, 2000), "udp readable");
    check(recvfrom(r, b, sizeof b, 0, (struct sockaddr *)&from, &fl) == 3, "recvfrom");
    struct sockaddr_storage tw;
    memset(&tw, 0, sizeof tw);
    memcpy(&tw, &ta, sizeof ta);
    describe(&from, 0, d, sizeof d);
    printf("datagram sender: %s, is the sending socket: %s\n", d, same_endpoint(&from, &tw) ? "yes" : "no");
    check(same_endpoint(&from, &tw), "udp sender");

    /* a too small buffer truncates the reported length but not the socket */
    char tiny[4];
    socklen_t tl = sizeof tiny;
    check(getsockname(c, (struct sockaddr *)tiny, &tl) == 0, "getsockname into tiny buffer");
    printf("full address needs %zu bytes, buffer had 4: reported length larger: %s\n", sizeof(struct sockaddr_in),
           tl > sizeof tiny ? "yes" : "no");
    check(tl == sizeof(struct sockaddr_in), "reported full length");
    close(r);
    close(t);
    close(c);
    close(s);
    close(ls);
    return 0;
}
