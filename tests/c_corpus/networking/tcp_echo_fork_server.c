/*
 * title: TCP rot13 server in a forked child
 * topic: networking
 * covers: fork, inherited listening socket, accept in child, waitpid status, stream transform
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

static char rot13(char ch) {
    if (ch >= 'a' && ch <= 'z')
        return (char)('a' + (ch - 'a' + 13) % 26);
    if (ch >= 'A' && ch <= 'Z')
        return (char)('A' + (ch - 'A' + 13) % 26);
    return ch;
}

/* child: serve exactly one connection, never touch stdio */
static void child_serve(int ls) {
    if (!wait_fd(ls, POLLIN, 3000))
        _exit(3);
    int c = accept(ls, NULL, NULL);
    if (c < 0)
        _exit(4);
    unsigned char buf[128];
    long handled = 0;
    for (;;) {
        if (!wait_fd(c, POLLIN, 3000))
            _exit(5);
        ssize_t r = recv(c, buf, sizeof buf, 0);
        if (r < 0)
            _exit(6);
        if (r == 0)
            break;
        for (ssize_t i = 0; i < r; i++)
            buf[i] = (unsigned char)rot13((char)buf[i]);
        if (send_all(c, buf, (size_t)r) < 0)
            _exit(7);
        handled += r;
    }
    close(c);
    close(ls);
    _exit(handled == 162 ? 0 : 8);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 4);
    struct sockaddr_in addr = local_addr(ls);
    fflush(stdout);
    pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0)
        child_serve(ls);

    int c = connect_to(addr);
    static const char *msgs[] = {"Hello, World!", "Iron Lang", "uryyb jbeyq", "The Quick Brown Fox 42",
                                 "abcdefghijklmnopqrstuvwxyz"};
    long sent = 0;
    for (int i = 0; i < 5; i++) {
        size_t n = strlen(msgs[i]);
        char back[64];
        check(send_all(c, msgs[i], n) == 0, "send");
        check(recv_all(c, back, n) == n, "recv");
        back[n] = 0;
        for (size_t k = 0; k < n; k++)
            check(back[k] == rot13(msgs[i][k]), "rot13 byte");
        printf("%s -> %s\n", msgs[i], back);
        /* applying rot13 twice gives the original back */
        char again[64];
        check(send_all(c, back, n) == 0, "send back");
        check(recv_all(c, again, n) == n, "recv again");
        check(memcmp(again, msgs[i], n) == 0, "involution");
        sent += 2 * (long)n;
    }
    close(c);
    close(ls);
    int status = 0;
    check(waitpid(pid, &status, 0) == pid, "waitpid");
    check(WIFEXITED(status), "child exited normally");
    printf("child handled %ld bytes, exit code %d\n", sent, WEXITSTATUS(status));
    check(WEXITSTATUS(status) == 0, "child exit code");
    return 0;
}
