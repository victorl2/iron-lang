/*
 * title: DNS truncation (TC bit) and TCP fallback
 * topic: networking
 * covers: 512-byte UDP limit, TC flag, two-byte TCP length framing, resolver retry, split TCP reads, compression pointers
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

/* ---- DNS wire helpers: name compression, parsing, zone answering ---- */
#define ZONE_ORIGIN "iron.test"
enum { DT_A = 1, DT_CNAME = 5, DT_MX = 15, DT_TXT = 16 };

typedef struct {
    char name[64];
    int type;
    unsigned ttl;
    char data[300]; /* A: dotted quad, CNAME: name, MX: "pref name", TXT: text */
} ZRec;

typedef struct {
    unsigned char b[4096];
    size_t n;
    struct { size_t off; char name[64]; } tab[48];
    int nt;
} DnsW;

const char *dns_type_name(int t) {
    switch (t) {
    case DT_A: return "A";
    case DT_CNAME: return "CNAME";
    case DT_MX: return "MX";
    case DT_TXT: return "TXT";
    default: return "?";
    }
}

void dw_u8(DnsW *w, unsigned v) { if (w->n < sizeof w->b) w->b[w->n++] = (unsigned char)v; }
void dw_u16(DnsW *w, unsigned v) { dw_u8(w, v >> 8); dw_u8(w, v & 255); }
void dw_u32(DnsW *w, unsigned v) { dw_u16(w, v >> 16); dw_u16(w, v & 0xFFFF); }

/* writes a domain name, using compression pointers to earlier suffixes when allowed */
void dw_name(DnsW *w, const char *name, int compress) {
    const char *p = name;
    while (*p) {
        if (compress)
            for (int i = 0; i < w->nt; i++)
                if (strcasecmp(w->tab[i].name, p) == 0) {
                    dw_u16(w, 0xC000u | (unsigned)w->tab[i].off);
                    return;
                }
        if (w->n < 0x3FFF && w->nt < 48) {
            w->tab[w->nt].off = w->n;
            snprintf(w->tab[w->nt].name, sizeof w->tab[0].name, "%s", p);
            w->nt++;
        }
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        dw_u8(w, (unsigned)l);
        for (size_t i = 0; i < l; i++) dw_u8(w, (unsigned char)p[i]);
        p += l;
        if (*p == '.') p++;
    }
    dw_u8(w, 0);
}

/* decodes a possibly compressed name at *pos; returns 0 ok, -1 bad */
int dr_name(const unsigned char *m, size_t len, size_t *pos, char *out, size_t cap) {
    size_t p = *pos, o = 0;
    int jumped = 0, hops = 0;
    for (;;) {
        if (p >= len) return -1;
        unsigned l = m[p];
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= len) return -1;
            size_t target = ((l & 0x3F) << 8) | m[p + 1];
            if (!jumped) *pos = p + 2;
            jumped = 1;
            if (++hops > 16 || target >= len) return -1;
            p = target;
            continue;
        }
        if (l & 0xC0) return -1;
        p++;
        if (l == 0) break;
        if (p + l > len || o + l + 2 > cap) return -1;
        if (o) out[o++] = '.';
        memcpy(out + o, m + p, l);
        o += l;
        p += l;
    }
    out[o] = 0;
    if (!jumped) *pos = p;
    return 0;
}

typedef struct {
    char name[64];
    int type;
    unsigned ttl;
    char text[300];
} Rr;

typedef struct {
    unsigned id, flags;
    int qd, an;
    char qname[64];
    int qtype;
    Rr rr[48];
    int nrr;
} DnsMsg;

/* parses header, question and answers; returns 0 ok, -1 malformed */
int dns_parse(const unsigned char *m, size_t len, DnsMsg *o) {
    memset(o, 0, sizeof *o);
    if (len < 12) return -1;
    o->id = (unsigned)(m[0] << 8 | m[1]);
    o->flags = (unsigned)(m[2] << 8 | m[3]);
    o->qd = m[4] << 8 | m[5];
    o->an = m[6] << 8 | m[7];
    size_t pos = 12;
    for (int i = 0; i < o->qd; i++) {
        char nm[64];
        if (dr_name(m, len, &pos, nm, sizeof nm) < 0 || pos + 4 > len) return -1;
        if (i == 0) {
            snprintf(o->qname, sizeof o->qname, "%s", nm);
            o->qtype = m[pos] << 8 | m[pos + 1];
        }
        pos += 4;
    }
    for (int i = 0; i < o->an; i++) {
        Rr *r = &o->rr[o->nrr];
        if (o->nrr >= 48) return -1;
        if (dr_name(m, len, &pos, r->name, sizeof r->name) < 0 || pos + 10 > len) return -1;
        r->type = m[pos] << 8 | m[pos + 1];
        r->ttl = (unsigned)m[pos + 4] << 24 | (unsigned)m[pos + 5] << 16 | (unsigned)m[pos + 6] << 8 | m[pos + 7];
        size_t rdl = (size_t)(m[pos + 8] << 8 | m[pos + 9]);
        pos += 10;
        if (pos + rdl > len) return -1;
        size_t rp = pos;
        if (r->type == DT_A && rdl == 4) snprintf(r->text, sizeof r->text, "%u.%u.%u.%u", m[rp], m[rp + 1], m[rp + 2], m[rp + 3]);
        else if (r->type == DT_CNAME) { if (dr_name(m, len, &rp, r->text, sizeof r->text) < 0) return -1; }
        else if (r->type == DT_MX) {
            char nm[64];
            size_t np = rp + 2;
            if (dr_name(m, len, &np, nm, sizeof nm) < 0) return -1;
            snprintf(r->text, sizeof r->text, "%u %s", (unsigned)(m[rp] << 8 | m[rp + 1]), nm);
        } else if (r->type == DT_TXT && rdl >= 1) {
            /* concatenate character-strings */
            size_t o2 = 0, q = rp;
            while (q < rp + rdl) {
                size_t l = m[q++];
                if (q + l > rp + rdl || o2 + l + 1 > sizeof r->text) return -1;
                memcpy(r->text + o2, m + q, l);
                o2 += l;
                q += l;
            }
            r->text[o2] = 0;
        } else
            snprintf(r->text, sizeof r->text, "?");
        pos += rdl;
        o->nrr++;
    }
    return 0;
}

size_t dns_query(unsigned char *out, unsigned id, const char *name, int type, int opcode) {
    DnsW w;
    memset(&w, 0, sizeof w);
    dw_u16(&w, id);
    dw_u16(&w, 0x0100u | ((unsigned)opcode << 11)); /* RD */
    dw_u16(&w, 1); dw_u16(&w, 0); dw_u16(&w, 0); dw_u16(&w, 0);
    dw_name(&w, name, 0);
    dw_u16(&w, (unsigned)type);
    dw_u16(&w, 1);
    memcpy(out, w.b, w.n);
    return w.n;
}

static int ends_with_origin(const char *n) {
    size_t l = strlen(n), o = strlen(ZONE_ORIGIN);
    if (l < o || strcasecmp(n + l - o, ZONE_ORIGIN) != 0) return 0;
    return l == o || n[l - o - 1] == '.';
}

/* answers a query from the zone. udp_limit==0 means no truncation. */
size_t dns_answer(const ZRec *z, int nz, const unsigned char *q, size_t ql, unsigned char *out, size_t udp_limit) {
    DnsMsg m;
    unsigned rcode = 0, aa = 1, tc = 0;
    int n_ans = 0;
    DnsW w;
    memset(&w, 0, sizeof w);
    unsigned id = ql >= 2 ? (unsigned)(q[0] << 8 | q[1]) : 0;
    ZRec ans[48];
    int have_q = dns_parse(q, ql, &m) == 0 && m.qd == 1;
    unsigned opcode = ql >= 3 ? (unsigned)((q[2] >> 3) & 15) : 0;
    if (!have_q) { rcode = 1; aa = 0; }
    else if (opcode != 0) { rcode = 4; aa = 0; }
    else if (!ends_with_origin(m.qname)) { rcode = 5; aa = 0; }
    else {
        char cur[64];
        snprintf(cur, sizeof cur, "%s", m.qname);
        int exists = 0;
        for (int hop = 0; hop < 5; hop++) {
            int cname_idx = -1;
            for (int i = 0; i < nz; i++) {
                if (strcasecmp(z[i].name, cur) != 0) continue;
                exists = 1;
                if (z[i].type == m.qtype) { if (n_ans < 48) ans[n_ans++] = z[i]; }
                else if (z[i].type == DT_CNAME) cname_idx = i;
            }
            int matched = 0;
            for (int i = 0; i < n_ans; i++) matched |= !strcasecmp(ans[i].name, cur) && ans[i].type == m.qtype;
            if (cname_idx >= 0 && !matched && m.qtype != DT_CNAME) {
                if (n_ans < 48) ans[n_ans++] = z[cname_idx];
                snprintf(cur, sizeof cur, "%s", z[cname_idx].data);
                if (!ends_with_origin(cur)) break;
                continue;
            }
            break;
        }
        if (!exists) rcode = 3;
    }
    dw_u16(&w, id);
    dw_u16(&w, 0x8000u | ((unsigned)opcode << 11) | (aa ? 0x0400u : 0) | 0x0100u | 0x0080u | rcode);
    dw_u16(&w, have_q ? 1 : 0); dw_u16(&w, 0); dw_u16(&w, 0); dw_u16(&w, 0);
    size_t an_off = 6;
    if (have_q) {
        dw_name(&w, m.qname, 1);
        dw_u16(&w, (unsigned)m.qtype);
        dw_u16(&w, 1);
    }
    for (int i = 0; i < n_ans; i++) {
        dw_name(&w, ans[i].name, 1);
        dw_u16(&w, (unsigned)ans[i].type);
        dw_u16(&w, 1);
        dw_u32(&w, ans[i].ttl);
        size_t lenpos = w.n;
        dw_u16(&w, 0);
        if (ans[i].type == DT_A) {
            unsigned a, b, c, d;
            sscanf(ans[i].data, "%u.%u.%u.%u", &a, &b, &c, &d);
            dw_u8(&w, a); dw_u8(&w, b); dw_u8(&w, c); dw_u8(&w, d);
        } else if (ans[i].type == DT_CNAME) dw_name(&w, ans[i].data, 1);
        else if (ans[i].type == DT_MX) {
            unsigned pref;
            char nm[64];
            sscanf(ans[i].data, "%u %63s", &pref, nm);
            dw_u16(&w, pref);
            dw_name(&w, nm, 1);
        } else {
            size_t l = strlen(ans[i].data);
            size_t off = 0;
            while (off < l) { /* split into <=255 byte character-strings */
                size_t k = l - off > 255 ? 255 : l - off;
                dw_u8(&w, (unsigned)k);
                for (size_t j = 0; j < k; j++) dw_u8(&w, (unsigned char)ans[i].data[off + j]);
                off += k;
            }
        }
        size_t rdl = w.n - lenpos - 2;
        w.b[lenpos] = (unsigned char)(rdl >> 8);
        w.b[lenpos + 1] = (unsigned char)rdl;
    }
    w.b[an_off] = (unsigned char)(n_ans >> 8);
    w.b[an_off + 1] = (unsigned char)n_ans;
    if (udp_limit && w.n > udp_limit) {
        /* truncate: keep header + question, set TC, no answers */
        tc = 1;
        size_t qend = 12;
        if (have_q) { size_t p = 12; char tmp[64]; dr_name(w.b, w.n, &p, tmp, sizeof tmp); qend = p + 4; }
        w.n = qend;
        w.b[6] = w.b[7] = 0;
        w.b[2] |= 0x02;
    }
    (void)tc;
    memcpy(out, w.b, w.n);
    return w.n;
}


static ZRec zone[80];
static int nz;

static void add_rec(const char *name, int type, unsigned ttl, const char *data) {
    ZRec *z = &zone[nz++];
    snprintf(z->name, sizeof z->name, "%s", name);
    z->type = type;
    z->ttl = ttl;
    snprintf(z->data, sizeof z->data, "%s", data);
}

typedef struct {
    int ufd, lfd;
    int udp_seen, tcp_conns, tcp_queries;
    int udp_truncated;
} Server;

static void *server_main(void *arg) {
    Server *s = arg;
    const int want_udp = 6, want_tcp = 4;
    while (s->udp_seen < want_udp || s->tcp_conns < want_tcp) {
        struct pollfd p[2] = {{s->ufd, POLLIN, 0}, {s->lfd, POLLIN, 0}};
        if (poll(p, 2, 4000) <= 0) break;
        if (p[0].revents & POLLIN) {
            unsigned char q[600], r[4096];
            int from;
            int n = udp_recv(s->ufd, q, sizeof q, &from);
            if (n < 0) continue;
            s->udp_seen++;
            size_t rn = dns_answer(zone, nz, q, (size_t)n, r, 512);
            if (r[2] & 0x02) s->udp_truncated++;
            udp_send(s->ufd, from, r, rn);
        }
        if (p[1].revents & POLLIN) {
            int fd = accept_lo(s->lfd);
            if (fd < 0) continue;
            s->tcp_conns++;
            Conn c;
            conn_init(&c, fd);
            for (;;) {
                unsigned char lb[2];
                if (conn_readn(&c, lb, 2) < 0) break;
                size_t ql = (size_t)(lb[0] << 8 | lb[1]);
                unsigned char q[600], r[4096 + 2];
                if (ql > sizeof q || conn_readn(&c, q, ql) < 0) break;
                s->tcp_queries++;
                size_t rn = dns_answer(zone, nz, q, ql, r + 2, 0); /* no truncation over TCP */
                r[0] = (unsigned char)(rn >> 8);
                r[1] = (unsigned char)rn;
                send_all(fd, r, rn + 2);
            }
            close(fd);
        }
    }
    return NULL;
}

typedef struct { int via_tcp; size_t udp_size, final_size; DnsMsg m; } Result;

static void resolve(int uport, int tport, unsigned id, const char *name, int type, Result *out, DnsMsg *scratch) {
    int cport;
    int cfd = udp_lo(&cport, 2000);
    unsigned char q[512], r[4096];
    size_t ql = dns_query(q, id, name, type, 0);
    CHECK(udp_send(cfd, uport, q, ql) == (int)ql);
    int rn = udp_recv(cfd, r, sizeof r, NULL);
    CHECK(rn > 0);
    close(cfd);
    out->udp_size = (size_t)rn;
    out->via_tcp = 0;
    CHECK(dns_parse(r, (size_t)rn, scratch) == 0);
    if (scratch->flags & 0x0200) { /* TC */
        int fd = connect_lo(tport);
        Conn c;
        conn_init(&c, fd);
        unsigned char frame[520];
        frame[0] = (unsigned char)(ql >> 8);
        frame[1] = (unsigned char)ql;
        memcpy(frame + 2, q, ql);
        /* trickle the length prefix to exercise partial reads on the server */
        CHECK(send_all(fd, frame, 1) == 0);
        CHECK(send_all(fd, frame + 1, ql + 1) == 0);
        unsigned char lb[2];
        CHECK(conn_readn(&c, lb, 2) == 0);
        size_t tl = (size_t)(lb[0] << 8 | lb[1]);
        CHECK(tl <= sizeof r && conn_readn(&c, r, tl) == 0);
        close(fd);
        out->via_tcp = 1;
        rn = (int)tl;
        CHECK(dns_parse(r, (size_t)rn, scratch) == 0);
    }
    out->final_size = (size_t)rn;
    CHECK(scratch->id == id);
}

int main(void) {
    net_init();
    char data[260];
    add_rec("small.iron.test", DT_A, 60, "192.0.2.1");
    add_rec("small.iron.test", DT_A, 60, "192.0.2.2");
    for (int i = 0; i < 40; i++) {
        snprintf(data, sizeof data, "198.51.100.%d", i + 1);
        add_rec("big.iron.test", DT_A, 300, data);
    }
    for (size_t i = 0; i < 3; i++) {
        memset(data, 'a' + (int)i, 200);
        data[200] = 0;
        add_rec("txt.iron.test", DT_TXT, 30, data);
    }
    memset(data, 'z', 259);
    data[259] = 0; /* one TXT record whose text needs two character-strings */
    add_rec("long.iron.test", DT_TXT, 30, data);
    add_rec("alias.iron.test", DT_CNAME, 30, "big.iron.test");

    Server s;
    memset(&s, 0, sizeof s);
    int uport, tport;
    s.ufd = udp_lo(&uport, 2000);
    s.lfd = listen_lo(&tport);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, &s)) die("thread");

    struct { const char *name; int type; } qs[] = {
        {"small.iron.test", DT_A}, {"big.iron.test", DT_A}, {"txt.iron.test", DT_TXT},
        {"long.iron.test", DT_TXT}, {"alias.iron.test", DT_A}, {"nothing.iron.test", DT_A},
    };
    DnsMsg *m = malloc(sizeof *m);
    if (!m) die("oom");
    for (int i = 0; i < 6; i++) {
        Result r;
        resolve(uport, tport, (unsigned)(0x2000 + i), qs[i].name, qs[i].type, &r, m);
        printf("%-18s %-3s udp=%3zuB %s", qs[i].name, dns_type_name(qs[i].type), r.udp_size, r.via_tcp ? "TC -> tcp " : "complete  ");
        printf("answers=%d final=%zuB rcode=%u\n", m->nrr, r.final_size, m->flags & 15);
        if (m->nrr) printf("    first: %s %s %.24s%s\n", m->rr[0].name, dns_type_name(m->rr[0].type), m->rr[0].text, strlen(m->rr[0].text) > 24 ? "..." : "");
        if (m->nrr > 1) printf("    last:  %s %s %.24s\n", m->rr[m->nrr - 1].name, dns_type_name(m->rr[m->nrr - 1].type), m->rr[m->nrr - 1].text);
        if (i == 3) CHECK(strlen(m->rr[0].text) == 259); /* two character-strings joined */
        if (i == 1) CHECK(m->nrr == 40);
    }
    /* several queries pipelined on one TCP connection */
    int fd = connect_lo(tport);
    Conn c;
    conn_init(&c, fd);
    unsigned char wire[600];
    size_t w = 0;
    const char *names[3] = {"small.iron.test", "big.iron.test", "nothing.iron.test"};
    for (int i = 0; i < 3; i++) {
        unsigned char q[300];
        size_t ql = dns_query(q, (unsigned)(0x3000 + i), names[i], DT_A, 0);
        wire[w++] = (unsigned char)(ql >> 8);
        wire[w++] = (unsigned char)ql;
        memcpy(wire + w, q, ql);
        w += ql;
    }
    CHECK(send_all(fd, wire, w) == 0);
    for (int i = 0; i < 3; i++) {
        unsigned char lb[2], r[4096];
        CHECK(conn_readn(&c, lb, 2) == 0);
        size_t tl = (size_t)(lb[0] << 8 | lb[1]);
        CHECK(conn_readn(&c, r, tl) == 0);
        CHECK(dns_parse(r, tl, m) == 0);
        printf("tcp pipelined %-18s id=%04x answers=%d size=%zuB tc=%u\n", names[i], m->id, m->nrr, tl, (m->flags >> 9) & 1);
    }
    close(fd);
    pthread_join(th, NULL);
    close(s.ufd);
    close(s.lfd);
    free(m);
    printf("server: udp=%d truncated=%d tcp_conns=%d tcp_queries=%d\n", s.udp_seen, s.udp_truncated, s.tcp_conns, s.tcp_queries);
    return 0;
}
