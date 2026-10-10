/*
 * title: Line dialog over a stream socketpair
 * topic: networking
 * covers: socketpair AF_UNIX SOCK_STREAM, line buffering, request and response, calculator protocol, pthread peer
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
    int handled;
} Peer;

/* reads one line into buf (without newline); returns length or -1 on EOF */
static int read_line(int fd, char *carry, size_t *clen, char *out, size_t cap) {
    for (;;) {
        for (size_t i = 0; i < *clen; i++) {
            if (carry[i] == '\n') {
                if (i >= cap)
                    return -1;
                memcpy(out, carry, i);
                out[i] = 0;
                memmove(carry, carry + i + 1, *clen - i - 1);
                *clen -= i + 1;
                return (int)i;
            }
        }
        if (*clen == 256)
            return -1;
        ssize_t r = recv(fd, carry + *clen, 256 - *clen, 0);
        if (r <= 0)
            return -1;
        *clen += (size_t)r;
    }
}

static void *calc_peer(void *arg) {
    Peer *p = arg;
    char carry[256], line[128];
    size_t clen = 0;
    while (read_line(p->fd, carry, &clen, line, sizeof line) >= 0) {
        char op[8];
        long a, b;
        char rep[64];
        int n;
        if (sscanf(line, "%7s %ld %ld", op, &a, &b) == 3) {
            if (strcmp(op, "ADD") == 0)
                n = snprintf(rep, sizeof rep, "%ld\n", a + b);
            else if (strcmp(op, "MUL") == 0)
                n = snprintf(rep, sizeof rep, "%ld\n", a * b);
            else if (strcmp(op, "DIV") == 0 && b != 0)
                n = snprintf(rep, sizeof rep, "%ld\n", a / b);
            else
                n = snprintf(rep, sizeof rep, "ERR\n");
        } else {
            n = snprintf(rep, sizeof rep, "ERR\n");
        }
        send_all(p->fd, rep, (size_t)n);
        p->handled++;
    }
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    set_timeout(sv[0], 2000);
    set_timeout(sv[1], 2000);
    Peer peer = {sv[1], 0};
    pthread_t th;
    check(pthread_create(&th, NULL, calc_peer, &peer) == 0, "pthread_create");

    static const char *ops[] = {"ADD", "MUL", "DIV", "POW"};
    char carry[256];
    size_t clen = 0;
    int sent = 0;
    for (int i = 0; i < 12; i++) {
        uint32_t r1 = rnd();
        long a = (long)(r1 % 200) - 50;
        uint32_t r2 = rnd();
        long b = (long)(r2 % 40) - 10;
        const char *op = ops[i % 4];
        char req[64];
        int n = snprintf(req, sizeof req, "%s %ld %ld\n", op, a, b);
        check(send_all(sv[0], req, (size_t)n) == 0, "send request");
        char rep[64];
        check(read_line(sv[0], carry, &clen, rep, sizeof rep) >= 0, "reply");
        long want;
        int ok = 1;
        if (strcmp(op, "ADD") == 0)
            want = a + b;
        else if (strcmp(op, "MUL") == 0)
            want = a * b;
        else if (strcmp(op, "DIV") == 0 && b != 0)
            want = a / b;
        else {
            want = 0;
            ok = 0;
        }
        if (ok) {
            check(atol(rep) == want, "calculator result");
            printf("%s %ld %ld = %s\n", op, a, b, rep);
        } else {
            check(strcmp(rep, "ERR") == 0, "error reply");
            printf("%s %ld %ld = error\n", op, a, b);
        }
        sent++;
    }
    /* a malformed line yields ERR and the dialog continues */
    check(send_all(sv[0], "hello there\n", 12) == 0, "send junk");
    char rep[64];
    check(read_line(sv[0], carry, &clen, rep, sizeof rep) >= 0 && strcmp(rep, "ERR") == 0, "junk error");
    sent++;
    close(sv[0]);
    pthread_join(th, NULL);
    close(sv[1]);
    check(peer.handled == sent, "handled count");
    printf("peer handled %d requests\n", peer.handled);
    return 0;
}
