/*
 * title: Length-prefixed binary RPC with marshalled structs
 * topic: networking
 * covers: frame header, big-endian marshalling, bounds-checked reader, nested structs, status codes, call ids
 * deps: libc, posix, pthread, sockets
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


enum { M_AREA = 1, M_STATS = 2, M_GREET = 3, M_DIVIDE = 4 };
enum { ST_OK = 0, ST_NO_METHOD, ST_BAD_PAYLOAD, ST_DIV_ZERO, ST_TOO_BIG, ST_BAD_VERSION };
static const char *st_name(int s) {
    static const char *n[] = {"OK", "NO_METHOD", "BAD_PAYLOAD", "DIV_ZERO", "TOO_BIG", "BAD_VERSION"};
    return s >= 0 && s <= 5 ? n[s] : "?";
}

#define MAX_FRAME 512

/* ---- writer ---- */
typedef struct { unsigned char b[MAX_FRAME]; size_t n; } Wr;
static void w8(Wr *w, unsigned v) { if (w->n < MAX_FRAME) w->b[w->n++] = (unsigned char)v; }
static void w16(Wr *w, unsigned v) { w8(w, v >> 8); w8(w, v & 255); }
static void w32(Wr *w, uint32_t v) { w16(w, v >> 16); w16(w, v & 0xFFFF); }
static void wi32(Wr *w, int32_t v) { w32(w, (uint32_t)v); }
static void wstr(Wr *w, const char *s) { size_t l = strlen(s); w16(w, (unsigned)l); for (size_t i = 0; i < l; i++) w8(w, (unsigned char)s[i]); }

/* ---- bounds-checked reader ---- */
typedef struct { const unsigned char *p, *end; int err; } Rd;
static unsigned r8(Rd *r) { if (r->p >= r->end) { r->err = 1; return 0; } return *r->p++; }
static unsigned r16(Rd *r) { unsigned a = r8(r); unsigned b = r8(r); return a << 8 | b; }
static uint32_t r32(Rd *r) { uint32_t a = r16(r); uint32_t b = r16(r); return a << 16 | b; }
static int32_t ri32(Rd *r) { return (int32_t)r32(r); }
static void rstr(Rd *r, char *out, size_t cap) {
    size_t l = r16(r);
    if ((size_t)(r->end - r->p) < l || l >= cap) { r->err = 1; out[0] = 0; return; }
    memcpy(out, r->p, l);
    out[l] = 0;
    r->p += l;
}

typedef struct { int32_t x, y; } Point;
typedef struct { Point tl, br; } Rect;
typedef struct { char name[32]; unsigned age; unsigned n; unsigned scores[16]; } Person;

static Point rd_point(Rd *r) { Point p; p.x = ri32(r); p.y = ri32(r); return p; }
static Rect rd_rect(Rd *r) { Rect q; q.tl = rd_point(r); q.br = rd_point(r); return q; }
static Person rd_person(Rd *r) {
    Person p;
    memset(&p, 0, sizeof p);
    rstr(r, p.name, sizeof p.name);
    p.age = r8(r);
    p.n = r16(r);
    if (p.n > 16) { r->err = 1; p.n = 0; }
    for (unsigned i = 0; i < p.n; i++) p.scores[i] = r16(r);
    return p;
}
static void wr_person(Wr *w, const Person *p) {
    wstr(w, p->name);
    w8(w, p->age);
    w16(w, p->n);
    for (unsigned i = 0; i < p->n; i++) w16(w, p->scores[i]);
}

static int dispatch(unsigned method, Rd *rd, Wr *out) {
    if (method == M_AREA) {
        Rect q = rd_rect(rd);
        if (rd->err) return ST_BAD_PAYLOAD;
        int64_t w = (int64_t)q.br.x - q.tl.x, h = (int64_t)q.br.y - q.tl.y;
        int64_t a = w * h;
        if (a < 0) a = -a;
        w32(out, (uint32_t)a);
        return ST_OK;
    }
    if (method == M_STATS) {
        Person p = rd_person(rd);
        if (rd->err || p.n == 0) return ST_BAD_PAYLOAD;
        unsigned mn = p.scores[0], mx = p.scores[0], sum = 0;
        for (unsigned i = 0; i < p.n; i++) {
            if (p.scores[i] < mn) mn = p.scores[i];
            if (p.scores[i] > mx) mx = p.scores[i];
            sum += p.scores[i];
        }
        w16(out, p.n); w16(out, mn); w16(out, mx); w32(out, sum);
        return ST_OK;
    }
    if (method == M_GREET) {
        Person p = rd_person(rd);
        if (rd->err) return ST_BAD_PAYLOAD;
        char g[64];
        snprintf(g, sizeof g, "hello %s (%u)", p.name, p.age);
        wstr(out, g);
        p.age++;
        wr_person(out, &p);
        return ST_OK;
    }
    if (method == M_DIVIDE) {
        int32_t a = ri32(rd), b = ri32(rd);
        if (rd->err) return ST_BAD_PAYLOAD;
        if (b == 0) return ST_DIV_ZERO;
        if (a == INT32_MIN && b == -1) return ST_BAD_PAYLOAD;
        wi32(out, a / b);
        wi32(out, a % b);
        return ST_OK;
    }
    return ST_NO_METHOD;
}

typedef struct { int lfd; int calls; int errors; } Server;

static void *server_main(void *arg) {
    Server *s = arg;
    int fd = accept_lo(s->lfd);
    if (fd < 0) return NULL;
    Conn c;
    conn_init(&c, fd);
    for (;;) {
        unsigned char hdr[4];
        if (conn_readn(&c, hdr, 4) < 0) break;
        uint32_t len = (uint32_t)hdr[0] << 24 | (uint32_t)hdr[1] << 16 | (uint32_t)hdr[2] << 8 | hdr[3];
        Wr rep;
        rep.n = 0;
        unsigned id = 0;
        int st;
        if (len > MAX_FRAME || len < 8) {
            st = ST_TOO_BIG;
            s->errors++;
            w8(&rep, 1); w16(&rep, 0); w32(&rep, 0); w8(&rep, (unsigned)st);
            unsigned char h2[4] = {0, 0, 0, (unsigned char)rep.n};
            send_all(fd, h2, 4);
            send_all(fd, rep.b, rep.n);
            break; /* framing is lost: close */
        }
        unsigned char body[MAX_FRAME];
        if (conn_readn(&c, body, len) < 0) break;
        s->calls++;
        Rd rd = {body, body + len, 0};
        unsigned ver = r8(&rd), method = r16(&rd);
        id = r32(&rd);
        r8(&rd); /* status byte, unused in requests */
        Wr payload;
        payload.n = 0;
        st = ver != 1 ? ST_BAD_VERSION : dispatch(method, &rd, &payload);
        if (st != ST_OK) { s->errors++; payload.n = 0; }
        w8(&rep, 1); w16(&rep, method); w32(&rep, id); w8(&rep, (unsigned)st);
        for (size_t i = 0; i < payload.n; i++) w8(&rep, payload.b[i]);
        unsigned char h2[4] = {0, 0, (unsigned char)(rep.n >> 8), (unsigned char)(rep.n & 255)};
        send_all(fd, h2, 4);
        send_all(fd, rep.b, rep.n);
    }
    close(fd);
    return NULL;
}

/* ---- client ---- */
static size_t frame(unsigned char *out, unsigned ver, unsigned method, unsigned id, const Wr *payload) {
    Wr w;
    w.n = 0;
    w8(&w, ver); w16(&w, method); w32(&w, id); w8(&w, 0);
    for (size_t i = 0; i < payload->n; i++) w8(&w, payload->b[i]);
    out[0] = 0; out[1] = 0; out[2] = (unsigned char)(w.n >> 8); out[3] = (unsigned char)w.n;
    memcpy(out + 4, w.b, w.n);
    return w.n + 4;
}

/* reads one reply; returns status, fills payload reader */
static int read_reply(Conn *c, unsigned *id, unsigned char *pay, size_t *pl) {
    unsigned char h[4];
    if (conn_readn(c, h, 4) < 0) return -1;
    size_t len = (size_t)h[2] << 8 | h[3];
    unsigned char b[MAX_FRAME];
    if (len < 8 || conn_readn(c, b, len) < 0) return -1;
    Rd r = {b, b + len, 0};
    r8(&r); r16(&r);
    *id = r32(&r);
    int st = (int)r8(&r);
    *pl = (size_t)(r.end - r.p);
    memcpy(pay, r.p, *pl);
    return st;
}

int main(void) {
    net_init();
    Server s = {0};
    int port;
    s.lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, &s)) die("thread");
    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);
    unsigned char wire[1024], pay[MAX_FRAME];
    size_t wl = 0, pl;
    unsigned id;

    /* three calls pipelined in one write */
    Wr a, b, d;
    a.n = b.n = d.n = 0;
    wi32(&a, 10); wi32(&a, 20); wi32(&a, 40); wi32(&a, -5); /* rect 30 x 25 (abs) */
    Person ada = {"ada", 36, 4, {90, 75, 100, 88}};
    wr_person(&b, &ada);
    wi32(&d, -17); wi32(&d, 5);
    wl += frame(wire + wl, 1, M_AREA, 101, &a);
    wl += frame(wire + wl, 1, M_STATS, 102, &b);
    wl += frame(wire + wl, 1, M_DIVIDE, 103, &d);
    CHECK(send_all(fd, wire, wl) == 0);

    int st = read_reply(&c, &id, pay, &pl);
    Rd r = {pay, pay + pl, 0};
    uint32_t area = r32(&r);
    printf("call %u area: %s area=%u\n", id, st_name(st), area);
    CHECK(id == 101 && st == ST_OK && area == 750);
    st = read_reply(&c, &id, pay, &pl);
    r = (Rd){pay, pay + pl, 0};
    unsigned n = r16(&r), mn = r16(&r), mx = r16(&r);
    uint32_t sum = r32(&r);
    printf("call %u stats: %s n=%u min=%u max=%u sum=%u\n", id, st_name(st), n, mn, mx, sum);
    CHECK(id == 102 && n == 4 && mn == 75 && mx == 100 && sum == 353);
    st = read_reply(&c, &id, pay, &pl);
    r = (Rd){pay, pay + pl, 0};
    int32_t q = ri32(&r), rem = ri32(&r);
    printf("call %u divide: %s q=%d r=%d\n", id, st_name(st), q, rem);
    CHECK(id == 103 && q == -3 && rem == -2);

    /* error paths */
    Wr e;
    e.n = 0;
    wi32(&e, 1); wi32(&e, 0);
    wl = frame(wire, 1, M_DIVIDE, 201, &e);
    send_all(fd, wire, wl);
    st = read_reply(&c, &id, pay, &pl);
    printf("call %u divide by zero: %s payload=%zu\n", id, st_name(st), pl);
    CHECK(st == ST_DIV_ZERO);
    wl = frame(wire, 1, 77, 202, &e);
    send_all(fd, wire, wl);
    st = read_reply(&c, &id, pay, &pl);
    printf("call %u method 77: %s\n", id, st_name(st));
    CHECK(st == ST_NO_METHOD);
    e.n = 6; /* truncated rect */
    wl = frame(wire, 1, M_AREA, 203, &e);
    send_all(fd, wire, wl);
    st = read_reply(&c, &id, pay, &pl);
    printf("call %u truncated rect: %s\n", id, st_name(st));
    CHECK(st == ST_BAD_PAYLOAD);
    wl = frame(wire, 2, M_AREA, 204, &a);
    send_all(fd, wire, wl);
    st = read_reply(&c, &id, pay, &pl);
    printf("call %u version 2: %s\n", id, st_name(st));
    CHECK(st == ST_BAD_VERSION);

    /* greet: struct in, struct out; the frame trickles in one byte at a time */
    Person bob = {"bob", 41, 2, {1, 2}};
    Wr g;
    g.n = 0;
    wr_person(&g, &bob);
    wl = frame(wire, 1, M_GREET, 300, &g);
    for (size_t i = 0; i < wl; i++) CHECK(send_all(fd, wire + i, 1) == 0);
    st = read_reply(&c, &id, pay, &pl);
    r = (Rd){pay, pay + pl, 0};
    char greeting[64];
    rstr(&r, greeting, sizeof greeting);
    Person back = rd_person(&r);
    printf("call %u greet: %s \"%s\" -> age %u, %u scores, sum %u\n", id, st_name(st), greeting, back.age,
           back.n, back.scores[0] + back.scores[1]);
    CHECK(!r.err && back.age == 42 && strcmp(back.name, "bob") == 0);

    /* oversized frame header: server answers TOO_BIG and hangs up */
    unsigned char big[4] = {0, 0, 0x10, 0};
    send_all(fd, big, 4);
    st = read_reply(&c, &id, pay, &pl);
    printf("oversized frame: %s\n", st_name(st));
    CHECK(st == ST_TOO_BIG);
    CHECK(conn_getc(&c) < 0);
    close(fd);
    pthread_join(th, NULL);
    close(s.lfd);
    printf("server: calls=%d errors=%d\n", s.calls, s.errors);
    return 0;
}
