/*
 * title: SIGPIPE and EPIPE on writes to a closed socket
 * topic: networking
 * covers: SIGPIPE default action avoided, SIG_IGN, custom handler counting, EPIPE errno, write versus send, shutdown SHUT_WR
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

static volatile sig_atomic_t pipe_signals = 0;

static void on_pipe(int sig) {
    (void)sig;
    pipe_signals++;
}

static const char *ename(int e) {
    if (e == EPIPE)
        return "EPIPE";
    if (e == ECONNRESET)
        return "ECONNRESET";
    return "other";
}

/* keep writing to a socket whose peer has gone away until the kernel reports an error */
static int write_until_error(int fd, int use_write) {
    for (int i = 0; i < 200; i++) {
        ssize_t w = use_write ? write(fd, "x", 1) : send(fd, "x", 1, 0);
        if (w < 0)
            return errno;
        usleep(1000);
    }
    return 0;
}

int main(void) {
    /* phase 1: SIGPIPE ignored, the error is only visible through errno */
    check(signal(SIGPIPE, SIG_IGN) != SIG_ERR, "signal ignore");
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    close(sv[1]);
    ssize_t w = send(sv[0], "hello", 5, 0);
    int e = errno;
    printf("send to closed peer (ignored): %s\n", w < 0 ? ename(e) : "sent");
    check(w < 0 && e == EPIPE, "EPIPE on socketpair");
    w = write(sv[0], "hello", 5);
    e = errno;
    printf("write to closed peer (ignored): %s\n", w < 0 ? ename(e) : "wrote");
    check(w < 0 && e == EPIPE, "EPIPE from write");
    close(sv[0]);

    /* phase 2: a handler observes exactly one signal per failing write */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_pipe;
    sigemptyset(&sa.sa_mask);
    check(sigaction(SIGPIPE, &sa, NULL) == 0, "sigaction");
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 2");
    close(sv[1]);
    for (int i = 0; i < 3; i++) {
        w = send(sv[0], "x", 1, 0);
        e = errno;
        check(w < 0 && e == EPIPE, "EPIPE with handler");
    }
    printf("handler ran %d times for 3 failed writes\n", (int)pipe_signals);
    check(pipe_signals == 3, "one SIGPIPE per failed write");
    close(sv[0]);

    /* phase 3: own SHUT_WR also breaks the write side */
    check(signal(SIGPIPE, SIG_IGN) != SIG_ERR, "ignore again");
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 3");
    check(shutdown(sv[0], SHUT_WR) == 0, "shutdown");
    w = send(sv[0], "x", 1, 0);
    e = errno;
    printf("send after own SHUT_WR: %s\n", w < 0 ? ename(e) : "sent");
    check(w < 0 && e == EPIPE, "EPIPE after shutdown");
    /* the peer can still write to us */
    check(send(sv[1], "still", 5, 0) == 5, "peer writes");
    char b[8];
    check(wait_fd(sv[0], POLLIN, 2000), "readable");
    check(recv(sv[0], b, sizeof b, 0) == 5, "read peer data");
    printf("reads still work after SHUT_WR\n");
    close(sv[0]);
    close(sv[1]);

    /* phase 4: TCP; the first write after the peer closed may succeed, a later one fails */
    int ls = make_socket(SOCK_STREAM, 1);
    struct sockaddr_in la = local_addr(ls);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    set_timeout(c, 2000);
    check(connect(c, (struct sockaddr *)&la, sizeof la) == 0, "connect");
    int s = accept(ls, NULL, NULL);
    check(s >= 0, "accept");
    close(s);
    e = write_until_error(c, 0);
    printf("tcp writes to closed peer eventually fail: %s\n", e == EPIPE || e == ECONNRESET ? "yes" : "no");
    check(e == EPIPE || e == ECONNRESET, "tcp error");
    close(c);
    close(ls);
    return 0;
}
