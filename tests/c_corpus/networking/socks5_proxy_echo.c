/*
 * title: SOCKS5 proxy handshake to a local echo server
 * topic: networking
 * covers: RFC 1928 method negotiation, RFC 1929 user/pass auth, CONNECT with IPv4 and domain, reply codes, relay
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
    int lfd;
    int echo_port;
    int ok, fail;
    long relayed;
} Proxy;

typedef struct { int lfd; int served; } Echo;

static void *echo_main(void *arg) {
    Echo *e = arg;
    for (int i = 0; i < 2; i++) {
        int fd = accept_lo(e->lfd);
        if (fd < 0) return NULL;
        unsigned char b[256];
        ssize_t n;
        while ((n = recv(fd, b, sizeof b, 0)) > 0) {
            for (ssize_t k = 0; k < n; k++)
                if (b[k] >= 'a' && b[k] <= 'z') b[k] = (unsigned char)(b[k] - 32);
            send_all(fd, b, (size_t)n);
        }
        e->served++;
        close(fd);
    }
    return NULL;
}

static int need(int fd, void *b, size_t n) {
    unsigned char *p = b;
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static void reply(int fd, int rep) {
    unsigned char r[10] = {5, (unsigned char)rep, 0, 1, 0, 0, 0, 0, 0, 0};
    send_all(fd, r, sizeof r);
}

static void *proxy_main(void *arg) {
    Proxy *p = arg;
    for (int i = 0; i < 7; i++) {
        int fd = accept_lo(p->lfd);
        if (fd < 0) return NULL;
        unsigned char h[2], methods[16];
        if (need(fd, h, 2) < 0 || h[0] != 5 || h[1] > 16 || need(fd, methods, h[1]) < 0) { close(fd); continue; }
        int has0 = 0, has2 = 0;
        for (int k = 0; k < h[1]; k++) { has0 |= methods[k] == 0; has2 |= methods[k] == 2; }
        unsigned char sel[2] = {5, has2 ? 2 : has0 ? 0 : 0xFF};
        send_all(fd, sel, 2);
        if (sel[1] == 0xFF) { p->fail++; close(fd); continue; }
        if (sel[1] == 2) {
            unsigned char v, ul, pl, user[256], pass[256];
            if (need(fd, &v, 1) < 0 || need(fd, &ul, 1) < 0 || need(fd, user, ul) < 0 || need(fd, &pl, 1) < 0 || need(fd, pass, pl) < 0) { close(fd); continue; }
            user[ul] = 0;
            pass[pl] = 0;
            int good = !strcmp((char *)user, "iron") && !strcmp((char *)pass, "s3cret");
            unsigned char st[2] = {1, (unsigned char)(good ? 0 : 1)};
            send_all(fd, st, 2);
            if (!good) { p->fail++; close(fd); continue; }
        }
        unsigned char rq[4];
        if (need(fd, rq, 4) < 0) { close(fd); continue; }
        int port = -1;
        char host[64] = "";
        int rep = 0;
        if (rq[3] == 1) {
            unsigned char a[6];
            if (need(fd, a, 6) < 0) { close(fd); continue; }
            snprintf(host, sizeof host, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
            port = a[4] << 8 | a[5];
        } else if (rq[3] == 3) {
            unsigned char l, nm[256], pt[2];
            if (need(fd, &l, 1) < 0 || need(fd, nm, l) < 0 || need(fd, pt, 2) < 0) { close(fd); continue; }
            nm[l] = 0;
            snprintf(host, sizeof host, "%s", (char *)nm);
            port = pt[0] << 8 | pt[1];
            if (!strcmp(host, "echo.local")) snprintf(host, sizeof host, "127.0.0.1");
            else rep = 4; /* host unreachable */
        } else if (rq[3] == 4) {
            unsigned char skip[18];
            need(fd, skip, 18);
            rep = 8; /* address type not supported */
        } else
            rep = 8;
        if (rq[1] != 1 && rep == 0) rep = 7; /* command not supported */
        int up = -1;
        if (rep == 0) {
            if (strcmp(host, "127.0.0.1") != 0) rep = 2;
            else {
                up = connect_try(port);
                if (up < 0) rep = 5;
            }
        }
        reply(fd, rep);
        if (rep == 0) {
            p->ok++;
            p->relayed += relay_pair(fd, up);
            close(up);
        } else
            p->fail++;
        close(fd);
    }
    return NULL;
}

static const char *rep_name(int r) {
    static const char *n[] = {"succeeded", "general failure", "not allowed", "network unreachable", "host unreachable",
                              "connection refused", "ttl expired", "command not supported", "address type not supported"};
    return r >= 0 && r <= 8 ? n[r] : "?";
}

static int negotiate(int fd, const unsigned char *methods, int nm) {
    unsigned char m[20] = {5, (unsigned char)nm};
    memcpy(m + 2, methods, (size_t)nm);
    CHECK(send_all(fd, m, (size_t)nm + 2) == 0);
    unsigned char sel[2];
    CHECK(need(fd, sel, 2) == 0 && sel[0] == 5);
    return sel[1];
}

static int auth(int fd, const char *u, const char *p) {
    unsigned char b[64];
    size_t n = 0;
    b[n++] = 1;
    b[n++] = (unsigned char)strlen(u);
    memcpy(b + n, u, strlen(u)); n += strlen(u);
    b[n++] = (unsigned char)strlen(p);
    memcpy(b + n, p, strlen(p)); n += strlen(p);
    CHECK(send_all(fd, b, n) == 0);
    unsigned char st[2];
    CHECK(need(fd, st, 2) == 0);
    return st[1];
}

static int request(int fd, int cmd, int atyp, const char *host, int port) {
    unsigned char b[64];
    size_t n = 0;
    b[n++] = 5; b[n++] = (unsigned char)cmd; b[n++] = 0; b[n++] = (unsigned char)atyp;
    if (atyp == 1) { b[n++] = 127; b[n++] = 0; b[n++] = 0; b[n++] = 1; }
    else if (atyp == 3) { b[n++] = (unsigned char)strlen(host); memcpy(b + n, host, strlen(host)); n += strlen(host); }
    else { memset(b + n, 0, 16); n += 16; }
    b[n++] = (unsigned char)(port >> 8);
    b[n++] = (unsigned char)(port & 255);
    CHECK(send_all(fd, b, n) == 0);
    unsigned char r[10];
    CHECK(need(fd, r, 10) == 0 && r[0] == 5);
    return r[1];
}

static void talk(int fd, const char *text) {
    char back[64];
    size_t l = strlen(text);
    CHECK(send_all(fd, text, l) == 0);
    CHECK(need(fd, back, l) == 0);
    back[l] = 0;
    printf("  echoed through proxy: \"%s\" -> \"%s\"\n", text, back);
}

int main(void) {
    net_init();
    Echo e = {0};
    Proxy p = {0};
    int eport, pport;
    e.lfd = listen_lo(&eport);
    p.lfd = listen_lo(&pport);
    p.echo_port = eport;
    pthread_t et, pt;
    if (pthread_create(&et, NULL, echo_main, &e)) die("thread");
    if (pthread_create(&pt, NULL, proxy_main, &p)) die("thread");
    int fd, rep;

    /* 1: no auth, IPv4 */
    unsigned char m0[] = {0};
    fd = connect_lo(pport);
    printf("case 1: method %d selected\n", negotiate(fd, m0, 1));
    rep = request(fd, 1, 1, NULL, eport);
    printf("  CONNECT ipv4 -> %s\n", rep_name(rep));
    CHECK(rep == 0);
    talk(fd, "hello socks five");
    close(fd);

    /* 2: user/pass, domain */
    unsigned char m02[] = {0, 2};
    fd = connect_lo(pport);
    int sel = negotiate(fd, m02, 2);
    printf("case 2: method %d selected (auth wins over none)\n", sel);
    CHECK(sel == 2);
    printf("  auth status %d\n", auth(fd, "iron", "s3cret"));
    rep = request(fd, 1, 3, "echo.local", eport);
    printf("  CONNECT domain -> %s\n", rep_name(rep));
    CHECK(rep == 0);
    talk(fd, "via domain name");
    close(fd);

    /* 3: bad password */
    fd = connect_lo(pport);
    negotiate(fd, m02, 2);
    int st = auth(fd, "iron", "wrong");
    unsigned char dummy;
    printf("case 3: auth status %d, then connection %s\n", st, recv(fd, &dummy, 1, 0) <= 0 ? "closed" : "open");
    CHECK(st != 0);
    close(fd);

    /* 4: only GSSAPI offered */
    unsigned char m1[] = {1};
    fd = connect_lo(pport);
    printf("case 4: method 0x%02X selected\n", negotiate(fd, m1, 1));
    close(fd);

    /* 5: refused target */
    fd = connect_lo(pport);
    negotiate(fd, m0, 1);
    rep = request(fd, 1, 1, NULL, dead_port());
    printf("case 5: CONNECT to closed port -> %s\n", rep_name(rep));
    CHECK(rep == 5);
    close(fd);

    /* 6: BIND not supported; 7: IPv6 and unknown host */
    fd = connect_lo(pport);
    negotiate(fd, m0, 1);
    rep = request(fd, 2, 1, NULL, eport);
    printf("case 6: BIND -> %s\n", rep_name(rep));
    CHECK(rep == 7);
    close(fd);
    fd = connect_lo(pport);
    negotiate(fd, m0, 1);
    rep = request(fd, 1, 3, "nowhere.invalid", 80);
    printf("case 7: CONNECT unknown domain -> %s\n", rep_name(rep));
    CHECK(rep == 4);
    close(fd);
    pthread_join(pt, NULL);
    /* the second echo connection was opened only in case 2; unblock the echo thread for the rest */
    pthread_join(et, NULL);
    close(e.lfd);
    close(p.lfd);
    printf("proxy: ok=%d fail=%d relayed>0=%d echo sessions=%d\n", p.ok, p.fail, p.relayed > 0, e.served);
    CHECK(p.ok == 2 && p.fail == 5 && e.served == 2);
    return 0;
}
