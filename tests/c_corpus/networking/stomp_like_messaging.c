/*
 * title: STOMP-style messaging broker with queues and topics
 * topic: networking
 * covers: NUL-terminated frames, headers, content-length bodies with NUL bytes, queue round robin, topic fan-out, receipts, ERROR
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


typedef struct {
    char cmd[16];
    char hk[12][24], hv[12][96];
    int nh;
    unsigned char body[256];
    size_t blen;
} SFrame;

static const char *sh(const SFrame *f, const char *k) {
    for (int i = 0; i < f->nh; i++)
        if (!strcmp(f->hk[i], k)) return f->hv[i];
    return NULL;
}

/* reads one frame; returns 0 ok, -1 eof/malformed */
static int frame_read(Conn *c, SFrame *f) {
    char line[200];
    memset(f, 0, sizeof *f);
    do {
        if (conn_readline(c, f->cmd, sizeof f->cmd) < 0) return -1; /* skip heart-beat blank lines */
    } while (!f->cmd[0]);
    for (;;) {
        if (conn_readline(c, line, sizeof line) < 0) return -1;
        if (!line[0]) break;
        char *colon = strchr(line, ':');
        if (!colon || f->nh >= 12) return -1;
        *colon++ = 0;
        snprintf(f->hk[f->nh], sizeof f->hk[0], "%s", line);
        snprintf(f->hv[f->nh], sizeof f->hv[0], "%s", colon);
        f->nh++;
    }
    const char *cl = sh(f, "content-length");
    if (cl) {
        size_t n = (size_t)atoi(cl);
        if (n > sizeof f->body || conn_readn(c, f->body, n) < 0) return -1;
        f->blen = n;
        if (conn_getc(c) != 0) return -1;
    } else {
        int ch;
        while ((ch = conn_getc(c)) > 0) {
            if (f->blen >= sizeof f->body) return -1;
            f->body[f->blen++] = (unsigned char)ch;
        }
        if (ch < 0) return -1;
    }
    return 0;
}

static void frame_send(int fd, const char *cmd, const char *hdrs, const unsigned char *body, size_t n) {
    char head[512];
    int hl = snprintf(head, sizeof head, "%s\n%scontent-length:%zu\n\n", cmd, hdrs, n);
    send_all(fd, head, (size_t)hl);
    if (n) send_all(fd, body, n);
    send_all(fd, "\0", 1);
}

#define MAXC 3
#define MAXS 4
typedef struct {
    Conn c;
    int live, connected;
    struct { char id[8]; char dest[24]; } sub[MAXS];
    int ns;
} Client;

typedef struct { char dest[24]; unsigned char body[256]; size_t n; } Stored;

typedef struct {
    int lfd;
    int msgid;
    int rr; /* round-robin counter per server (single queue at a time in this test) */
    Stored stored[8];
    int nst;
    int errors, delivered;
} Server;

static void deliver(Server *s, Client *cl, int ci, const char *dest, const unsigned char *body, size_t n) {
    for (int i = 0; i < cl[ci].ns; i++)
        if (!strcmp(cl[ci].sub[i].dest, dest)) {
            char h[200];
            snprintf(h, sizeof h, "subscription:%s\nmessage-id:m-%d\ndestination:%s\n", cl[ci].sub[i].id, ++s->msgid, dest);
            frame_send(cl[ci].c.fd, "MESSAGE", h, body, n);
            s->delivered++;
            return;
        }
}

static void route(Server *s, Client *cl, const char *dest, const unsigned char *body, size_t n) {
    int subs[MAXC], ns = 0;
    for (int i = 0; i < MAXC; i++)
        for (int k = 0; k < cl[i].live * cl[i].ns; k++)
            if (!strcmp(cl[i].sub[k].dest, dest)) { subs[ns++] = i; break; }
    if (!strncmp(dest, "/topic/", 7)) {
        for (int i = 0; i < ns; i++) deliver(s, cl, subs[i], dest, body, n);
    } else if (ns > 0) {
        deliver(s, cl, subs[s->rr++ % ns], dest, body, n);
    } else if (s->nst < 8) { /* queue without consumers: keep the message */
        snprintf(s->stored[s->nst].dest, 24, "%s", dest);
        memcpy(s->stored[s->nst].body, body, n);
        s->stored[s->nst++].n = n;
    }
}

static void *server_main(void *arg) {
    Server *s = arg;
    Client *cl = calloc(MAXC, sizeof *cl);
    int n = 0, closed = 0;
    while (closed < MAXC) {
        struct pollfd pf[MAXC + 1];
        int map[MAXC + 1], k = 0;
        pf[k].fd = s->lfd; pf[k].events = POLLIN; pf[k].revents = 0; map[k++] = -1;
        for (int i = 0; i < n; i++)
            if (cl[i].live) { pf[k].fd = cl[i].c.fd; pf[k].events = POLLIN; pf[k].revents = 0; map[k++] = i; }
        if (poll(pf, (nfds_t)k, 5000) <= 0) break;
        for (int j = 0; j < k; j++) {
            if (!(pf[j].revents & (POLLIN | POLLHUP))) continue;
            if (map[j] < 0) {
                int fd = accept_lo(s->lfd);
                if (fd < 0 || n == MAXC) { if (fd >= 0) close(fd); continue; }
                conn_init(&cl[n].c, fd);
                cl[n++].live = 1;
                continue;
            }
            int ci = map[j];
            Client *me = &cl[ci];
            do {
                SFrame f;
                if (frame_read(&me->c, &f) < 0) { me->live = 0; close(me->c.fd); closed++; break; }
                const char *rc = sh(&f, "receipt");
                char rh[64] = "";
                if (rc) snprintf(rh, sizeof rh, "receipt-id:%s\n", rc);
                if (!strcmp(f.cmd, "CONNECT")) {
                    const char *pw = sh(&f, "passcode");
                    if (!pw || strcmp(pw, "letmein") != 0) {
                        frame_send(me->c.fd, "ERROR", "message:bad credentials\n", NULL, 0);
                        s->errors++;
                        me->live = 0; close(me->c.fd); closed++;
                        break;
                    }
                    me->connected = 1;
                    frame_send(me->c.fd, "CONNECTED", "version:1.2\n", NULL, 0);
                } else if (!me->connected) {
                    frame_send(me->c.fd, "ERROR", "message:not connected\n", NULL, 0);
                    s->errors++;
                    me->live = 0; close(me->c.fd); closed++;
                    break;
                } else if (!strcmp(f.cmd, "SUBSCRIBE")) {
                    const char *id = sh(&f, "id"), *dest = sh(&f, "destination");
                    if (me->ns < MAXS && id && dest) {
                        snprintf(me->sub[me->ns].id, 8, "%s", id);
                        snprintf(me->sub[me->ns++].dest, 24, "%s", dest);
                        /* hand over stored messages, oldest first */
                        for (int q = 0; q < s->nst;) {
                            if (!strcmp(s->stored[q].dest, dest)) {
                                deliver(s, cl, ci, dest, s->stored[q].body, s->stored[q].n);
                                memmove(&s->stored[q], &s->stored[q + 1], (size_t)(s->nst - q - 1) * sizeof s->stored[0]);
                                s->nst--;
                            } else
                                q++;
                        }
                    }
                    if (rc) frame_send(me->c.fd, "RECEIPT", rh, NULL, 0);
                } else if (!strcmp(f.cmd, "SEND")) {
                    const char *dest = sh(&f, "destination");
                    if (dest) route(s, cl, dest, f.body, f.blen);
                    if (rc) frame_send(me->c.fd, "RECEIPT", rh, NULL, 0);
                } else if (!strcmp(f.cmd, "ACK")) {
                    if (rc) frame_send(me->c.fd, "RECEIPT", rh, NULL, 0);
                } else if (!strcmp(f.cmd, "DISCONNECT")) {
                    if (rc) frame_send(me->c.fd, "RECEIPT", rh, NULL, 0);
                    me->live = 0; shutdown(me->c.fd, SHUT_WR); close(me->c.fd); closed++;
                    break;
                } else {
                    char eh[96];
                    snprintf(eh, sizeof eh, "message:unknown command %s\n", f.cmd);
                    frame_send(me->c.fd, "ERROR", eh, NULL, 0);
                    s->errors++;
                    me->live = 0; close(me->c.fd); closed++;
                    break;
                }
            } while (me->live && me->c.pos < me->c.len);
        }
    }
    free(cl);
    return NULL;
}

static void tx(Conn *c, const char *cmd, const char *hdrs, const void *body, size_t n) {
    char h[300];
    int hl = snprintf(h, sizeof h, "%s\n%s", cmd, hdrs);
    if (n) hl += snprintf(h + hl, sizeof h - (size_t)hl, "content-length:%zu\n", n);
    hl += snprintf(h + hl, sizeof h - (size_t)hl, "\n");
    send_all(c->fd, h, (size_t)hl);
    if (n) send_all(c->fd, body, n);
    send_all(c->fd, "\0", 1);
}

static void rx(Conn *c, const char *who, SFrame *f) {
    CHECK(frame_read(c, f) == 0);
    const char *mid = sh(f, "message-id"), *sub = sh(f, "subscription"), *dest = sh(f, "destination");
    printf("%-5s <- %s", who, f->cmd);
    if (sub) printf(" sub=%s", sub);
    if (dest) printf(" dest=%s", dest);
    if (mid) printf(" id=%s", mid);
    if (sh(f, "receipt-id")) printf(" receipt=%s", sh(f, "receipt-id"));
    if (sh(f, "message")) printf(" message=\"%s\"", sh(f, "message"));
    if (f->blen) {
        printf(" body=[");
        for (size_t i = 0; i < f->blen; i++) {
            if (f->body[i] == 0) printf("\\0"); else if (f->body[i] == '\n') printf("\\n"); else putchar(f->body[i]);
        }
        printf("]");
    }
    printf("\n");
}

int main(void) {
    net_init();
    Server s = {0};
    int port;
    s.lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, &s)) die("thread");
    Conn prod, c1, c2;
    conn_init(&prod, connect_lo(port));
    conn_init(&c1, connect_lo(port));
    conn_init(&c2, connect_lo(port));
    SFrame f;
    tx(&prod, "CONNECT", "accept-version:1.2\nlogin:p\npasscode:letmein\n", NULL, 0);
    rx(&prod, "prod", &f);
    tx(&c1, "CONNECT", "accept-version:1.2\nlogin:c1\npasscode:letmein\n", NULL, 0);
    rx(&c1, "c1", &f);
    tx(&c2, "CONNECT", "accept-version:1.2\nlogin:c2\npasscode:letmein\n", NULL, 0);
    rx(&c2, "c2", &f);

    tx(&c1, "SUBSCRIBE", "id:0\ndestination:/queue/jobs\nreceipt:r1\n", NULL, 0);
    rx(&c1, "c1", &f);
    tx(&c2, "SUBSCRIBE", "id:7\ndestination:/queue/jobs\nreceipt:r2\n", NULL, 0);
    rx(&c2, "c2", &f);
    tx(&c1, "SUBSCRIBE", "id:1\ndestination:/topic/news\nreceipt:r3\n", NULL, 0);
    rx(&c1, "c1", &f);
    tx(&c2, "SUBSCRIBE", "id:8\ndestination:/topic/news\nreceipt:r4\n", NULL, 0);
    rx(&c2, "c2", &f);

    /* four jobs alternate between the two competing consumers; job 3 has a NUL byte in its body */
    const unsigned char nulbody[] = {'j', 'o', 'b', 0, '3'};
    const char *jobs[] = {"job-1", "job-2", NULL, "job-4"};
    for (int i = 0; i < 4; i++) {
        if (jobs[i]) tx(&prod, "SEND", "destination:/queue/jobs\n", jobs[i], strlen(jobs[i]));
        else tx(&prod, "SEND", "destination:/queue/jobs\n", nulbody, sizeof nulbody);
    }
    tx(&prod, "SEND", "destination:/queue/jobs\nreceipt:sync1\n", "line1\nline2", 11);
    rx(&prod, "prod", &f);
    for (int i = 0; i < 3; i++) rx(&c1, "c1", &f);
    for (int i = 0; i < 2; i++) rx(&c2, "c2", &f);
    CHECK(f.blen == 11 || f.blen == 5 || f.blen == 5);
    tx(&prod, "SEND", "destination:/topic/news\nreceipt:sync2\n", "extra! extra!", 13);
    rx(&prod, "prod", &f);
    rx(&c1, "c1", &f);
    rx(&c2, "c2", &f);

    /* stored until someone subscribes */
    tx(&prod, "SEND", "destination:/queue/late\n", "first", 5);
    tx(&prod, "SEND", "destination:/queue/late\nreceipt:sync3\n", "second", 6);
    rx(&prod, "prod", &f);
    tx(&c2, "SUBSCRIBE", "id:9\ndestination:/queue/late\nreceipt:r5\n", NULL, 0);
    rx(&c2, "c2", &f);
    rx(&c2, "c2", &f);
    rx(&c2, "c2", &f);
    tx(&c2, "ACK", "id:m-1\nreceipt:ack1\n", NULL, 0);
    rx(&c2, "c2", &f);

    /* protocol error closes the connection */
    tx(&c1, "BOGUS", "", NULL, 0);
    rx(&c1, "c1", &f);
    CHECK(conn_getc(&c1) < 0);
    tx(&prod, "DISCONNECT", "receipt:bye\n", NULL, 0);
    rx(&prod, "prod", &f);
    close(prod.fd);
    close(c1.fd);
    close(c2.fd);
    pthread_join(th, NULL);
    close(s.lfd);
    printf("broker: delivered=%d errors=%d stored_left=%d msgids=%d\n", s.delivered, s.errors, s.nst, s.msgid);
    return 0;
}
