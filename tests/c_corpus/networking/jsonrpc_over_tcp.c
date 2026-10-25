/*
 * title: JSON-RPC 2.0 over newline-delimited TCP
 * topic: networking
 * covers: recursive descent JSON parser, arena nodes, serializer, batch requests, notifications, error codes
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


enum { T_NULL, T_BOOL, T_NUM, T_STR, T_ARR, T_OBJ };

typedef struct J {
    int t;
    long n;
    char s[48];
    char key[16];
    struct J *child, *next;
} J;

typedef struct {
    J pool[96];
    int used;
    const char *p;
    int err;
} Parser;

static J *newj(Parser *ps, int t) {
    if (ps->used >= 96) { ps->err = 1; return NULL; }
    J *j = &ps->pool[ps->used++];
    memset(j, 0, sizeof *j);
    j->t = t;
    return j;
}

static void skip_ws(Parser *ps) {
    while (*ps->p == ' ' || *ps->p == '\t') ps->p++;
}

static int parse_str(Parser *ps, char *out, size_t cap) {
    if (*ps->p != '"') return -1;
    ps->p++;
    size_t n = 0;
    while (*ps->p && *ps->p != '"') {
        char ch = *ps->p++;
        if (ch == '\\') {
            char e = *ps->p++;
            ch = e == 'n' ? '\n' : e == 't' ? '\t' : e;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = ch;
    }
    if (*ps->p != '"') return -1;
    ps->p++;
    out[n] = 0;
    return 0;
}

static J *parse_val(Parser *ps, int depth) {
    if (depth > 8) { ps->err = 1; return NULL; }
    skip_ws(ps);
    char ch = *ps->p;
    J *j;
    if (ch == '{' || ch == '[') {
        int obj = ch == '{';
        j = newj(ps, obj ? T_OBJ : T_ARR);
        if (!j) return NULL;
        ps->p++;
        skip_ws(ps);
        J **tail = &j->child;
        if (*ps->p == (obj ? '}' : ']')) { ps->p++; return j; }
        for (;;) {
            char key[16] = "";
            skip_ws(ps);
            if (obj) {
                if (parse_str(ps, key, sizeof key) < 0) { ps->err = 1; return NULL; }
                skip_ws(ps);
                if (*ps->p++ != ':') { ps->err = 1; return NULL; }
            }
            J *v = parse_val(ps, depth + 1);
            if (!v) return NULL;
            snprintf(v->key, sizeof v->key, "%s", key);
            *tail = v;
            tail = &v->next;
            skip_ws(ps);
            if (*ps->p == ',') { ps->p++; continue; }
            if (*ps->p == (obj ? '}' : ']')) { ps->p++; return j; }
            ps->err = 1;
            return NULL;
        }
    }
    if (ch == '"') {
        j = newj(ps, T_STR);
        if (!j || parse_str(ps, j->s, sizeof j->s) < 0) { ps->err = 1; return NULL; }
        return j;
    }
    if (!strncmp(ps->p, "null", 4)) { ps->p += 4; return newj(ps, T_NULL); }
    if (!strncmp(ps->p, "true", 4)) { ps->p += 4; j = newj(ps, T_BOOL); if (j) j->n = 1; return j; }
    if (!strncmp(ps->p, "false", 5)) { ps->p += 5; return newj(ps, T_BOOL); }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        char *end;
        j = newj(ps, T_NUM);
        if (!j) return NULL;
        j->n = strtol(ps->p, &end, 10);
        ps->p = end;
        return j;
    }
    ps->err = 1;
    return NULL;
}

static J *parse(Parser *ps, const char *text) {
    ps->used = 0;
    ps->err = 0;
    ps->p = text;
    J *j = parse_val(ps, 0);
    if (!j || ps->err) return NULL;
    skip_ws(ps);
    return *ps->p ? NULL : j;
}

static J *member(J *o, const char *k) {
    if (!o || o->t != T_OBJ) return NULL;
    for (J *c = o->child; c; c = c->next)
        if (!strcmp(c->key, k)) return c;
    return NULL;
}

static size_t ser(const J *j, char *out, size_t cap) {
    size_t o = 0;
    switch (j->t) {
    case T_NULL: o = (size_t)snprintf(out, cap, "null"); break;
    case T_BOOL: o = (size_t)snprintf(out, cap, j->n ? "true" : "false"); break;
    case T_NUM: o = (size_t)snprintf(out, cap, "%ld", j->n); break;
    case T_STR: o = (size_t)snprintf(out, cap, "\"%s\"", j->s); break;
    default: {
        int obj = j->t == T_OBJ;
        out[o++] = obj ? '{' : '[';
        for (const J *c = j->child; c; c = c->next) {
            if (c != j->child) out[o++] = ',';
            if (obj) o += (size_t)snprintf(out + o, cap - o, "\"%s\":", c->key);
            o += ser(c, out + o, cap - o);
        }
        out[o++] = obj ? '}' : ']';
        out[o] = 0;
    }
    }
    return o;
}

/* fetch positional or named parameter */
static J *param(J *params, int idx, const char *name) {
    if (!params) return NULL;
    if (params->t == T_OBJ) return member(params, name);
    if (params->t != T_ARR) return NULL;
    J *c = params->child;
    for (int i = 0; c && i < idx; i++) c = c->next;
    return c;
}

/* handles one request object; writes response (or nothing for notifications) */
static int handle(J *req, char *out, size_t cap) {
    J *id = member(req, "id");
    char ids[64] = "null";
    if (id) ser(id, ids, sizeof ids);
    J *m = member(req, "method"), *ver = member(req, "jsonrpc");
    if (!req || req->t != T_OBJ || !m || m->t != T_STR || !ver || strcmp(ver->s, "2.0") != 0)
        return snprintf(out, cap, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Invalid Request\"},\"id\":null}");
    J *pr = member(req, "params");
    char result[160] = "";
    int code = 0;
    const char *msg = "";
    if (!strcmp(m->s, "add") || !strcmp(m->s, "subtract")) {
        J *a = param(pr, 0, !strcmp(m->s, "add") ? "a" : "minuend");
        J *b = param(pr, 1, !strcmp(m->s, "add") ? "b" : "subtrahend");
        if (!a || !b || a->t != T_NUM || b->t != T_NUM) { code = -32602; msg = "Invalid params"; }
        else snprintf(result, sizeof result, "%ld", m->s[0] == 'a' ? a->n + b->n : a->n - b->n);
    } else if (!strcmp(m->s, "sum")) {
        J *arr = param(pr, 0, "values");
        if (!arr || arr->t != T_ARR) { code = -32602; msg = "Invalid params"; }
        else {
            long t = 0;
            for (J *c = arr->child; c; c = c->next) {
                if (c->t != T_NUM) { code = -32602; msg = "Invalid params"; }
                else t += c->n;
            }
            snprintf(result, sizeof result, "%ld", t);
        }
    } else if (!strcmp(m->s, "concat")) {
        J *a = param(pr, 0, "a"), *b = param(pr, 1, "b");
        if (!a || !b || a->t != T_STR || b->t != T_STR) { code = -32602; msg = "Invalid params"; }
        else snprintf(result, sizeof result, "\"%.20s%.20s\"", a->s, b->s);
    } else if (!strcmp(m->s, "echo")) {
        J *a = param(pr, 0, "value");
        if (!a) { code = -32602; msg = "Invalid params"; }
        else ser(a, result, sizeof result);
    } else if (!strcmp(m->s, "notify_log")) {
        return 0;
    } else {
        code = -32601;
        msg = "Method not found";
    }
    if (!id) return 0; /* notification: no response */
    if (code) return snprintf(out, cap, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":%s}", code, msg, ids);
    return snprintf(out, cap, "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":%s}", result, ids);
}

typedef struct { int lfd; int lines, notifs; } Server;

static void *server_main(void *arg) {
    Server *s = arg;
    int fd = accept_lo(s->lfd);
    if (fd < 0) return NULL;
    Conn c;
    conn_init(&c, fd);
    char line[512];
    Parser *ps = malloc(sizeof *ps);
    while (conn_readline(&c, line, sizeof line) >= 0) {
        s->lines++;
        char out[1024];
        size_t o = 0;
        J *root = parse(ps, line);
        if (!root) {
            o = (size_t)snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Parse error\"},\"id\":null}");
        } else if (root->t == T_ARR) {
            if (!root->child) o = (size_t)snprintf(out, sizeof out, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Invalid Request\"},\"id\":null}");
            else {
                out[o++] = '[';
                int any = 0;
                for (J *r = root->child; r; r = r->next) {
                    char one[300];
                    int n = handle(r, one, sizeof one);
                    if (n == 0) { s->notifs++; continue; }
                    if (any) out[o++] = ',';
                    memcpy(out + o, one, (size_t)n);
                    o += (size_t)n;
                    any = 1;
                }
                out[o++] = ']';
                if (!any) o = 0;
            }
        } else {
            int n = handle(root, out, sizeof out);
            if (n == 0) s->notifs++;
            o = (size_t)n;
        }
        if (o) {
            out[o++] = '\n';
            send_all(fd, out, o);
        }
    }
    free(ps);
    close(fd);
    return NULL;
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
    struct { const char *req; int reply; } t[] = {
        {"{\"jsonrpc\":\"2.0\",\"method\":\"add\",\"params\":[19,23],\"id\":1}", 1},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"subtract\",\"params\":{\"subtrahend\":23,\"minuend\":42},\"id\":\"req-2\"}", 1},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"sum\",\"params\":[[1,2,3,4,5,-5]],\"id\":3}", 1},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"concat\",\"params\":[\"iron\",\"-lang\"],\"id\":4}", 1},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"echo\",\"params\":[{\"k\":[1,true,null,\"x\"],\"e\":{}}],\"id\":5}", 1},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"notify_log\",\"params\":[\"ignored\"]}", 0},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"nope\",\"id\":6}", 1},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"add\",\"params\":[1,\"two\"],\"id\":7}", 1},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"add\",\"params\":[1,2", 1},
        {"{\"method\":\"add\",\"id\":8}", 1},
        {"[1,2]", 1},
        {"[]", 1},
        {"[{\"jsonrpc\":\"2.0\",\"method\":\"add\",\"params\":[1,2],\"id\":\"a\"},"
         "{\"jsonrpc\":\"2.0\",\"method\":\"notify_log\"},"
         "{\"jsonrpc\":\"2.0\",\"method\":\"sum\",\"params\":{\"values\":[10,20]},\"id\":\"b\"},"
         "{\"jsonrpc\":\"2.0\",\"method\":\"zzz\",\"id\":\"c\"}]", 1},
        {"[{\"jsonrpc\":\"2.0\",\"method\":\"notify_log\"},{\"jsonrpc\":\"2.0\",\"method\":\"notify_log\"}]", 0},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"add\",\"params\":[5,5],\"id\":9}", 1},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        CHECK(sendf(fd, "%s\n", t[i].req) == 0);
        if (!t[i].reply) { printf("--> notification, no reply expected\n"); continue; }
        char line[1024];
        CHECK(conn_readline(&c, line, sizeof line) >= 0);
        printf("--> %.60s%s\n<-- %s\n", t[i].req, strlen(t[i].req) > 60 ? "..." : "", line);
    }
    close(fd);
    pthread_join(th, NULL);
    close(s.lfd);
    printf("server lines=%d notifications=%d\n", s.lines, s.notifs);
    return 0;
}
