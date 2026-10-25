/*
 * title: poll based line server with fragmented client writes
 * topic: networking
 * covers: poll, pollfd array compaction, per-connection line buffers, partial line accumulation, POLLIN and POLLHUP handling
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

enum { NCLI = 4, MAXC = 8 };

typedef struct {
    struct sockaddr_in addr;
    char replies[NCLI][256];
    int ok;
} Clients;

static const char *lines[NCLI][3] = {
    {"alpha one", "alpha two", "alpha three"},
    {"beta", "another beta line here", "b"},
    {"gamma gamma", "g", "gamma-3"},
    {"delta 4", "delta 44", "delta 444"},
};

/* fragments of 3 bytes are written round robin so every server buffer holds partial lines */
static void *clients_main(void *arg) {
    Clients *cl = arg;
    int fd[NCLI];
    char payload[NCLI][128];
    size_t plen[NCLI], pos[NCLI];
    cl->ok = 1;
    for (int i = 0; i < NCLI; i++) {
        fd[i] = socket(AF_INET, SOCK_STREAM, 0);
        set_timeout(fd[i], 3000);
        if (connect(fd[i], (struct sockaddr *)&cl->addr, sizeof cl->addr) != 0)
            exit(1);
        plen[i] = 0;
        for (int k = 0; k < 3; k++)
            plen[i] += (size_t)snprintf(payload[i] + plen[i], sizeof payload[i] - plen[i], "%s\n", lines[i][k]);
        pos[i] = 0;
    }
    int active = NCLI;
    while (active > 0) {
        active = 0;
        for (int i = 0; i < NCLI; i++) {
            if (pos[i] >= plen[i])
                continue;
            size_t n = plen[i] - pos[i] < 3 ? plen[i] - pos[i] : 3;
            if (send_all(fd[i], payload[i] + pos[i], n) < 0)
                cl->ok = 0;
            pos[i] += n;
            if (pos[i] < plen[i])
                active++;
            else
                shutdown(fd[i], SHUT_WR);
        }
    }
    for (int i = 0; i < NCLI; i++) {
        size_t n = recv_all(fd[i], cl->replies[i], sizeof cl->replies[i] - 1);
        cl->replies[i][n] = 0;
        close(fd[i]);
    }
    return NULL;
}

typedef struct {
    char buf[128];
    size_t len;
    int lines;
} Conn;

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 8);
    static Clients cl;
    cl.addr = local_addr(ls);
    pthread_t th;
    check(pthread_create(&th, NULL, clients_main, &cl) == 0, "thread");

    struct pollfd pfd[MAXC];
    Conn conn[MAXC];
    int n = 0, seen = 0, finished = 0, guard = 0;
    pfd[0].fd = ls;
    pfd[0].events = POLLIN;
    n = 1;
    int total_lines = 0;
    while (finished < NCLI) {
        check(++guard < 5000, "loop bound");
        int r = poll(pfd, (nfds_t)n, 3000);
        check(r > 0, "poll progress");
        for (int i = n - 1; i >= 0; i--) {
            if (!pfd[i].revents)
                continue;
            if (i == 0) {
                int c = accept(ls, NULL, NULL);
                check(c >= 0 && n < MAXC, "accept");
                pfd[n].fd = c;
                pfd[n].events = POLLIN;
                memset(&conn[n], 0, sizeof conn[n]);
                n++;
                seen++;
                continue;
            }
            char tmp[64];
            ssize_t got = recv(pfd[i].fd, tmp, sizeof tmp, 0);
            if (got <= 0) {
                check(conn[i].len == 0, "no partial line left at EOF");
                close(pfd[i].fd);
                pfd[i] = pfd[n - 1];
                conn[i] = conn[n - 1];
                n--;
                finished++;
                continue;
            }
            check(conn[i].len + (size_t)got <= sizeof conn[i].buf, "line buffer");
            memcpy(conn[i].buf + conn[i].len, tmp, (size_t)got);
            conn[i].len += (size_t)got;
            /* answer every complete line with its length and upper-cased text */
            size_t start = 0;
            for (size_t k = 0; k < conn[i].len; k++) {
                if (conn[i].buf[k] != '\n')
                    continue;
                char out[160];
                int m = snprintf(out, sizeof out, "%zu:", k - start);
                for (size_t q = start; q < k; q++) {
                    char ch = conn[i].buf[q];
                    out[m++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
                }
                out[m++] = '|';
                check(send_all(pfd[i].fd, out, (size_t)m) == 0, "reply");
                conn[i].lines++;
                total_lines++;
                start = k + 1;
            }
            memmove(conn[i].buf, conn[i].buf + start, conn[i].len - start);
            conn[i].len -= start;
        }
    }
    pthread_join(th, NULL);
    check(cl.ok && seen == NCLI, "clients ok");
    for (int i = 0; i < NCLI; i++)
        printf("client %d: %s\n", i, cl.replies[i]);
    printf("%d lines answered\n", total_lines);
    check(total_lines == NCLI * 3, "line count");
    close(ls);
    return 0;
}
