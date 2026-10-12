/*
 * title: MQTT 3.1.1 subset broker with QoS 0/1 and retained messages
 * topic: networking
 * covers: fixed header and remaining-length varint, CONNECT/CONNACK, SUBSCRIBE/SUBACK, PUBLISH QoS downgrade, retain, PING, UNSUBSCRIBE
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


static size_t enc_rl(unsigned char *o, uint32_t v) {
    size_t n = 0;
    do {
        unsigned char b = (unsigned char)(v % 128);
        v /= 128;
        if (v) b |= 0x80;
        o[n++] = b;
    } while (v);
    return n;
}

/* reads the remaining length from a connection; returns -1 on error, -2 if malformed */
static long read_rl(Conn *c) {
    long v = 0;
    for (int i = 0; i < 4; i++) {
        int b = conn_getc(c);
        if (b < 0) return -1;
        v += (long)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) return v;
    }
    return -2;
}

static int topic_match(const char *pat, const char *topic) {
    while (*pat) {
        if (*pat == '#') return pat[1] == 0;
        const char *pe = strchr(pat, '/'), *te = strchr(topic, '/');
        size_t pl = pe ? (size_t)(pe - pat) : strlen(pat), tl = te ? (size_t)(te - topic) : strlen(topic);
        if (!(pl == 1 && *pat == '+') && !(pl == tl && !strncmp(pat, topic, pl))) return 0;
        if (!pe || !te) {
            if (pe && pe[1] == '#' && !pe[2] && !te) return 1;
            return !pe && !te;
        }
        pat = pe + 1;
        topic = te + 1;
    }
    return *topic == 0;
}

static int filter_valid(const char *f) {
    for (const char *p = f; *p; p++) {
        int start = p == f || p[-1] == '/', end = p[1] == 0 || p[1] == '/';
        if (*p == '+' && !(start && end)) return 0;
        if (*p == '#' && !(start && p[1] == 0)) return 0;
    }
    return f[0] != 0;
}

#define MAXC 5
#define MAXSUB 4

typedef struct {
    Conn c;
    int live, connected;
    char id[24];
    struct { char f[32]; int qos; } sub[MAXSUB];
    int ns;
    unsigned next_pid;
} Client;

typedef struct { char topic[32]; unsigned char pay[64]; size_t n; int used; } Retained;

typedef struct {
    int lfd;
    Retained ret[4];
    int published, delivered, acks, refused;
} Broker;

static void send_pkt(int fd, int hdr, const unsigned char *body, size_t n) {
    unsigned char h[5];
    h[0] = (unsigned char)hdr;
    size_t hl = 1 + enc_rl(h + 1, (uint32_t)n);
    send_all(fd, h, hl);
    if (n) send_all(fd, body, n);
}

static void deliver(Client *c, const char *topic, const unsigned char *pay, size_t n, int qos, int retain) {
    unsigned char b[600];
    size_t o = 0;
    size_t tl = strlen(topic);
    b[o++] = (unsigned char)(tl >> 8);
    b[o++] = (unsigned char)tl;
    memcpy(b + o, topic, tl);
    o += tl;
    if (qos) {
        c->next_pid++;
        b[o++] = (unsigned char)(c->next_pid >> 8);
        b[o++] = (unsigned char)c->next_pid;
    }
    memcpy(b + o, pay, n);
    o += n;
    send_pkt(c->c.fd, 0x30 | (qos << 1) | (retain ? 1 : 0), b, o);
}

static void *broker_main(void *arg) {
    Broker *br = arg;
    Client *cl = calloc(MAXC, sizeof *cl);
    int n = 0, closed = 0;
    while (closed < MAXC) {
        struct pollfd pf[MAXC + 1];
        int map[MAXC + 1], k = 0;
        pf[k].fd = br->lfd; pf[k].events = POLLIN; pf[k].revents = 0; map[k++] = -1;
        for (int i = 0; i < n; i++)
            if (cl[i].live) { pf[k].fd = cl[i].c.fd; pf[k].events = POLLIN; pf[k].revents = 0; map[k++] = i; }
        if (poll(pf, (nfds_t)k, 5000) <= 0) break;
        for (int j = 0; j < k; j++) {
            if (!(pf[j].revents & (POLLIN | POLLHUP))) continue;
            if (map[j] < 0) {
                int fd = accept_lo(br->lfd);
                if (fd < 0 || n == MAXC) { if (fd >= 0) close(fd); continue; }
                conn_init(&cl[n].c, fd);
                cl[n++].live = 1;
                continue;
            }
            Client *me = &cl[map[j]];
            int hb = conn_getc(&me->c);
            long rl = hb < 0 ? -1 : read_rl(&me->c);
            unsigned char body[700];
            if (hb < 0 || rl < 0 || rl > (long)sizeof body || conn_readn(&me->c, body, (size_t)rl) < 0) {
                me->live = 0; close(me->c.fd); closed++;
                continue;
            }
            int type = hb >> 4;
            if (type == 1) { /* CONNECT */
                size_t o = 0;
                size_t pl = (size_t)(body[0] << 8 | body[1]);
                o = 2 + pl;
                int level = body[o++], flags = body[o++];
                o += 2; /* keepalive */
                size_t il = (size_t)(body[o] << 8 | body[o + 1]);
                o += 2;
                char id[24] = "";
                memcpy(id, body + o, il < 23 ? il : 23);
                o += il;
                char user[16] = "", pass[16] = "";
                if (flags & 0x80) { size_t ul = (size_t)(body[o] << 8 | body[o + 1]); memcpy(user, body + o + 2, ul); o += 2 + ul; }
                if (flags & 0x40) { size_t pl2 = (size_t)(body[o] << 8 | body[o + 1]); memcpy(pass, body + o + 2, pl2); o += 2 + pl2; }
                unsigned char rc = 0;
                if (level != 4) rc = 1;
                else if (il == 0 && !(flags & 0x02)) rc = 2;
                else if ((flags & 0x80) && (strcmp(user, "iron") || strcmp(pass, "pw"))) rc = 4;
                unsigned char ack[2] = {0, rc};
                send_pkt(me->c.fd, 0x20, ack, 2);
                if (rc) { br->refused++; me->live = 0; close(me->c.fd); closed++; continue; }
                me->connected = 1;
                snprintf(me->id, sizeof me->id, "%s", id);
            } else if (!me->connected) {
                me->live = 0; close(me->c.fd); closed++;
            } else if (type == 8) { /* SUBSCRIBE */
                unsigned char sack[8];
                size_t sn = 2, o = 2;
                sack[0] = body[0]; sack[1] = body[1];
                while (o < (size_t)rl) {
                    size_t fl = (size_t)(body[o] << 8 | body[o + 1]);
                    char f[32] = "";
                    memcpy(f, body + o + 2, fl < 31 ? fl : 31);
                    int q = body[o + 2 + fl];
                    o += 3 + fl;
                    if (!filter_valid(f) || me->ns == MAXSUB) { sack[sn++] = 0x80; continue; }
                    int g = q > 1 ? 1 : q;
                    snprintf(me->sub[me->ns].f, 32, "%s", f);
                    me->sub[me->ns++].qos = g;
                    sack[sn++] = (unsigned char)g;
                }
                send_pkt(me->c.fd, 0x90, sack, sn);
                /* now retained messages for the newly added filters */
                o = 2;
                while (o < (size_t)rl) {
                    size_t fl = (size_t)(body[o] << 8 | body[o + 1]);
                    char f[32] = "";
                    memcpy(f, body + o + 2, fl < 31 ? fl : 31);
                    int q = body[o + 2 + fl];
                    o += 3 + fl;
                    if (!filter_valid(f)) continue;
                    for (int r = 0; r < 4; r++)
                        if (br->ret[r].used && topic_match(f, br->ret[r].topic)) {
                            deliver(me, br->ret[r].topic, br->ret[r].pay, br->ret[r].n, q > 1 ? 1 : q, 1);
                            br->delivered++;
                        }
                }
            } else if (type == 10) { /* UNSUBSCRIBE */
                size_t o = 2;
                while (o < (size_t)rl) {
                    size_t fl = (size_t)(body[o] << 8 | body[o + 1]);
                    char f[32] = "";
                    memcpy(f, body + o + 2, fl < 31 ? fl : 31);
                    o += 2 + fl;
                    for (int s2 = 0; s2 < me->ns; s2++)
                        if (!strcmp(me->sub[s2].f, f)) { me->sub[s2] = me->sub[me->ns - 1]; me->ns--; break; }
                }
                send_pkt(me->c.fd, 0xB0, body, 2);
            } else if (type == 3) { /* PUBLISH */
                int qos = (hb >> 1) & 3, retain = hb & 1;
                size_t tl = (size_t)(body[0] << 8 | body[1]);
                char topic[32] = "";
                memcpy(topic, body + 2, tl < 31 ? tl : 31);
                size_t o = 2 + tl;
                unsigned pid = 0;
                if (qos) { pid = (unsigned)(body[o] << 8 | body[o + 1]); o += 2; }
                const unsigned char *pay = body + o;
                size_t pn = (size_t)rl - o;
                br->published++;
                if (qos) {
                    unsigned char pa[2] = {(unsigned char)(pid >> 8), (unsigned char)pid};
                    send_pkt(me->c.fd, 0x40, pa, 2);
                }
                if (retain) {
                    int slot = -1;
                    for (int r = 0; r < 4; r++) if (br->ret[r].used && !strcmp(br->ret[r].topic, topic)) slot = r;
                    for (int r = 0; r < 4 && slot < 0; r++) if (!br->ret[r].used) slot = r;
                    if (slot >= 0 && pn <= 64) {
                        br->ret[slot].used = 1;
                        snprintf(br->ret[slot].topic, 32, "%s", topic);
                        memcpy(br->ret[slot].pay, pay, pn);
                        br->ret[slot].n = pn;
                    }
                }
                for (int i = 0; i < n; i++) {
                    if (!cl[i].live || !cl[i].connected) continue;
                    int best = -1;
                    for (int s2 = 0; s2 < cl[i].ns; s2++)
                        if (topic_match(cl[i].sub[s2].f, topic) && cl[i].sub[s2].qos > best) best = cl[i].sub[s2].qos;
                    if (best < 0) {
                        int any = 0;
                        for (int s2 = 0; s2 < cl[i].ns; s2++) any |= topic_match(cl[i].sub[s2].f, topic);
                        if (!any) continue;
                        best = 0;
                    }
                    deliver(&cl[i], topic, pay, pn, qos < best ? qos : best, 0);
                    br->delivered++;
                }
            } else if (type == 4) {
                br->acks++;
            } else if (type == 12) { /* PINGREQ */
                send_pkt(me->c.fd, 0xD0, NULL, 0);
            } else if (type == 14) { /* DISCONNECT */
                me->live = 0; close(me->c.fd); closed++;
            }
        }
    }
    free(cl);
    return NULL;
}

/* ---- client side ---- */
static size_t put_str(unsigned char *b, const char *s) {
    size_t l = strlen(s);
    b[0] = (unsigned char)(l >> 8);
    b[1] = (unsigned char)l;
    memcpy(b + 2, s, l);
    return l + 2;
}

static void connect_pkt(Conn *c, const char *id, int flags, const char *user, const char *pass) {
    unsigned char b[128];
    size_t o = put_str(b, "MQTT");
    b[o++] = 4;
    b[o++] = (unsigned char)flags;
    b[o++] = 0; b[o++] = 60;
    o += put_str(b + o, id);
    if (flags & 0x80) o += put_str(b + o, user);
    if (flags & 0x40) o += put_str(b + o, pass);
    send_pkt(c->fd, 0x10, b, o);
}

static int recv_pkt(Conn *c, int *hdr, unsigned char *b, size_t cap) {
    int h = conn_getc(c);
    CHECK(h >= 0);
    long rl = read_rl(c);
    CHECK(rl >= 0 && (size_t)rl <= cap);
    CHECK(conn_readn(c, b, (size_t)rl) == 0);
    *hdr = h;
    return (int)rl;
}

static void show(const char *who, int hdr, const unsigned char *b, int n) {
    int type = hdr >> 4;
    printf("%-6s <- ", who);
    switch (type) {
    case 2: printf("CONNACK rc=%d\n", b[1]); break;
    case 4: printf("PUBACK id=%d\n", b[0] << 8 | b[1]); break;
    case 9: printf("SUBACK id=%d granted=", b[0] << 8 | b[1]); for (int i = 2; i < n; i++) printf("%s%02x", i > 2 ? "," : "", b[i]); printf("\n"); break;
    case 11: printf("UNSUBACK id=%d\n", b[0] << 8 | b[1]); break;
    case 13: printf("PINGRESP\n"); break;
    case 3: {
        int qos = (hdr >> 1) & 3;
        size_t tl = (size_t)(b[0] << 8 | b[1]);
        size_t o = 2 + tl + (qos ? 2 : 0);
        printf("PUBLISH qos=%d retain=%d topic=%.*s pid=%d payload=%d bytes", qos, hdr & 1, (int)tl, b + 2,
               qos ? (b[2 + tl] << 8 | b[3 + tl]) : 0, n - (int)o);
        if (n - (int)o <= 12) printf(" \"%.*s\"", n - (int)o, b + o);
        printf("\n");
        break;
    }
    default: printf("type %d\n", type);
    }
}

static void expect(Conn *c, const char *who) {
    unsigned char b[700];
    int hdr;
    int n = recv_pkt(c, &hdr, b, sizeof b);
    show(who, hdr, b, n);
}

static void subscribe(Conn *c, unsigned pid, const char **filters, const int *qos, int nf) {
    unsigned char b[200];
    size_t o = 0;
    b[o++] = (unsigned char)(pid >> 8);
    b[o++] = (unsigned char)pid;
    for (int i = 0; i < nf; i++) {
        o += put_str(b + o, filters[i]);
        b[o++] = (unsigned char)qos[i];
    }
    send_pkt(c->fd, 0x82, b, o);
}

static void publish(Conn *c, const char *topic, const void *pay, size_t n, int qos, unsigned pid, int retain) {
    unsigned char b[700];
    size_t o = put_str(b, topic);
    if (qos) { b[o++] = (unsigned char)(pid >> 8); b[o++] = (unsigned char)pid; }
    memcpy(b + o, pay, n);
    o += n;
    send_pkt(c->fd, 0x30 | (qos << 1) | retain, b, o);
}

int main(void) {
    net_init();
    /* remaining length vectors from the MQTT spec */
    const struct { uint32_t v; size_t n; unsigned char b[4]; } rlv[] = {
        {0, 1, {0x00}}, {127, 1, {0x7F}}, {128, 2, {0x80, 0x01}}, {16383, 2, {0xFF, 0x7F}},
        {16384, 3, {0x80, 0x80, 0x01}}, {2097151, 3, {0xFF, 0xFF, 0x7F}}, {2097152, 4, {0x80, 0x80, 0x80, 0x01}},
        {268435455u, 4, {0xFF, 0xFF, 0xFF, 0x7F}},
    };
    for (size_t i = 0; i < sizeof rlv / sizeof rlv[0]; i++) {
        unsigned char o[4];
        size_t n = enc_rl(o, rlv[i].v);
        CHECK(n == rlv[i].n && memcmp(o, rlv[i].b, n) == 0);
    }
    printf("remaining-length vectors: %zu ok\n", sizeof rlv / sizeof rlv[0]);

    Broker *br = calloc(1, sizeof *br);
    if (!br) die("oom");
    int port;
    br->lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, broker_main, br)) die("thread");
    Conn pub, sa, sb, bad1, bad2;
    conn_init(&pub, connect_lo(port));
    conn_init(&sa, connect_lo(port));
    conn_init(&sb, connect_lo(port));
    conn_init(&bad1, connect_lo(port));
    conn_init(&bad2, connect_lo(port));
    connect_pkt(&pub, "pub-1", 0x02, NULL, NULL);
    expect(&pub, "pub");
    connect_pkt(&sa, "sub-a", 0x02 | 0x80 | 0x40, "iron", "pw");
    expect(&sa, "sub-a");
    connect_pkt(&sb, "sub-b", 0x02, NULL, NULL);
    expect(&sb, "sub-b");
    connect_pkt(&bad1, "intruder", 0x02 | 0x80 | 0x40, "iron", "nope");
    expect(&bad1, "bad1");
    connect_pkt(&bad2, "", 0x00, NULL, NULL);
    expect(&bad2, "bad2");

    const char *fa[] = {"sensors/+/temp", "alerts/#", "bad/#/x"};
    const int qa[] = {1, 1, 1};
    subscribe(&sa, 10, fa, qa, 3);
    expect(&sa, "sub-a");
    const char *fb[] = {"sensors/#"};
    const int qb[] = {0};
    subscribe(&sb, 11, fb, qb, 1);
    expect(&sb, "sub-b");

    publish(&pub, "sensors/kitchen/temp", "21", 2, 1, 100, 0);
    expect(&pub, "pub");
    expect(&sa, "sub-a");
    {
        const unsigned char pa[2] = {0, 1};
        send_pkt(sa.fd, 0x40, pa, 2); /* subscriber acknowledges its QoS 1 delivery */
    }
    expect(&sb, "sub-b");
    publish(&pub, "alerts/fire", "!!", 2, 0, 0, 1); /* retained */
    expect(&sa, "sub-a");
    publish(&pub, "sensors/hall/humidity", "40%", 3, 1, 101, 0);
    expect(&pub, "pub");
    expect(&sb, "sub-b");

    const char *fb2[] = {"alerts/#"};
    const int qb2[] = {1};
    subscribe(&sb, 12, fb2, qb2, 1);
    expect(&sb, "sub-b");
    expect(&sb, "sub-b"); /* retained alert delivered after SUBACK */

    send_pkt(pub.fd, 0xC0, NULL, 0);
    expect(&pub, "pub");
    unsigned char un[64];
    size_t uo = 0;
    un[uo++] = 0; un[uo++] = 13;
    uo += put_str(un + uo, "alerts/#");
    send_pkt(sa.fd, 0xA2, un, uo);
    expect(&sa, "sub-a");
    publish(&pub, "alerts/flood", "wet", 3, 1, 102, 0);
    expect(&pub, "pub");
    expect(&sb, "sub-b"); /* sub-a no longer gets alerts */
    {
        const unsigned char pa[2] = {0, 2};
        send_pkt(sb.fd, 0x40, pa, 2);
    }

    /* payload big enough to need a two-byte remaining length */
    unsigned char big[300];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (unsigned char)('a' + i % 26);
    publish(&pub, "sensors/lab/temp", big, sizeof big, 1, 103, 0);
    expect(&pub, "pub");
    expect(&sa, "sub-a");
    expect(&sb, "sub-b");

    send_pkt(pub.fd, 0xE0, NULL, 0);
    send_pkt(sa.fd, 0xE0, NULL, 0);
    send_pkt(sb.fd, 0xE0, NULL, 0);
    close(pub.fd); close(sa.fd); close(sb.fd); close(bad1.fd); close(bad2.fd);
    pthread_join(th, NULL);
    close(br->lfd);
    printf("broker: published=%d delivered=%d puback_from_subscribers=%d refused=%d\n", br->published, br->delivered,
           br->acks, br->refused);
    free(br);
    return 0;
}
