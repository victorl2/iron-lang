/*
 * title: Token relay ring of threads over socketpairs
 * topic: networking
 * covers: chain of socketpairs, one thread per node, token transformed at each hop, stop token propagation, per-node hop counts
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

enum { NODES = 6, LAPS = 5 };
#define STOP 0xFFFFFFFFu

typedef struct {
    uint32_t lap;
    uint32_t value;
} Token;

typedef struct {
    int node;
    int in_fd, out_fd;
    int hops;
} Node;

static uint32_t hop(uint32_t v, int node) { return v * 31u + (uint32_t)node + 1u; }

static void *node_main(void *arg) {
    Node *n = arg;
    for (;;) {
        Token t;
        if (recv_all(n->in_fd, &t, sizeof t) != sizeof t)
            exit(1);
        if (t.lap == STOP) {
            send_all(n->out_fd, &t, sizeof t);
            break;
        }
        t.value = hop(t.value, n->node);
        n->hops++;
        if (send_all(n->out_fd, &t, sizeof t) < 0)
            exit(1);
    }
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    /* link i carries tokens from node i to node (i+1) % NODES: end 0 writes, end 1 reads */
    int link[NODES][2];
    for (int i = 0; i < NODES; i++) {
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, link[i]) == 0, "socketpair");
        set_timeout(link[i][0], 3000);
        set_timeout(link[i][1], 3000);
    }
    Node nodes[NODES];
    pthread_t th[NODES];
    memset(nodes, 0, sizeof nodes);
    for (int i = 1; i < NODES; i++) {
        nodes[i].node = i;
        nodes[i].in_fd = link[i - 1][1];
        nodes[i].out_fd = link[i][0];
        check(pthread_create(&th[i], NULL, node_main, &nodes[i]) == 0, "thread");
    }
    uint32_t value = 7, expect = 7;
    for (int lap = 0; lap < LAPS; lap++) {
        value = hop(value, 0);
        Token t = {(uint32_t)lap, value};
        check(send_all(link[0][0], &t, sizeof t) == 0, "inject");
        Token back;
        check(recv_all(link[NODES - 1][1], &back, sizeof back) == sizeof back, "token returns");
        check(back.lap == (uint32_t)lap, "lap tag");
        value = back.value;
        for (int i = 0; i < NODES; i++)
            expect = hop(expect, i);
        check(value == expect, "value after lap");
        printf("lap %d: token value %08x\n", lap, (unsigned)value);
    }
    Token stop = {STOP, 0};
    check(send_all(link[0][0], &stop, sizeof stop) == 0, "send stop");
    Token echo;
    check(recv_all(link[NODES - 1][1], &echo, sizeof echo) == sizeof echo && echo.lap == STOP, "stop returns");
    for (int i = 1; i < NODES; i++)
        pthread_join(th[i], NULL);
    for (int i = 1; i < NODES; i++) {
        printf("node %d forwarded %d tokens\n", i, nodes[i].hops);
        check(nodes[i].hops == LAPS, "hops per node");
    }
    printf("stop token travelled the whole ring: yes\n");
    for (int i = 0; i < NODES; i++) {
        close(link[i][0]);
        close(link[i][1]);
    }
    return 0;
}
