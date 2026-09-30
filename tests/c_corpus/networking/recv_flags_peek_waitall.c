/*
 * title: recv flags MSG_PEEK, MSG_WAITALL and MSG_DONTWAIT
 * topic: networking
 * covers: MSG_PEEK on TCP and UDP, MSG_WAITALL across slow senders, MSG_DONTWAIT EAGAIN, header then body reads
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

typedef struct {
    int fd;
} Slow;

/* sends a 24 byte message in three separate pieces with pauses in between */
static void *slow_sender(void *arg) {
    Slow *s = arg;
    static const char *pieces[3] = {"ABCDEFGH", "IJKLMNOP", "QRSTUVWX"};
    for (int i = 0; i < 3; i++) {
        usleep(15000);
        send_all(s->fd, pieces[i], 8);
    }
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(ls);
    int c = connect_to(la);
    int s = accept_to(ls);

    /* MSG_PEEK looks without consuming */
    check(send_all(c, "\x00\x05hello world", 13) == 0, "send framed");
    check(wait_fd(s, POLLIN, 2000), "readable");
    unsigned char hdr[2];
    check(recv(s, hdr, 2, MSG_PEEK | MSG_WAITALL) == 2, "peek header");
    unsigned len = (unsigned)((hdr[0] << 8) | hdr[1]);
    printf("peeked length field: %u\n", len);
    unsigned char hdr2[2];
    check(recv(s, hdr2, 2, MSG_PEEK) == 2 && memcmp(hdr, hdr2, 2) == 0, "peek again");
    printf("second peek sees the same bytes: yes\n");
    unsigned char all[16];
    check(recv(s, all, 2, MSG_WAITALL) == 2, "consume header");
    char body[16];
    check(recv(s, body, len, MSG_WAITALL) == (ssize_t)len, "body");
    body[len] = 0;
    printf("body after consuming header: %s\n", body);
    char rest[16];
    check(recv(s, rest, 6, MSG_WAITALL) == 6, "rest");
    rest[6] = 0;
    printf("remaining: '%s'\n", rest);

    /* peek larger than what has arrived returns only what is there */
    check(send_all(c, "xyz", 3) == 0, "send xyz");
    check(wait_fd(s, POLLIN, 2000), "readable 2");
    char pk[16];
    ssize_t r = recv(s, pk, sizeof pk, MSG_PEEK);
    printf("peek with room for 16 bytes saw %zd\n", r);
    check(r == 3, "peek sees 3");
    r = recv(s, pk, sizeof pk, 0);
    check(r == 3, "read 3");

    /* nothing pending: MSG_DONTWAIT fails with EAGAIN */
    r = recv(s, pk, sizeof pk, MSG_DONTWAIT);
    int e = errno;
    printf("recv with nothing pending: %s\n", r < 0 && (e == EAGAIN || e == EWOULDBLOCK) ? "EAGAIN" : "unexpected");
    check(r < 0 && (e == EAGAIN || e == EWOULDBLOCK), "EAGAIN");

    /* MSG_WAITALL gathers a message the sender delivers slowly */
    Slow sl = {c};
    pthread_t th;
    check(pthread_create(&th, NULL, slow_sender, &sl) == 0, "thread");
    char buf[32];
    r = recv(s, buf, 24, MSG_WAITALL);
    pthread_join(th, NULL);
    check(r == 24, "waitall gathered everything");
    buf[24] = 0;
    printf("MSG_WAITALL collected %zd bytes: %s\n", r, buf);
    close(c);
    close(s);
    close(ls);

    /* UDP: peek leaves the datagram queued, even a short peek does not consume it */
    int rx = make_socket(SOCK_DGRAM, 0);
    int tx = make_socket(SOCK_DGRAM, 0);
    struct sockaddr_in ra = local_addr(rx);
    set_timeout(rx, 2000);
    check(sendto(tx, "datagram!", 9, 0, (struct sockaddr *)&ra, sizeof ra) == 9, "udp send");
    check(wait_fd(rx, POLLIN, 2000), "udp readable");
    r = recv(rx, buf, 4, MSG_PEEK);
    check(r == 4 && memcmp(buf, "data", 4) == 0, "short peek");
    r = recv(rx, buf, sizeof buf, MSG_PEEK);
    check(r == 9, "full peek");
    r = recv(rx, buf, sizeof buf, 0);
    buf[r] = 0;
    printf("udp peeked twice, then read %zd bytes: %s\n", r, buf);
    check(r == 9, "udp read");
    r = recv(rx, buf, sizeof buf, MSG_DONTWAIT);
    check(r < 0, "queue now empty");
    printf("udp queue is empty afterwards\n");
    close(rx);
    close(tx);
    return 0;
}
