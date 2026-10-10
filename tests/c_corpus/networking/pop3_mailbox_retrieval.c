/*
 * title: POP3-style mailbox server with deletions across sessions
 * topic: networking
 * covers: USER/PASS auth, STAT/LIST/UIDL/RETR/TOP/DELE/RSET/QUIT, byte-stuffing, update state on QUIT
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


#define NMSG 4

typedef struct {
    char text[256];
    int deleted;
    int pending_del;
} Msg;

typedef struct {
    int lfd;
    Msg box[NMSG];
    int sessions;
} Server;

static int nmsg_live(const Server *s) {
    int n = 0;
    for (int i = 0; i < NMSG; i++) n += !s->box[i].deleted && !s->box[i].pending_del;
    return n;
}

static void ok(int fd, const char *fmt, ...) {
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    sendf(fd, "+OK %s\r\n", b);
}

static void send_stuffed(int fd, const char *text, int max_body_lines) {
    /* text uses \n line ends; emits headers, blank, then up to max_body_lines body lines */
    int in_body = 0, bl = 0;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        if (in_body && max_body_lines >= 0 && bl >= max_body_lines) break;
        sendf(fd, "%s%.*s\r\n", *p == '.' ? "." : "", (int)(e - p), p);
        if (in_body) bl++;
        if (!in_body && e == p) in_body = 1;
        p = *e ? e + 1 : e;
    }
    send_str(fd, ".\r\n");
}

static void session(Server *s, int fd) {
    Conn c;
    conn_init(&c, fd);
    ok(fd, "POP3 ready");
    int authed = 0, have_user = 0;
    char line[128];
    while (conn_readline(&c, line, sizeof line) >= 0) {
        char cmd[8] = "", arg[100] = "";
        sscanf(line, "%7s %99[^\n]", cmd, arg);
        int n = atoi(arg);
        if (!strcasecmp(cmd, "USER")) {
            have_user = !strcmp(arg, "ada");
            ok(fd, "send PASS");
        } else if (!strcasecmp(cmd, "PASS")) {
            if (have_user && !strcmp(arg, "secret")) { authed = 1; ok(fd, "%d messages", nmsg_live(s)); }
            else sendf(fd, "-ERR invalid credentials\r\n");
        } else if (!authed && strcasecmp(cmd, "QUIT")) {
            sendf(fd, "-ERR not authenticated\r\n");
        } else if (!strcasecmp(cmd, "STAT")) {
            int cnt = 0, oct = 0;
            for (int i = 0; i < NMSG; i++)
                if (!s->box[i].deleted && !s->box[i].pending_del) { cnt++; oct += (int)strlen(s->box[i].text); }
            ok(fd, "%d %d", cnt, oct);
        } else if (!strcasecmp(cmd, "LIST") || !strcasecmp(cmd, "UIDL")) {
            int uidl = !strcasecmp(cmd, "UIDL");
            if (arg[0]) {
                if (n < 1 || n > NMSG || s->box[n - 1].deleted || s->box[n - 1].pending_del) { sendf(fd, "-ERR no such message\r\n"); continue; }
                if (uidl) ok(fd, "%d %08x", n, fnv1a(s->box[n - 1].text, strlen(s->box[n - 1].text)));
                else ok(fd, "%d %zu", n, strlen(s->box[n - 1].text));
                continue;
            }
            ok(fd, "listing");
            for (int i = 0; i < NMSG; i++) {
                if (s->box[i].deleted || s->box[i].pending_del) continue;
                if (uidl) sendf(fd, "%d %08x\r\n", i + 1, fnv1a(s->box[i].text, strlen(s->box[i].text)));
                else sendf(fd, "%d %zu\r\n", i + 1, strlen(s->box[i].text));
            }
            send_str(fd, ".\r\n");
        } else if (!strcasecmp(cmd, "RETR") || !strcasecmp(cmd, "TOP")) {
            int top = !strcasecmp(cmd, "TOP"), lines = -1;
            if (top) { int a; if (sscanf(arg, "%d %d", &a, &lines) != 2) { sendf(fd, "-ERR usage\r\n"); continue; } n = a; }
            if (n < 1 || n > NMSG || s->box[n - 1].deleted || s->box[n - 1].pending_del) { sendf(fd, "-ERR no such message\r\n"); continue; }
            ok(fd, "message follows");
            send_stuffed(fd, s->box[n - 1].text, lines);
        } else if (!strcasecmp(cmd, "DELE")) {
            if (n < 1 || n > NMSG || s->box[n - 1].deleted || s->box[n - 1].pending_del) { sendf(fd, "-ERR no such message\r\n"); continue; }
            s->box[n - 1].pending_del = 1;
            ok(fd, "message %d marked", n);
        } else if (!strcasecmp(cmd, "RSET")) {
            for (int i = 0; i < NMSG; i++) s->box[i].pending_del = 0;
            ok(fd, "maildrop has %d messages", nmsg_live(s));
        } else if (!strcasecmp(cmd, "QUIT")) {
            for (int i = 0; i < NMSG; i++)
                if (s->box[i].pending_del) { s->box[i].deleted = 1; s->box[i].pending_del = 0; }
            ok(fd, "bye, %d left", nmsg_live(s));
            return;
        } else
            sendf(fd, "-ERR unknown command\r\n");
    }
}

static void *server_main(void *arg) {
    Server *s = arg;
    for (int i = 0; i < 2; i++) {
        int fd = accept_lo(s->lfd);
        if (fd < 0) return NULL;
        s->sessions++;
        session(s, fd);
        close(fd);
    }
    return NULL;
}

/* client helpers */
static void expect_ok(Conn *c, const char *sent, char *line) {
    if (conn_readline(c, line, 300) < 0) die("eof");
    printf("%-14s %s\n", sent, line);
    CHECK(strncmp(line, "+OK", 3) == 0);
}

static int cmd_ok(Conn *c, const char *text) {
    char line[300];
    sendf(c->fd, "%s\r\n", text);
    if (conn_readline(c, line, sizeof line) < 0) die("eof");
    printf("%-14s %s\n", text, line);
    return line[0] == '+';
}

static int read_multi(Conn *c, char out[][80], int max) {
    int n = 0;
    char line[300];
    while (conn_readline(c, line, sizeof line) >= 0) {
        if (!strcmp(line, ".")) return n;
        const char *t = line[0] == '.' ? line + 1 : line; /* unstuff */
        if (n < max) snprintf(out[n], 80, "%s", t);
        n++;
    }
    die("eof in multiline");
    return -1;
}

int main(void) {
    net_init();
    Server *s = calloc(1, sizeof *s);
    if (!s) die("oom");
    snprintf(s->box[0].text, 256, "Subject: one\n\nfirst body\nsecond line\n");
    snprintf(s->box[1].text, 256, "Subject: dots\n\n.leading dot\n..two\nend\n");
    snprintf(s->box[2].text, 256, "Subject: three\n\nx\n");
    snprintf(s->box[3].text, 256, "Subject: four\nFrom: z@iron.test\n\nlast one\nmore\nmore2\n");
    int port;
    s->lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, s)) die("thread");

    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);
    char line[300], m[16][80];
    expect_ok(&c, "(greeting)", line);
    CHECK(!cmd_ok(&c, "STAT"));
    CHECK(cmd_ok(&c, "USER ada"));
    CHECK(!cmd_ok(&c, "PASS wrong"));
    CHECK(cmd_ok(&c, "USER ada"));
    CHECK(cmd_ok(&c, "PASS secret"));
    CHECK(cmd_ok(&c, "STAT"));
    CHECK(cmd_ok(&c, "LIST"));
    int n = read_multi(&c, m, 16);
    for (int i = 0; i < n; i++) printf("  list: %s\n", m[i]);
    CHECK(cmd_ok(&c, "UIDL 3"));
    CHECK(cmd_ok(&c, "RETR 2"));
    n = read_multi(&c, m, 16);
    for (int i = 0; i < n; i++) printf("  retr2[%d]: %s\n", i, m[i]);
    CHECK(n == 5 && strcmp(m[2], ".leading dot") == 0 && strcmp(m[3], "..two") == 0);
    CHECK(cmd_ok(&c, "TOP 4 1"));
    n = read_multi(&c, m, 16);
    printf("  top lines: %d\n", n);
    CHECK(n == 4);
    CHECK(cmd_ok(&c, "DELE 1"));
    CHECK(!cmd_ok(&c, "DELE 1"));
    CHECK(cmd_ok(&c, "DELE 3"));
    CHECK(!cmd_ok(&c, "RETR 3"));
    CHECK(cmd_ok(&c, "RSET"));
    CHECK(cmd_ok(&c, "DELE 3"));
    CHECK(!cmd_ok(&c, "BOGUS"));
    CHECK(cmd_ok(&c, "QUIT"));
    close(fd);

    /* second session sees the committed deletions */
    fd = connect_lo(port);
    conn_init(&c, fd);
    expect_ok(&c, "(greeting)", line);
    CHECK(cmd_ok(&c, "USER ada"));
    CHECK(cmd_ok(&c, "PASS secret"));
    CHECK(cmd_ok(&c, "UIDL"));
    n = read_multi(&c, m, 16);
    printf("  uidl entries: %d\n", n);
    CHECK(n == 3);
    CHECK(!cmd_ok(&c, "RETR 3"));
    CHECK(cmd_ok(&c, "RETR 4"));
    n = read_multi(&c, m, 16);
    printf("  retr4 lines: %d, last=%s\n", n, m[n - 1]);
    CHECK(cmd_ok(&c, "QUIT"));
    close(fd);
    pthread_join(th, NULL);
    close(s->lfd);
    printf("sessions=%d live=%d\n", s->sessions, nmsg_live(s));
    free(s);
    return 0;
}
