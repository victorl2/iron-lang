/*
 * title: NTP-style time exchange with fake clocks
 * topic: networking
 * covers: 32.32 fixed-point timestamps, four-timestamp offset and delay, clock filter, spoof rejection, UDP
 * deps: libc, posix, sockets
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

void die(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

uint32_t rng32(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (uint32_t)(z >> 16);
}

void net_init(void) { signal(SIGPIPE, SIG_IGN); }

void set_tmo(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static void lo_addr(struct sockaddr_in *a, int port) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a->sin_port = htons((unsigned short)port);
}

int listen_lo(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    lo_addr(&a, 0);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) die("bind");
    if (listen(fd, 16) < 0) die("listen");
    socklen_t l = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &l) < 0) die("getsockname");
    *port = ntohs(a.sin_port);
    return fd;
}

int accept_lo(int lfd) {
    struct pollfd p;
    p.fd = lfd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, 5000) <= 0) return -1;
    int fd = accept(lfd, NULL, NULL);
    if (fd >= 0) set_tmo(fd, 5000);
    return fd;
}

int connect_lo(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");
    struct sockaddr_in a;
    lo_addr(&a, port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) die("connect");
    set_tmo(fd, 5000);
    return fd;
}

/* UDP bound to loopback, receive timeout in ms */
int udp_lo(int *port, int ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) die("socket");
    struct sockaddr_in a;
    lo_addr(&a, 0);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) die("bind");
    socklen_t l = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &l) < 0) die("getsockname");
    *port = ntohs(a.sin_port);
    set_tmo(fd, ms);
    return fd;
}

int udp_send(int fd, int port, const void *b, size_t n) {
    struct sockaddr_in a;
    lo_addr(&a, port);
    return (int)sendto(fd, b, n, 0, (struct sockaddr *)&a, sizeof a);
}

/* returns bytes, or -1 on timeout; *from = sender port */
int udp_recv(int fd, void *b, size_t cap, int *from) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    ssize_t n = recvfrom(fd, b, cap, 0, (struct sockaddr *)&a, &l);
    if (n < 0) return -1;
    if (from) *from = ntohs(a.sin_port);
    return (int)n;
}

int send_all(int fd, const void *b, size_t n) {
    const unsigned char *p = b;
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int send_str(int fd, const char *s) { return send_all(fd, s, strlen(s)); }

int sendf(int fd, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof buf) die("sendf overflow");
    return send_all(fd, buf, (size_t)n);
}

/* buffered reader */
typedef struct {
    int fd;
    unsigned char buf[8192];
    size_t pos, len;
} Conn;

void conn_init(Conn *c, int fd) {
    c->fd = fd;
    c->pos = c->len = 0;
}

int conn_getc(Conn *c) {
    if (c->pos == c->len) {
        ssize_t n = recv(c->fd, c->buf, sizeof c->buf, 0);
        if (n <= 0) return -1;
        c->pos = 0;
        c->len = (size_t)n;
    }
    return c->buf[c->pos++];
}

/* reads a line, strips CRLF/LF; returns length or -1 on EOF/timeout/overflow */
int conn_readline(Conn *c, char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        int ch = conn_getc(c);
        if (ch < 0) return -1;
        if (ch == '\n') break;
        if (n + 1 >= cap) return -1;
        out[n++] = (char)ch;
    }
    if (n > 0 && out[n - 1] == '\r') n--;
    out[n] = 0;
    return (int)n;
}

int conn_readn(Conn *c, void *out, size_t n) {
    unsigned char *p = out;
    for (size_t i = 0; i < n; i++) {
        int ch = conn_getc(c);
        if (ch < 0) return -1;
        p[i] = (unsigned char)ch;
    }
    return 0;
}

uint32_t crc32_buf(const unsigned char *d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

uint32_t fnv1a(const void *d, size_t n) {
    const unsigned char *p = d;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* like connect_lo but returns -1 on failure */
int connect_try(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");
    struct sockaddr_in a;
    lo_addr(&a, port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    set_tmo(fd, 5000);
    return fd;
}

/* returns a loopback port that nothing listens on */
int dead_port(void) {
    int port;
    int fd = listen_lo(&port);
    close(fd);
    return port;
}

/* copies bytes both ways until one side closes (5s idle limit); returns bytes relayed */
long relay_pair(int a, int b) {
    long total = 0;
    for (;;) {
        struct pollfd p[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
        if (poll(p, 2, 5000) <= 0) break;
        int done = 0;
        for (int i = 0; i < 2 && !done; i++) {
            if (!(p[i].revents & (POLLIN | POLLHUP))) continue;
            unsigned char buf[2048];
            ssize_t n = recv(p[i].fd, buf, sizeof buf, 0);
            if (n <= 0) { done = 1; break; }
            if (send_all(p[1 - i].fd, buf, (size_t)n) < 0) { done = 1; break; }
            total += n;
        }
        if (done) break;
    }
    return total;
}


/* All times come from a simulated world clock; nothing here reads the real clock. */
static int64_t world_us = 3900000000LL * 1000000LL; /* microseconds since 1900 */
static const int64_t SERVER_BIAS_US = 0;
static const int64_t CLIENT_BIAS_US = -1750250; /* client clock runs 1.75025 s behind */

static uint64_t us_to_ntp(int64_t us) {
    uint64_t sec = (uint64_t)(us / 1000000);
    uint64_t frac = ((uint64_t)(us % 1000000) << 32) / 1000000u;
    return (sec << 32) | frac;
}

static int64_t ticks_to_us(int64_t t) {
    int64_t half = (int64_t)1 << 31;
    return (t * 1000000 + (t >= 0 ? half : -half)) / ((int64_t)1 << 32);
}

static void put64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (56 - 8 * i));
}
static uint64_t get64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

/* packet: [0]=mode [1]=stratum [8..15]=origin [16..23]=receive [24..31]=transmit; 48 bytes total */
#define PKT 48

static int64_t client_clock(int64_t bias) { return world_us + bias; }
static int64_t server_clock(void) { return world_us + SERVER_BIAS_US; }

/* the server handles exactly one request that is already queued on its socket */
static int server_step(int sfd, int proc_us) {
    unsigned char in[64], out[PKT];
    int from;
    int n = udp_recv(sfd, in, sizeof in, &from);
    if (n != PKT || (in[0] & 7) != 3) return -1;
    memset(out, 0, sizeof out);
    out[0] = 4; /* server mode */
    out[1] = 2; /* stratum */
    uint64_t t2 = us_to_ntp(server_clock());
    memcpy(out + 8, in + 24, 8); /* origin = client's transmit */
    world_us += proc_us;
    uint64_t t3 = us_to_ntp(server_clock());
    put64(out + 16, t2);
    put64(out + 24, t3);
    return udp_send(sfd, from, out, sizeof out) == PKT ? 0 : -1;
}

typedef struct {
    int64_t theta_us, delta_us;
    int64_t d1, d2;
} Sample;

int main(void) {
    net_init();
    int sport, cport, xport;
    int sfd = udp_lo(&sport, 2000);
    int cfd = udp_lo(&cport, 2000);
    int xfd = udp_lo(&xport, 2000); /* spoofer */
    uint64_t rs = 123;
    Sample best;
    memset(&best, 0, sizeof best);
    best.delta_us = INT64_MAX;
    int64_t bias = CLIENT_BIAS_US;
    int spoofs_rejected = 0;

    for (int round = 0; round < 8; round++) {
        uint32_t a = rng32(&rs);
        uint32_t b = rng32(&rs);
        int64_t d1 = 200 + (int64_t)(a % 800) * 10; /* microseconds, multiples of 10 */
        int64_t d2 = 200 + (int64_t)(b % 800) * 10;
        if (round == 5) d2 = d1; /* one perfectly symmetric sample */
        unsigned char req[PKT], rep[PKT];
        memset(req, 0, sizeof req);
        req[0] = 3;
        uint64_t t1 = us_to_ntp(client_clock(bias));
        put64(req + 24, t1);
        CHECK(udp_send(cfd, sport, req, sizeof req) == PKT);
        world_us += d1;
        CHECK(server_step(sfd, 300) == 0);

        /* a spoofed reply with a wrong origin timestamp arrives first */
        if (round % 3 == 0) {
            unsigned char fake[PKT];
            memcpy(fake, req, sizeof fake);
            fake[0] = 4;
            put64(fake + 8, t1 ^ 0x100);
            put64(fake + 16, t1);
            put64(fake + 24, t1);
            CHECK(udp_send(xfd, cport, fake, sizeof fake) == PKT);
        }
        world_us += d2;
        int got = 0;
        for (int k = 0; k < 2 && !got; k++) {
            CHECK(udp_recv(cfd, rep, sizeof rep, NULL) == PKT);
            if (get64(rep + 8) != t1) { spoofs_rejected++; continue; }
            got = 1;
        }
        CHECK(got);
        uint64_t t4 = us_to_ntp(client_clock(bias));
        uint64_t t2 = get64(rep + 16), t3 = get64(rep + 24);
        int64_t off_ticks = ((int64_t)(t2 - t1) + (int64_t)(t3 - t4)) / 2;
        int64_t dly_ticks = (int64_t)(t4 - t1) - (int64_t)(t3 - t2);
        Sample s;
        s.theta_us = ticks_to_us(off_ticks);
        s.delta_us = ticks_to_us(dly_ticks);
        s.d1 = d1;
        s.d2 = d2;
        printf("round %d: out=%lldus back=%lldus  offset=%lldus delay=%lldus\n", round, (long long)d1, (long long)d2,
               (long long)s.theta_us, (long long)s.delta_us);
        /* the true offset is SERVER_BIAS - bias; error is bounded by half the round trip delay */
        int64_t truth = SERVER_BIAS_US - bias;
        int64_t err = s.theta_us - truth;
        CHECK(err <= s.delta_us / 2 + 1 && -err <= s.delta_us / 2 + 1);
        CHECK(s.delta_us >= d1 + d2 - 1 && s.delta_us <= d1 + d2 + 1);
        if (s.delta_us < best.delta_us) best = s;
        world_us += 50000; /* poll interval */
    }
    printf("best sample: offset=%lldus delay=%lldus (out=%lld back=%lld)\n", (long long)best.theta_us,
           (long long)best.delta_us, (long long)best.d1, (long long)best.d2);
    int64_t truth = SERVER_BIAS_US - bias;
    bias += best.theta_us;
    int64_t resid = SERVER_BIAS_US - bias;
    printf("true offset=%lldus, residual after correction=%lldus, spoofed replies rejected=%d\n", (long long)truth,
           (long long)resid, spoofs_rejected);
    CHECK(resid == (best.d2 - best.d1) / 2 || resid == (best.d2 - best.d1) / 2 + 1 || resid == (best.d2 - best.d1) / 2 - 1);
    CHECK(llabs((long long)resid) <= best.delta_us / 2 + 1);
    CHECK(spoofs_rejected == 3);
    printf("client clock now within %lldus of server\n", (long long)llabs((long long)(client_clock(bias) - server_clock())));
    close(sfd);
    close(cfd);
    close(xfd);
    return 0;
}
