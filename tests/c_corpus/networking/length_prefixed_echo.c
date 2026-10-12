/*
 * title: Length-prefixed framing echo with a streaming parser
 * topic: networking
 * covers: 4-byte big-endian frame length, incremental frame reassembly, zero-length frames, oversize rejection, coalesced and fragmented writes
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
static uint32_t fnv1a(const unsigned char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
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

enum { MAX_FRAME = 1024 };

typedef struct {
    unsigned char hdr[4];
    size_t hdr_have;
    unsigned char body[MAX_FRAME];
    size_t body_need, body_have;
    int in_body;
} Parser;

/* feeds bytes; calls emit(frame,len) per completed frame; returns -1 on a bad length */
static int feed(Parser *p, const unsigned char *d, size_t n, int (*emit)(void *, const unsigned char *, size_t), void *ctx) {
    while (n > 0) {
        if (!p->in_body) {
            p->hdr[p->hdr_have++] = *d++;
            n--;
            if (p->hdr_have == 4) {
                uint32_t len = ((uint32_t)p->hdr[0] << 24) | ((uint32_t)p->hdr[1] << 16) | ((uint32_t)p->hdr[2] << 8) | p->hdr[3];
                if (len > MAX_FRAME)
                    return -1;
                p->hdr_have = 0;
                p->body_need = len;
                p->body_have = 0;
                if (len == 0) {
                    if (emit(ctx, p->body, 0) < 0)
                        return -1;
                } else {
                    p->in_body = 1;
                }
            }
        } else {
            size_t take = p->body_need - p->body_have;
            if (take > n)
                take = n;
            memcpy(p->body + p->body_have, d, take);
            p->body_have += take;
            d += take;
            n -= take;
            if (p->body_have == p->body_need) {
                p->in_body = 0;
                if (emit(ctx, p->body, p->body_need) < 0)
                    return -1;
            }
        }
    }
    return 0;
}

static size_t put_frame(unsigned char *out, const unsigned char *body, size_t len) {
    out[0] = (unsigned char)(len >> 24);
    out[1] = (unsigned char)(len >> 16);
    out[2] = (unsigned char)(len >> 8);
    out[3] = (unsigned char)len;
    memcpy(out + 4, body, len);
    return len + 4;
}

typedef struct {
    int fd;
    int frames;
    int rejected;
} Echo;

static int echo_emit(void *ctx, const unsigned char *f, size_t n) {
    Echo *e = ctx;
    unsigned char out[MAX_FRAME + 4];
    size_t m = put_frame(out, f, n);
    e->frames++;
    return send_all(e->fd, out, m);
}

/* server reads in 7 byte gulps so frames straddle reads */
static void *server_main(void *arg) {
    Echo *e = arg;
    Parser p;
    memset(&p, 0, sizeof p);
    unsigned char buf[7];
    for (;;) {
        ssize_t r = recv(e->fd, buf, sizeof buf, 0);
        if (r <= 0)
            break;
        if (feed(&p, buf, (size_t)r, echo_emit, e) < 0) {
            e->rejected = 1;
            break;
        }
    }
    close(e->fd);
    return NULL;
}

typedef struct {
    int count;
    uint32_t hash;
    size_t bytes;
    size_t expect_len[64];
} Collect;

static int collect_emit(void *ctx, const unsigned char *f, size_t n) {
    Collect *c = ctx;
    if (c->count >= 64 || c->expect_len[c->count] != n)
        return -1;
    c->hash = c->hash * 16777619u ^ fnv1a(f, n);
    c->bytes += n;
    c->count++;
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(ls);
    int c = connect_to(la);
    Echo echo = {accept_to(ls), 0, 0};
    pthread_t th;
    check(pthread_create(&th, NULL, server_main, &echo) == 0, "thread");

    enum { NFRAMES = 40 };
    static unsigned char wire[NFRAMES * (MAX_FRAME + 4)];
    size_t wlen = 0;
    Collect want;
    memset(&want, 0, sizeof want);
    uint32_t want_hash = 0;
    size_t want_bytes = 0;
    for (int i = 0; i < NFRAMES; i++) {
        size_t len = (i % 9 == 0) ? 0 : (size_t)(rnd() % 300) + 1;
        if (i == 17)
            len = MAX_FRAME;
        unsigned char body[MAX_FRAME];
        for (size_t k = 0; k < len; k++)
            body[k] = (unsigned char)rnd();
        wlen += put_frame(wire + wlen, body, len);
        want.expect_len[i] = len;
        want_hash = want_hash * 16777619u ^ fnv1a(body, len);
        want_bytes += len;
    }
    /* first half in one coalesced write, second half dribbled in 11 byte pieces */
    size_t half = wlen / 2;
    check(send_all(c, wire, half) == 0, "coalesced send");
    for (size_t off = half; off < wlen; off += 11) {
        size_t n = wlen - off < 11 ? wlen - off : 11;
        check(send_all(c, wire + off, n) == 0, "piece send");
    }
    /* read the echoed stream and parse it with the same parser */
    Parser p;
    memset(&p, 0, sizeof p);
    Collect got;
    memset(&got, 0, sizeof got);
    memcpy(got.expect_len, want.expect_len, sizeof got.expect_len);
    size_t received = 0;
    while (got.count < NFRAMES) {
        unsigned char buf[97];
        ssize_t r = recv(c, buf, sizeof buf, 0);
        check(r > 0, "echo recv");
        received += (size_t)r;
        check(feed(&p, buf, (size_t)r, collect_emit, &got) == 0, "echo parse");
    }
    check(received == wlen, "echo byte count");
    check(got.hash == want_hash, "frame contents");
    printf("frames echoed: %d, payload bytes: %zu, wire bytes: %zu\n", got.count, got.bytes, received);
    printf("frame hash %08x, largest frame accepted\n", (unsigned)got.hash);
    check(got.bytes == want_bytes, "payload total");

    /* an oversized length makes the server drop the connection */
    unsigned char bad[4] = {0x00, 0x10, 0x00, 0x00};
    check(send_all(c, bad, 4) == 0, "send oversize header");
    char b;
    ssize_t r = recv(c, &b, 1, 0);
    printf("oversize frame header: connection %s\n", r == 0 ? "closed by server" : "still open");
    check(r == 0, "server closes");
    close(c);
    pthread_join(th, NULL);
    check(echo.rejected == 1 && echo.frames == NFRAMES, "server state");
    printf("server echoed %d frames and rejected the oversize header\n", echo.frames);
    close(ls);
    return 0;
}
