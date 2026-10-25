/*
 * title: HTTP 100-continue and request size limits
 * topic: networking
 * covers: Expect: 100-continue, 413 without reading body, 414 long URI, 431 too many headers, poll-based waits
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

/* ---- minimal HTTP/1.1 helpers ---- */
typedef struct {
    int n;
    char k[24][40];
    char v[24][256];
} Hdrs;

const char *hget(const Hdrs *h, const char *name) {
    for (int i = 0; i < h->n; i++)
        if (strcasecmp(h->k[i], name) == 0) return h->v[i];
    return NULL;
}

/* reads header lines until blank line; returns 0 ok, -1 error */
int read_headers(Conn *c, Hdrs *h) {
    char line[1024];
    h->n = 0;
    for (;;) {
        int n = conn_readline(c, line, sizeof line);
        if (n < 0) return -1;
        if (n == 0) return 0;
        char *colon = strchr(line, ':');
        if (!colon || h->n >= 24) return -1;
        *colon++ = 0;
        while (*colon == ' ') colon++;
        snprintf(h->k[h->n], sizeof h->k[0], "%s", line);
        snprintf(h->v[h->n], sizeof h->v[0], "%s", colon);
        h->n++;
    }
}

typedef struct {
    char method[16], target[256], version[16];
    Hdrs h;
} ReqHead;

/* returns 0 ok, -1 eof/error, -2 malformed request line */
int read_req_head(Conn *c, ReqHead *r) {
    char line[1024];
    int n = conn_readline(c, line, sizeof line);
    if (n < 0) return -1;
    char *sp1 = strchr(line, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2) {
        r->h.n = 0;
        return -2;
    }
    *sp1 = 0;
    *sp2 = 0;
    snprintf(r->method, sizeof r->method, "%s", line);
    snprintf(r->target, sizeof r->target, "%s", sp1 + 1);
    snprintf(r->version, sizeof r->version, "%s", sp2 + 1);
    return read_headers(c, &r->h);
}

/* reads "HTTP/1.1 200 OK" plus headers. returns status code or -1 */
int read_status(Conn *c, Hdrs *h, char *reason, size_t rcap) {
    char line[1024];
    if (conn_readline(c, line, sizeof line) < 0) return -1;
    if (strncmp(line, "HTTP/1.", 7) != 0) return -1;
    int code = atoi(line + 9);
    if (reason) snprintf(reason, rcap, "%s", line + 13);
    if (read_headers(c, h) < 0) return -1;
    return code;
}

/* decodes a chunked body into out; returns length or -1 */
long read_chunked(Conn *c, unsigned char *out, size_t cap, Hdrs *trailers) {
    size_t total = 0;
    char line[256];
    for (;;) {
        if (conn_readline(c, line, sizeof line) < 0) return -1;
        char *semi = strchr(line, ';');
        if (semi) *semi = 0;
        char *end;
        unsigned long sz = strtoul(line, &end, 16);
        if (end == line) return -1;
        if (sz == 0) break;
        if (total + sz > cap) return -1;
        if (conn_readn(c, out + total, sz) < 0) return -1;
        total += sz;
        if (conn_readline(c, line, sizeof line) != 0) return -1;
    }
    Hdrs tmp;
    if (read_headers(c, trailers ? trailers : &tmp) < 0) return -1;
    return (long)total;
}

/* reads body per Content-Length or chunked; returns length or -1 */
long read_body(Conn *c, const Hdrs *h, unsigned char *out, size_t cap) {
    const char *te = hget(h, "Transfer-Encoding");
    if (te && strcasecmp(te, "chunked") == 0) return read_chunked(c, out, cap, NULL);
    const char *cl = hget(h, "Content-Length");
    if (!cl) return 0;
    long n = atol(cl);
    if (n < 0 || (size_t)n > cap) return -1;
    if (conn_readn(c, out, (size_t)n) < 0) return -1;
    return n;
}

const char *reason_of(int code) {
    switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    default: return "Unknown";
    }
}

/* sends a response with Content-Length body and optional extra header lines
   (each terminated with \r\n) */
int send_resp(int fd, int code, const char *extra, const void *body, size_t n) {
    char head[1024];
    int hl = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n%s\r\n", code,
                      reason_of(code), n, extra ? extra : "");
    if (hl < 0 || hl >= (int)sizeof head) die("head overflow");
    if (send_all(fd, head, (size_t)hl) < 0) return -1;
    if (n && send_all(fd, body, n) < 0) return -1;
    return 0;
}

typedef struct {
    int code;
    char reason[64];
    Hdrs h;
    unsigned char body[16384];
    long blen;
} Resp;

/* sends a request (Content-Length set when bl>0 or method is POST/PUT) and reads the reply */
int client_req(Conn *c, const char *method, const char *target, const char *extra, const void *body,
               size_t bl, Resp *r) {
    char head[1024];
    int post = strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0;
    int n = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: iron.test\r\n%s", method, target,
                     extra ? extra : "");
    if (post || bl) n += snprintf(head + n, sizeof head - (size_t)n, "Content-Length: %zu\r\n", bl);
    n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
    if (send_all(c->fd, head, (size_t)n) < 0) return -1;
    if (bl && send_all(c->fd, body, bl) < 0) return -1;
    r->code = read_status(c, &r->h, r->reason, sizeof r->reason);
    if (r->code < 0) return -1;
    r->blen = 0;
    r->body[0] = 0;
    if (strcmp(method, "HEAD") == 0 || r->code == 304 || r->code == 204) return 0;
    r->blen = read_body(c, &r->h, r->body, sizeof r->body - 1);
    if (r->blen < 0) return -1;
    r->body[r->blen] = 0;
    return 0;
}


#define MAX_BODY 100
#define MAX_URI 60
#define MAX_HDRS 5

typedef struct {
    int lfd;
    int continues, rejected, accepted;
} Server;

static void *server_main(void *arg) {
    Server *s = arg;
    int fd = accept_lo(s->lfd);
    if (fd < 0) return NULL;
    Conn c;
    conn_init(&c, fd);
    for (;;) {
        ReqHead r;
        int rc = read_req_head(&c, &r);
        if (rc == -1) break;
        if (strlen(r.target) > MAX_URI) {
            s->rejected++;
            send_resp(fd, 414, NULL, NULL, 0);
            continue;
        }
        if (r.h.n > MAX_HDRS) {
            s->rejected++;
            send_resp(fd, 431, NULL, NULL, 0);
            continue;
        }
        const char *cl = hget(&r.h, "Content-Length");
        long n = cl ? atol(cl) : 0;
        if (n > MAX_BODY) {
            /* refuse before the client uploads anything, then close: unread body would corrupt framing */
            s->rejected++;
            send_resp(fd, 413, "Connection: close\r\n", NULL, 0);
            break;
        }
        const char *ex = hget(&r.h, "Expect");
        if (ex && strcasecmp(ex, "100-continue") == 0) {
            s->continues++;
            send_str(fd, "HTTP/1.1 100 Continue\r\n\r\n");
        }
        unsigned char body[MAX_BODY + 1];
        if (n > 0 && conn_readn(&c, body, (size_t)n) < 0) break;
        s->accepted++;
        char out[64];
        int ol = snprintf(out, sizeof out, "stored %ld bytes", n);
        send_resp(fd, 201, NULL, out, (size_t)ol);
    }
    close(fd);
    return NULL;
}

/* true when data is readable within ms */
static int readable(int fd, int ms) {
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, ms) > 0;
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
    Hdrs h;
    char reason[64];
    unsigned char payload[200];
    memset(payload, 'p', sizeof payload);

    /* 1. small body with Expect: wait for 100 before sending it */
    send_str(fd, "PUT /a HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 50\r\n\r\n");
    int code = read_status(&c, &h, reason, sizeof reason);
    printf("interim status: %d %s\n", code, reason);
    CHECK(code == 100);
    CHECK(send_all(fd, payload, 50) == 0);
    code = read_status(&c, &h, reason, sizeof reason);
    unsigned char b[64];
    long n = read_body(&c, &h, b, sizeof b - 1);
    b[n] = 0;
    printf("final status: %d %s (%s)\n", code, reason, (char *)b);

    /* 2. no Expect: server should not send an interim response */
    send_str(fd, "PUT /b HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n");
    printf("interim without Expect: %s\n", readable(fd, 100) ? "sent" : "none");
    send_all(fd, payload, 10);
    code = read_status(&c, &h, reason, sizeof reason);
    n = read_body(&c, &h, b, sizeof b - 1);
    printf("final status: %d (%ld bytes reply)\n", code, n);

    /* 3. long URI */
    char uri[128];
    memset(uri, 'u', sizeof uri);
    uri[100] = 0;
    sendf(fd, "GET /%s HTTP/1.1\r\nHost: x\r\n\r\n", uri);
    code = read_status(&c, &h, reason, sizeof reason);
    printf("long uri: %d %s\n", code, reason);
    CHECK(code == 414);

    /* 4. too many headers */
    send_str(fd, "GET /h HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\nD: 4\r\nE: 5\r\nF: 6\r\n\r\n");
    code = read_status(&c, &h, reason, sizeof reason);
    printf("many headers: %d %s\n", code, reason);
    CHECK(code == 431);

    /* 5. oversized body announced with Expect: refused before upload */
    send_str(fd, "PUT /c HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 200\r\n\r\n");
    code = read_status(&c, &h, reason, sizeof reason);
    printf("oversize: %d %s, close=%s\n", code, reason, hget(&h, "Connection"));
    CHECK(code == 413);
    CHECK(conn_getc(&c) < 0);
    close(fd);
    pthread_join(th, NULL);
    close(s.lfd);
    printf("server: continues=%d rejected=%d accepted=%d\n", s.continues, s.rejected, s.accepted);
    CHECK(s.continues == 1 && s.rejected == 3 && s.accepted == 2);
    return 0;
}
