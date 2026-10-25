/*
 * title: getaddrinfo restricted to numeric hosts and services
 * topic: networking
 * covers: getaddrinfo, AI_NUMERICHOST, AI_NUMERICSERV, AI_PASSIVE, EAI_NONAME, freeaddrinfo, getnameinfo NI_NUMERIC, connect via resolved address
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
static int accept_to(int ls) {
    check(wait_fd(ls, POLLIN, 2000), "accept ready");
    int c = accept(ls, NULL, NULL);
    check(c >= 0, "accept");
    set_timeout(c, 2000);
    return c;
}

static const char *gai_name(int e) {
    if (e == 0)
        return "ok";
    if (e == EAI_NONAME)
        return "EAI_NONAME";
    return "other";
}

static int lookup(const char *host, const char *serv, int flags, int socktype, struct addrinfo **out) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;
    hints.ai_flags = flags;
    return getaddrinfo(host, serv, &hints, out);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    struct addrinfo *ai = NULL;
    int rc = lookup("127.0.0.1", "8080", AI_NUMERICHOST | AI_NUMERICSERV, SOCK_STREAM, &ai);
    printf("numeric host and service: %s\n", gai_name(rc));
    check(rc == 0 && ai != NULL, "numeric lookup");
    check(ai->ai_family == AF_INET && ai->ai_socktype == SOCK_STREAM, "family and type");
    struct sockaddr_in sa;
    memcpy(&sa, ai->ai_addr, sizeof sa);
    printf("address is loopback: %s, port 8080: %s, addrlen matches: %s\n",
           sa.sin_addr.s_addr == htonl(INADDR_LOOPBACK) ? "yes" : "no", ntohs(sa.sin_port) == 8080 ? "yes" : "no",
           ai->ai_addrlen == sizeof sa ? "yes" : "no");
    check(sa.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(sa.sin_port) == 8080, "resolved values");
    char host[64], serv[16];
    rc = getnameinfo(ai->ai_addr, ai->ai_addrlen, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV);
    check(rc == 0, "getnameinfo");
    printf("getnameinfo: %s:%s\n", host, serv);
    freeaddrinfo(ai);

    /* names that would need DNS are rejected up front */
    rc = lookup("localhost", "80", AI_NUMERICHOST | AI_NUMERICSERV, SOCK_STREAM, &ai);
    printf("hostname with AI_NUMERICHOST: %s\n", gai_name(rc));
    check(rc == EAI_NONAME, "hostname refused");
    rc = lookup("127.0.0.1", "http", AI_NUMERICHOST | AI_NUMERICSERV, SOCK_STREAM, &ai);
    printf("service name with AI_NUMERICSERV: %s\n", gai_name(rc));
    check(rc == EAI_NONAME, "service name refused");
    rc = lookup("999.1.1.1", "80", AI_NUMERICHOST | AI_NUMERICSERV, SOCK_STREAM, &ai);
    printf("malformed address: %s\n", gai_name(rc));
    check(rc == EAI_NONAME, "bad address refused");

    /* passive lookup without a host gives the wildcard address */
    rc = lookup(NULL, "9000", AI_PASSIVE | AI_NUMERICSERV, SOCK_DGRAM, &ai);
    check(rc == 0 && ai != NULL, "passive lookup");
    memcpy(&sa, ai->ai_addr, sizeof sa);
    printf("passive: wildcard %s, datagram %s, port 9000 %s\n", sa.sin_addr.s_addr == htonl(INADDR_ANY) ? "yes" : "no",
           ai->ai_socktype == SOCK_DGRAM ? "yes" : "no", ntohs(sa.sin_port) == 9000 ? "yes" : "no");
    check(sa.sin_addr.s_addr == htonl(INADDR_ANY) && ntohs(sa.sin_port) == 9000, "passive values");
    freeaddrinfo(ai);

    /* use resolved addresses for real: bind to port 0 then connect with a numeric service string */
    rc = lookup("127.0.0.1", "0", AI_NUMERICHOST | AI_NUMERICSERV, SOCK_STREAM, &ai);
    check(rc == 0, "bind lookup");
    int ls = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    check(ls >= 0 && bind(ls, ai->ai_addr, ai->ai_addrlen) == 0 && listen(ls, 2) == 0, "bind and listen");
    freeaddrinfo(ai);
    struct sockaddr_in bound;
    socklen_t bl = sizeof bound;
    check(getsockname(ls, (struct sockaddr *)&bound, &bl) == 0, "getsockname");
    char port[16];
    snprintf(port, sizeof port, "%u", (unsigned)ntohs(bound.sin_port));
    rc = lookup("127.0.0.1", port, AI_NUMERICHOST | AI_NUMERICSERV, SOCK_STREAM, &ai);
    check(rc == 0, "connect lookup");
    int c = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    check(c >= 0 && connect(c, ai->ai_addr, ai->ai_addrlen) == 0, "connect");
    freeaddrinfo(ai);
    int s = accept_to(ls);
    check(send(c, "resolved", 8, 0) == 8, "send");
    char buf[8];
    check(recv(s, buf, 8, MSG_WAITALL) == 8 && memcmp(buf, "resolved", 8) == 0, "recv");
    printf("connected through getaddrinfo results: yes\n");
    close(c);
    close(s);
    close(ls);
    return 0;
}
