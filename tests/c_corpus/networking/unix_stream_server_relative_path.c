/*
 * title: AF_UNIX stream server on a relative path
 * topic: networking
 * covers: sockaddr_un, bind to filesystem path, S_ISSOCK, EADDRINUSE, unlink, ENOENT on connect, echo thread
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

#define SOCK_PATH "iron_echo.sock"

static const char *ename(int e) {
    if (e == EADDRINUSE)
        return "EADDRINUSE";
    if (e == ENOENT)
        return "ENOENT";
    if (e == ECONNREFUSED)
        return "ECONNREFUSED";
    return "other";
}

static struct sockaddr_un make_addr(const char *path, socklen_t *len) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    check(strlen(path) < sizeof a.sun_path, "path fits");
    strcpy(a.sun_path, path);
    *len = (socklen_t)sizeof a;
    return a;
}

static void *echo_server(void *arg) {
    int ls = *(int *)arg;
    if (!wait_fd(ls, POLLIN, 3000))
        return NULL;
    int c = accept(ls, NULL, NULL);
    if (c < 0)
        return NULL;
    char buf[64];
    for (;;) {
        if (!wait_fd(c, POLLIN, 3000))
            break;
        ssize_t r = recv(c, buf, sizeof buf, 0);
        if (r <= 0)
            break;
        for (ssize_t i = 0; i < r; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z')
                buf[i] = (char)(buf[i] - 32);
        if (send_all(c, buf, (size_t)r) < 0)
            break;
    }
    close(c);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    unlink(SOCK_PATH);
    socklen_t al;
    struct sockaddr_un a = make_addr(SOCK_PATH, &al);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    check(ls >= 0, "socket");
    check(bind(ls, (struct sockaddr *)&a, al) == 0, "bind");
    check(listen(ls, 4) == 0, "listen");

    struct stat st;
    check(stat(SOCK_PATH, &st) == 0, "stat");
    printf("path is socket: %s\n", S_ISSOCK(st.st_mode) ? "yes" : "no");

    int dup = socket(AF_UNIX, SOCK_STREAM, 0);
    int rc = bind(dup, (struct sockaddr *)&a, al);
    int e = errno;
    printf("second bind: %s (%s)\n", rc == 0 ? "ok" : "failed", rc == 0 ? "-" : ename(e));
    check(rc != 0, "second bind must fail");
    close(dup);

    pthread_t th;
    check(pthread_create(&th, NULL, echo_server, &ls) == 0, "thread");
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    set_timeout(c, 2000);
    check(connect(c, (struct sockaddr *)&a, al) == 0, "connect");
    static const char *msgs[] = {"hello unix", "second message", "MiXeD 123"};
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(msgs[i]);
        char back[64];
        check(send_all(c, msgs[i], n) == 0, "send");
        check(recv_all(c, back, n) == n, "recv");
        back[n] = 0;
        printf("%s -> %s\n", msgs[i], back);
    }
    close(c);
    pthread_join(th, NULL);
    close(ls);

    /* the socket file remains until unlinked, but nobody listens any more */
    int c2 = socket(AF_UNIX, SOCK_STREAM, 0);
    rc = connect(c2, (struct sockaddr *)&a, al);
    e = errno;
    printf("connect to stale socket file: %s\n", rc == 0 ? "ok" : ename(e));
    check(rc != 0 && (e == ECONNREFUSED), "stale refused");
    close(c2);
    check(unlink(SOCK_PATH) == 0, "unlink");
    int c3 = socket(AF_UNIX, SOCK_STREAM, 0);
    rc = connect(c3, (struct sockaddr *)&a, al);
    e = errno;
    printf("connect after unlink: %s\n", rc == 0 ? "ok" : ename(e));
    check(rc != 0 && e == ENOENT, "unlinked");
    close(c3);
    return 0;
}
