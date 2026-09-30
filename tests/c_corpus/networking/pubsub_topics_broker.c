/*
 * title: Pub/sub broker with wildcard topics and retained messages
 * topic: networking
 * covers: topic tree matching (+ and #), poll-based broker, delivery counts, retained messages, unsubscribe
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


/* '+' matches one level, '#' (last) matches the rest */
static int topic_match(const char *pat, const char *topic) {
    while (*pat) {
        if (*pat == '#') return pat[1] == 0;
        const char *pe = strchr(pat, '/'), *te = strchr(topic, '/');
        size_t pl = pe ? (size_t)(pe - pat) : strlen(pat);
        size_t tl = te ? (size_t)(te - topic) : strlen(topic);
        if (!(pl == 1 && *pat == '+') && !(pl == tl && !strncmp(pat, topic, pl))) return 0;
        if (!pe || !te) {
            if (pe && pe[1] == '#' && !pe[2] && !te) return 1; /* "a/#" matches "a" */
            return !pe && !te;
        }
        pat = pe + 1;
        topic = te + 1;
    }
    return *topic == 0;
}

#define MAXC 4
#define MAXP 4
#define MAXR 8

typedef struct {
    Conn c;
    int live;
    char pats[MAXP][32];
    int np;
} Sub;

typedef struct {
    int lfd;
    char rtopic[MAXR][32], rpay[MAXR][48];
    int nr;
    int published, delivered;
} Broker;

static int wants(const Sub *s, const char *topic) {
    for (int i = 0; i < s->np; i++)
        if (topic_match(s->pats[i], topic)) return 1;
    return 0;
}

static void *broker_main(void *arg) {
    Broker *b = arg;
    Sub *subs = calloc(MAXC, sizeof *subs);
    int n = 0, closed = 0;
    while (closed < 4) {
        struct pollfd pf[MAXC + 1];
        int map[MAXC + 1], k = 0;
        pf[k].fd = b->lfd; pf[k].events = POLLIN; pf[k].revents = 0; map[k++] = -1;
        for (int i = 0; i < n; i++)
            if (subs[i].live) { pf[k].fd = subs[i].c.fd; pf[k].events = POLLIN; pf[k].revents = 0; map[k++] = i; }
        if (poll(pf, (nfds_t)k, 5000) <= 0) break;
        for (int j = 0; j < k; j++) {
            if (!(pf[j].revents & (POLLIN | POLLHUP))) continue;
            if (map[j] < 0) {
                int fd = accept_lo(b->lfd);
                if (fd < 0 || n == MAXC) { if (fd >= 0) close(fd); continue; }
                conn_init(&subs[n].c, fd);
                subs[n++].live = 1;
                continue;
            }
            Sub *me = &subs[map[j]];
            char line[160];
            if (conn_readline(&me->c, line, sizeof line) < 0) {
                me->live = 0;
                close(me->c.fd);
                closed++;
                continue;
            }
            char cmd[8] = "", topic[32] = "";
            int off = 0;
            sscanf(line, "%7s %31s%n", cmd, topic, &off);
            const char *payload = line + off + (line[off] == ' ');
            if (!strcmp(cmd, "SUB")) {
                if (me->np == MAXP) { send_str(me->c.fd, "ERR too many subscriptions\n"); continue; }
                snprintf(me->pats[me->np++], 32, "%s", topic);
                sendf(me->c.fd, "OK SUB %d\n", me->np);
                /* retained messages, in topic order */
                int order[MAXR];
                for (int r = 0; r < b->nr; r++) order[r] = r;
                for (int a = 0; a < b->nr; a++)
                    for (int c2 = a + 1; c2 < b->nr; c2++)
                        if (strcmp(b->rtopic[order[a]], b->rtopic[order[c2]]) > 0) { int t = order[a]; order[a] = order[c2]; order[c2] = t; }
                for (int a = 0; a < b->nr; a++)
                    if (topic_match(topic, b->rtopic[order[a]]))
                        sendf(me->c.fd, "RETAINED %s %s\n", b->rtopic[order[a]], b->rpay[order[a]]);
            } else if (!strcmp(cmd, "UNSUB")) {
                int found = 0;
                for (int i = 0; i < me->np; i++)
                    if (!strcmp(me->pats[i], topic)) {
                        memmove(me->pats[i], me->pats[i + 1], (size_t)(me->np - i - 1) * 32);
                        me->np--;
                        found = 1;
                        break;
                    }
                sendf(me->c.fd, found ? "OK UNSUB\n" : "ERR not subscribed\n");
            } else if (!strcmp(cmd, "PUB") || !strcmp(cmd, "PUBR")) {
                if (strchr(topic, '+') || strchr(topic, '#') || !topic[0]) { send_str(me->c.fd, "ERR bad topic\n"); continue; }
                b->published++;
                if (cmd[3] == 'R') {
                    int idx = -1;
                    for (int r = 0; r < b->nr; r++)
                        if (!strcmp(b->rtopic[r], topic)) idx = r;
                    if (idx < 0 && b->nr < MAXR) idx = b->nr++;
                    if (idx >= 0) { snprintf(b->rtopic[idx], 32, "%s", topic); snprintf(b->rpay[idx], 48, "%s", payload); }
                }
                int cnt = 0;
                for (int i = 0; i < n; i++)
                    if (subs[i].live && wants(&subs[i], topic)) {
                        sendf(subs[i].c.fd, "MSG %s %s\n", topic, payload);
                        cnt++;
                    }
                b->delivered += cnt;
                sendf(me->c.fd, "OK %d\n", cnt);
            } else
                send_str(me->c.fd, "ERR unknown command\n");
        }
    }
    free(subs);
    return NULL;
}

static void line_of(Conn *c, const char *who, char *out, size_t cap) {
    CHECK(conn_readline(c, out, cap) >= 0);
    printf("%-5s <- %s\n", who, out);
}

int main(void) {
    net_init();
    /* pattern matcher checks */
    struct { const char *p, *t; int want; } mt[] = {
        {"a/b/c", "a/b/c", 1}, {"a/b/c", "a/b", 0}, {"a/+/c", "a/x/c", 1}, {"a/+/c", "a/x/y/c", 0},
        {"a/#", "a/b/c/d", 1}, {"#", "anything/at/all", 1}, {"+", "one", 1}, {"+", "one/two", 0},
        {"a/+", "a", 0}, {"a/#", "a", 1}, {"a/b", "a/bc", 0}, {"+/+", "x/y", 1}, {"a/+/#", "a/b", 1},
    };
    int okc = 0;
    for (size_t i = 0; i < sizeof mt / sizeof mt[0]; i++) {
        int r = topic_match(mt[i].p, mt[i].t);
        CHECK(r == mt[i].want);
        okc++;
        printf("match %-8s %-16s %s\n", mt[i].p, mt[i].t, r ? "yes" : "no");
    }
    printf("matcher table: %d checks\n", okc);

    Broker b;
    memset(&b, 0, sizeof b);
    int port;
    b.lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, broker_main, &b)) die("thread");
    Conn pub, dash, logger, alarm_;
    conn_init(&pub, connect_lo(port));
    conn_init(&dash, connect_lo(port));
    conn_init(&logger, connect_lo(port));
    conn_init(&alarm_, connect_lo(port));
    char l[160];

    CHECK(sendf(pub.fd, "PUBR home/kitchen/temp 21.5\n") == 0);
    line_of(&pub, "pub", l, sizeof l);
    CHECK(sendf(pub.fd, "PUBR home/hall/temp 19.0\n") == 0);
    line_of(&pub, "pub", l, sizeof l);
    CHECK(sendf(pub.fd, "PUBR home/kitchen/door open\n") == 0);
    line_of(&pub, "pub", l, sizeof l);

    CHECK(sendf(dash.fd, "SUB home/+/temp\n") == 0);
    line_of(&dash, "dash", l, sizeof l);
    line_of(&dash, "dash", l, sizeof l);
    line_of(&dash, "dash", l, sizeof l);
    CHECK(sendf(logger.fd, "SUB #\n") == 0);
    line_of(&logger, "log", l, sizeof l);
    for (int i = 0; i < 3; i++) line_of(&logger, "log", l, sizeof l);
    CHECK(sendf(alarm_.fd, "SUB home/kitchen/#\n") == 0);
    line_of(&alarm_, "alarm", l, sizeof l);
    for (int i = 0; i < 2; i++) line_of(&alarm_, "alarm", l, sizeof l);
    CHECK(sendf(dash.fd, "SUB home/hall/#\n") == 0);
    line_of(&dash, "dash", l, sizeof l);
    line_of(&dash, "dash", l, sizeof l); /* retained hall/temp arrives again via 2nd pattern */

    /* publish; dash matches twice for hall/temp but gets one copy */
    struct { const char *topic, *pay; int dash, log, alarm; } pubs[] = {
        {"home/kitchen/temp", "22.0", 1, 1, 1},
        {"home/hall/temp", "18.5", 1, 1, 0},
        {"home/hall/light", "on", 1, 1, 0},
        {"garage/door", "closed", 0, 1, 0},
        {"home/kitchen/door", "closed", 0, 1, 1},
    };
    int total = 0;
    for (size_t i = 0; i < sizeof pubs / sizeof pubs[0]; i++) {
        CHECK(sendf(pub.fd, "PUB %s %s\n", pubs[i].topic, pubs[i].pay) == 0);
        line_of(&pub, "pub", l, sizeof l);
        int want = pubs[i].dash + pubs[i].log + pubs[i].alarm;
        CHECK(atoi(l + 3) == want);
        total += want;
        if (pubs[i].dash) line_of(&dash, "dash", l, sizeof l);
        if (pubs[i].log) line_of(&logger, "log", l, sizeof l);
        if (pubs[i].alarm) line_of(&alarm_, "alarm", l, sizeof l);
    }
    CHECK(sendf(dash.fd, "UNSUB home/hall/#\n") == 0);
    line_of(&dash, "dash", l, sizeof l);
    CHECK(sendf(dash.fd, "UNSUB home/hall/#\n") == 0);
    line_of(&dash, "dash", l, sizeof l);
    CHECK(sendf(pub.fd, "PUB home/hall/light off\n") == 0);
    line_of(&pub, "pub", l, sizeof l);
    line_of(&logger, "log", l, sizeof l);
    CHECK(sendf(pub.fd, "PUB home/+/x bad\n") == 0);
    line_of(&pub, "pub", l, sizeof l);
    CHECK(sendf(pub.fd, "HELLO\n") == 0);
    line_of(&pub, "pub", l, sizeof l);
    close(pub.fd); close(dash.fd); close(logger.fd); close(alarm_.fd);
    pthread_join(th, NULL);
    close(b.lfd);
    printf("broker: published=%d delivered=%d retained=%d\n", b.published, b.delivered, b.nr);
    CHECK(b.delivered == total + 1 + 0 + 3 * 0);
    return 0;
}
