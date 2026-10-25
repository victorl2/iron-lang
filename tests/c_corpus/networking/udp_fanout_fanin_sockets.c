/*
 * title: UDP fan-out to many local sockets and fan-in of replies
 * topic: networking
 * covers: iterated sendto as broadcast emulation, one socket per subscriber, per-receiver transformation, reply aggregation, poll drain
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

enum { NSUB = 6 };

int main(void) {
    int pub = make_socket(SOCK_DGRAM, 0);
    struct sockaddr_in pub_addr = local_addr(pub);
    set_timeout(pub, 2000);
    int sub[NSUB];
    struct sockaddr_in addr[NSUB];
    for (int i = 0; i < NSUB; i++) {
        sub[i] = make_socket(SOCK_DGRAM, 0);
        addr[i] = local_addr(sub[i]);
        set_timeout(sub[i], 2000);
    }

    /* three rounds of announcements to every subscriber, tagged with round and target */
    uint32_t checksum = 0;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < NSUB; i++) {
            char msg[48];
            uint32_t token = rnd() % 1000;
            int n = snprintf(msg, sizeof msg, "round=%d target=%d token=%u", round, i, (unsigned)token);
            check(sendto(pub, msg, (size_t)n, 0, (struct sockaddr *)&addr[i], sizeof addr[i]) == n, "fanout send");
        }
        /* each subscriber sees exactly its own message and answers with token squared */
        for (int i = 0; i < NSUB; i++) {
            char buf[64];
            check(wait_fd(sub[i], POLLIN, 2000), "subscriber readable");
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t r = recvfrom(sub[i], buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
            check(r > 0, "subscriber recv");
            buf[r] = 0;
            int rd = -1, tg = -1;
            unsigned tok = 0;
            check(sscanf(buf, "round=%d target=%d token=%u", &rd, &tg, &tok) == 3, "parse");
            check(rd == round && tg == i, "message meant for this subscriber");
            check(from.sin_port == pub_addr.sin_port, "from publisher");
            char rep[32];
            int n = snprintf(rep, sizeof rep, "%d:%u", i, tok * tok);
            check(sendto(sub[i], rep, (size_t)n, 0, (struct sockaddr *)&from, fl) == n, "reply");
            checksum = checksum * 131u + tok;
        }
        /* the publisher drains replies; since sends were in order, so are the replies */
        printf("round %d replies:", round);
        int count = 0;
        while (wait_fd(pub, POLLIN, count < NSUB ? 2000 : 20)) {
            char buf[32];
            ssize_t r = recv(pub, buf, sizeof buf - 1, 0);
            check(r > 0, "reply recv");
            buf[r] = 0;
            int who;
            unsigned sq;
            check(sscanf(buf, "%d:%u", &who, &sq) == 2, "reply parse");
            check(who == count, "reply order");
            printf(" %s", buf);
            count++;
        }
        printf("\n");
        check(count == NSUB, "all replies");
    }
    printf("checksum of tokens: %08x\n", (unsigned)checksum);
    for (int i = 0; i < NSUB; i++)
        close(sub[i]);
    close(pub);
    return 0;
}
