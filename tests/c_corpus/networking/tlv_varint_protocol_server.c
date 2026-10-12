/*
 * title: Varint TLV messages over TCP (protobuf-style wire format)
 * topic: networking
 * covers: LEB128 varints, zigzag, field keys and wire types, nested messages, unknown-field skipping, malformed input
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


typedef struct { unsigned char b[512]; size_t n; } Buf;

static void put_varint(Buf *w, uint64_t v) {
    while (v >= 0x80) {
        w->b[w->n++] = (unsigned char)(v | 0x80);
        v >>= 7;
    }
    w->b[w->n++] = (unsigned char)v;
}
static uint64_t zigzag(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v < 0 ? -1 : 0); }
static int64_t unzigzag(uint64_t u) { return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); }
static void put_key(Buf *w, unsigned field, unsigned wt) { put_varint(w, (uint64_t)field << 3 | wt); }
static void put_uint(Buf *w, unsigned field, uint64_t v) { put_key(w, field, 0); put_varint(w, v); }
static void put_sint(Buf *w, unsigned field, int64_t v) { put_key(w, field, 0); put_varint(w, zigzag(v)); }
static void put_bytes(Buf *w, unsigned field, const void *d, size_t n) {
    put_key(w, field, 2);
    put_varint(w, n);
    memcpy(w->b + w->n, d, n);
    w->n += n;
}
static void put_fixed32(Buf *w, unsigned field, uint32_t v) {
    put_key(w, field, 5);
    for (int i = 0; i < 4; i++) w->b[w->n++] = (unsigned char)(v >> (8 * i));
}
static void put_fixed64(Buf *w, unsigned field, uint64_t v) {
    put_key(w, field, 1);
    for (int i = 0; i < 8; i++) w->b[w->n++] = (unsigned char)(v >> (8 * i));
}

typedef struct { const unsigned char *p, *end; int err; } Cur;

static uint64_t get_varint(Cur *c) {
    uint64_t v = 0;
    for (int i = 0; i < 10; i++) {
        if (c->p >= c->end) { c->err = 1; return 0; }
        unsigned char b = *c->p++;
        if (i == 9 && b > 1) { c->err = 1; return 0; } /* would overflow 64 bits */
        v |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) return v;
    }
    c->err = 1;
    return 0;
}

/* skips a field of the given wire type; returns 0 ok */
static int skip_field(Cur *c, unsigned wt) {
    size_t n;
    switch (wt) {
    case 0: get_varint(c); return c->err ? -1 : 0;
    case 1: n = 8; break;
    case 5: n = 4; break;
    case 2: n = (size_t)get_varint(c); if (c->err) return -1; break;
    default: c->err = 1; return -1;
    }
    if ((size_t)(c->end - c->p) < n) { c->err = 1; return -1; }
    c->p += n;
    return 0;
}

#define MAXARGS 8
typedef struct {
    uint64_t id;
    char op[16];
    int64_t args[MAXARGS];
    int nargs;
    int ntags;
    uint64_t deadline_ms, trace;
    int unknown_skipped;
    int bad;
} Req;

static void decode_meta(Cur c, Req *r) {
    while (c.p < c.end && !c.err) {
        uint64_t key = get_varint(&c);
        unsigned f = (unsigned)(key >> 3), wt = (unsigned)(key & 7);
        if (f == 1 && wt == 0) r->deadline_ms = get_varint(&c);
        else if (f == 2 && wt == 1) {
            if (c.end - c.p < 8) { c.err = 1; break; }
            r->trace = 0;
            for (int i = 0; i < 8; i++) r->trace |= (uint64_t)c.p[i] << (8 * i);
            c.p += 8;
        } else if (skip_field(&c, wt) < 0) break;
    }
    if (c.err) r->bad = 1;
}

static void decode_req(const unsigned char *m, size_t n, Req *r) {
    memset(r, 0, sizeof *r);
    Cur c = {m, m + n, 0};
    while (c.p < c.end && !c.err) {
        uint64_t key = get_varint(&c);
        unsigned f = (unsigned)(key >> 3), wt = (unsigned)(key & 7);
        if (c.err) break;
        if (f == 1 && wt == 0) r->id = get_varint(&c);
        else if (f == 2 && wt == 2) {
            size_t l = (size_t)get_varint(&c);
            if (c.err || l >= sizeof r->op || (size_t)(c.end - c.p) < l) { c.err = 1; break; }
            memcpy(r->op, c.p, l);
            r->op[l] = 0;
            c.p += l;
        } else if (f == 3 && wt == 2) { /* packed repeated sint64 */
            size_t l = (size_t)get_varint(&c);
            if (c.err || (size_t)(c.end - c.p) < l) { c.err = 1; break; }
            Cur pc = {c.p, c.p + l, 0};
            while (pc.p < pc.end && !pc.err) {
                uint64_t z = get_varint(&pc);
                if (r->nargs < MAXARGS) r->args[r->nargs++] = unzigzag(z);
            }
            if (pc.err) c.err = 1;
            c.p += l;
        } else if (f == 4 && wt == 2) { /* repeated string: only counted */
            if (skip_field(&c, 2) == 0) r->ntags++;
        } else if (f == 5 && wt == 2) {
            size_t l = (size_t)get_varint(&c);
            if (c.err || (size_t)(c.end - c.p) < l) { c.err = 1; break; }
            Cur mc = {c.p, c.p + l, 0};
            decode_meta(mc, r);
            c.p += l;
        } else if (f <= 5 && f >= 1) {
            c.err = 1; /* known field with the wrong wire type */
        } else {
            if (skip_field(&c, wt) == 0) r->unknown_skipped++;
        }
    }
    if (c.err) r->bad = 1;
}

typedef struct { int lfd; int handled, bad; } Server;

static uint64_t rd_len(Conn *c, int *ok) {
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) {
        int b = conn_getc(c);
        if (b < 0) { *ok = 0; return 0; }
        v |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) { *ok = 1; return v; }
    }
    *ok = 0;
    return 0;
}

static void *server_main(void *arg) {
    Server *s = arg;
    int fd = accept_lo(s->lfd);
    if (fd < 0) return NULL;
    Conn c;
    conn_init(&c, fd);
    for (;;) {
        int ok;
        uint64_t len = rd_len(&c, &ok);
        if (!ok || len > 400) break;
        unsigned char m[400];
        if (conn_readn(&c, m, (size_t)len) < 0) break;
        Req r;
        decode_req(m, (size_t)len, &r);
        s->handled++;
        Buf resp, framed;
        resp.n = framed.n = 0;
        put_uint(&resp, 1, r.id);
        if (r.bad) {
            s->bad++;
            put_uint(&resp, 2, 2);
            put_bytes(&resp, 4, "malformed request", 17);
        } else if (!strcmp(r.op, "sum") || !strcmp(r.op, "minmax") || !strcmp(r.op, "mul")) {
            int64_t a = 0, mn = INT64_MAX, mx = INT64_MIN;
            uint64_t prod = 1;
            for (int i = 0; i < r.nargs; i++) {
                a = (int64_t)((uint64_t)a + (uint64_t)r.args[i]);
                if (r.args[i] < mn) mn = r.args[i];
                if (r.args[i] > mx) mx = r.args[i];
                prod *= (uint64_t)r.args[i];
            }
            put_uint(&resp, 2, 0);
            put_sint(&resp, 3, !strcmp(r.op, "sum") ? a : !strcmp(r.op, "mul") ? (int64_t)prod : (mx - mn));
            char note[96];
            int nl = snprintf(note, sizeof note, "%d args, %d tags, deadline %llu, trace %llx, %d unknown", r.nargs, r.ntags,
                              (unsigned long long)r.deadline_ms, (unsigned long long)r.trace, r.unknown_skipped);
            put_bytes(&resp, 4, note, (size_t)nl);
        } else {
            put_uint(&resp, 2, 1);
            put_bytes(&resp, 4, "unknown op", 10);
        }
        put_varint(&framed, resp.n);
        memcpy(framed.b + framed.n, resp.b, resp.n);
        framed.n += resp.n;
        send_all(fd, framed.b, framed.n);
    }
    close(fd);
    return NULL;
}

static void show_hex(const unsigned char *d, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x%s", d[i], i + 1 < n ? " " : "");
}

static void roundtrip_tests(void) {
    /* protobuf documentation vectors */
    Buf b;
    b.n = 0;
    put_uint(&b, 1, 150);
    printf("field 1 = 150: ");
    show_hex(b.b, b.n);
    printf("\n");
    CHECK(b.n == 3 && b.b[0] == 0x08 && b.b[1] == 0x96 && b.b[2] == 0x01);
    b.n = 0;
    put_varint(&b, 300);
    CHECK(b.n == 2 && b.b[0] == 0xAC && b.b[1] == 0x02);
    b.n = 0;
    put_varint(&b, UINT64_MAX);
    printf("UINT64_MAX: ");
    show_hex(b.b, b.n);
    printf("\n");
    CHECK(b.n == 10 && b.b[9] == 0x01);
    const int64_t zs[] = {0, -1, 1, -2, 2, 2147483647, -2147483648LL};
    const uint64_t zw[] = {0, 1, 2, 3, 4, 4294967294ULL, 4294967295ULL};
    for (int i = 0; i < 7; i++) {
        CHECK(zigzag(zs[i]) == zw[i] && unzigzag(zw[i]) == zs[i]);
    }
    printf("zigzag vectors ok\n");
    uint64_t rs = 77;
    int checked = 0;
    for (int i = 0; i < 2000; i++) {
        uint64_t hi = rng32(&rs), lo = rng32(&rs);
        uint64_t v = (hi << 32 | lo) >> (rng32(&rs) % 64);
        b.n = 0;
        put_varint(&b, v);
        Cur c = {b.b, b.b + b.n, 0};
        CHECK(get_varint(&c) == v && !c.err && c.p == c.end);
        CHECK(unzigzag(zigzag((int64_t)v)) == (int64_t)v);
        checked++;
    }
    /* over-long encoding must be rejected */
    unsigned char bad[11];
    memset(bad, 0xFF, sizeof bad);
    Cur c = {bad, bad + 11, 0};
    get_varint(&c);
    CHECK(c.err);
    unsigned char trunc[2] = {0x80, 0x80};
    Cur c2 = {trunc, trunc + 2, 0};
    get_varint(&c2);
    CHECK(c2.err);
    printf("random roundtrips: %d, overlong and truncated varints rejected\n", checked);
}

static void call(Conn *c, Buf *req, const char *label) {
    Buf f;
    f.n = 0;
    put_varint(&f, req->n);
    memcpy(f.b + f.n, req->b, req->n);
    f.n += req->n;
    CHECK(send_all(c->fd, f.b, f.n) == 0);
    int ok;
    uint64_t len = rd_len(c, &ok);
    CHECK(ok && len < 300);
    unsigned char m[300];
    CHECK(conn_readn(c, m, (size_t)len) == 0);
    Cur cur = {m, m + len, 0};
    uint64_t id = 0, status = 99;
    int64_t result = 0;
    char msg[100] = "";
    while (cur.p < cur.end && !cur.err) {
        uint64_t key = get_varint(&cur);
        unsigned fl = (unsigned)(key >> 3);
        if (fl == 1) id = get_varint(&cur);
        else if (fl == 2) status = get_varint(&cur);
        else if (fl == 3) result = unzigzag(get_varint(&cur));
        else if (fl == 4) {
            size_t l = (size_t)get_varint(&cur);
            memcpy(msg, cur.p, l);
            msg[l] = 0;
            cur.p += l;
        } else break;
    }
    printf("%-22s req %zu bytes -> id=%llu status=%llu result=%lld [%s]\n", label, req->n, (unsigned long long)id,
           (unsigned long long)status, (long long)result, msg);
}

int main(void) {
    net_init();
    roundtrip_tests();
    Server s = {0};
    int port;
    s.lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, &s)) die("thread");
    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);

    Buf r, packed, meta;
    r.n = packed.n = meta.n = 0;
    put_uint(&r, 1, 1001);
    put_bytes(&r, 2, "sum", 3);
    const int64_t args[] = {5, -12, 300, -1, 70000, INT32_MIN};
    for (size_t i = 0; i < 6; i++) put_varint(&packed, zigzag(args[i]));
    put_bytes(&r, 3, packed.b, packed.n);
    put_bytes(&r, 4, "alpha", 5);
    put_bytes(&r, 4, "beta", 4);
    put_uint(&meta, 1, 2500);
    put_fixed64(&meta, 2, 0xDEADBEEFCAFEF00DULL);
    put_bytes(&r, 5, meta.b, meta.n);
    printf("request 1001 wire: ");
    show_hex(r.b, r.n < 24 ? r.n : 24);
    printf(" ...\n");
    call(&c, &r, "sum with meta");

    /* a request with unknown fields of three wire types: they are skipped and counted */
    r.n = 0;
    put_uint(&r, 1, 1002);
    put_bytes(&r, 2, "minmax", 6);
    put_bytes(&r, 3, packed.b, packed.n);
    put_fixed32(&r, 15, 0x11223344);
    put_bytes(&r, 16, "future extension", 16);
    put_uint(&r, 17, 1ULL << 40);
    call(&c, &r, "minmax + unknown fields");

    r.n = 0;
    put_uint(&r, 1, 1003);
    put_bytes(&r, 2, "explode", 7);
    call(&c, &r, "unknown op");

    /* wrong wire type for field 2 (op given as varint) */
    r.n = 0;
    put_uint(&r, 1, 1004);
    put_uint(&r, 2, 5);
    call(&c, &r, "wrong wire type");

    /* truncated: length says 10 bytes but only 3 follow */
    r.n = 0;
    put_uint(&r, 1, 1005);
    put_key(&r, 2, 2);
    put_varint(&r, 10);
    r.b[r.n++] = 's'; r.b[r.n++] = 'u'; r.b[r.n++] = 'm';
    call(&c, &r, "truncated string");

    /* runaway varint */
    r.n = 0;
    put_uint(&r, 1, 1006);
    put_key(&r, 3, 2);
    put_varint(&r, 11);
    memset(r.b + r.n, 0xFF, 11);
    r.n += 11;
    call(&c, &r, "runaway varint");

    r.n = 0;
    put_uint(&r, 1, 1007);
    put_bytes(&r, 2, "mul", 3);
    Buf p2;
    p2.n = 0;
    for (int i = 1; i <= 5; i++) put_varint(&p2, zigzag(i * (i % 2 ? -1 : 1)));
    put_bytes(&r, 3, p2.b, p2.n);
    call(&c, &r, "mul of -1,2,-3,4,-5");
    close(fd);
    pthread_join(th, NULL);
    close(s.lfd);
    printf("server handled %d requests, %d malformed\n", s.handled, s.bad);
    return 0;
}
