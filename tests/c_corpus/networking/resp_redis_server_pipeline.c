/*
 * title: Redis RESP server with pipelining
 * topic: networking
 * covers: RESP arrays and bulk strings, string and list commands, error replies, pipelined batch, fragmented input
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
    char *key;
    int is_list;
    char *str;
    size_t slen;
    char **items;
    size_t *ilen;
    int n;
} Entry;

typedef struct {
    int lfd;
    Entry *e;
    int ne, cap;
    int cmds;
} Server;

static Entry *find(Server *s, const char *k) {
    for (int i = 0; i < s->ne; i++)
        if (!strcmp(s->e[i].key, k)) return &s->e[i];
    return NULL;
}

static Entry *create(Server *s, const char *k) {
    if (s->ne == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->e = realloc(s->e, (size_t)s->cap * sizeof *s->e);
        if (!s->e) die("oom");
    }
    Entry *e = &s->e[s->ne++];
    memset(e, 0, sizeof *e);
    e->key = strdup(k);
    return e;
}

static void free_entry_body(Entry *e) {
    free(e->str);
    for (int i = 0; i < e->n; i++) free(e->items[i]);
    free(e->items);
    free(e->ilen);
    e->str = NULL;
    e->items = NULL;
    e->ilen = NULL;
    e->n = 0;
}

static void remove_entry(Server *s, Entry *e) {
    free_entry_body(e);
    free(e->key);
    *e = s->e[--s->ne];
}

static void set_str(Entry *e, const char *v, size_t n) {
    free(e->str);
    e->str = malloc(n + 1);
    memcpy(e->str, v, n);
    e->str[n] = 0;
    e->slen = n;
}

static void push(Entry *e, const char *v, size_t n, int front) {
    e->items = realloc(e->items, (size_t)(e->n + 1) * sizeof *e->items);
    e->ilen = realloc(e->ilen, (size_t)(e->n + 1) * sizeof *e->ilen);
    if (front) {
        memmove(e->items + 1, e->items, (size_t)e->n * sizeof *e->items);
        memmove(e->ilen + 1, e->ilen, (size_t)e->n * sizeof *e->ilen);
    }
    int at = front ? 0 : e->n;
    e->items[at] = malloc(n + 1);
    memcpy(e->items[at], v, n);
    e->items[at][n] = 0;
    e->ilen[at] = n;
    e->n++;
}

static void bulk(int fd, const char *v, size_t n) {
    sendf(fd, "$%zu\r\n", n);
    send_all(fd, v, n);
    send_str(fd, "\r\n");
}

static int parse_int(const char *s, size_t n, long *out) {
    if (n == 0 || n > 18) return 0;
    char *end;
    char tmp[24];
    memcpy(tmp, s, n);
    tmp[n] = 0;
    *out = strtol(tmp, &end, 10);
    return *end == 0;
}

/* reads one command; returns argc or -1 on EOF */
static int read_cmd(Conn *c, char **argv, size_t *alen, int max) {
    char line[64];
    if (conn_readline(c, line, sizeof line) < 0) return -1;
    if (line[0] != '*') return -1;
    int n = atoi(line + 1);
    if (n < 1 || n > max) return -1;
    for (int i = 0; i < n; i++) {
        if (conn_readline(c, line, sizeof line) < 0 || line[0] != '$') return -1;
        size_t len = (size_t)atoi(line + 1);
        argv[i] = malloc(len + 3);
        if (conn_readn(c, argv[i], len + 2) < 0) return -1;
        argv[i][len] = 0;
        alen[i] = len;
    }
    return n;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void exec_cmd(Server *s, int fd, int argc, char **av, size_t *al) {
    for (char *p = av[0]; *p; p++) *p = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
    const char *c = av[0];
    Entry *e;
#define ARITY(n) if (argc != (n)) { send_str(fd, "-ERR wrong number of arguments\r\n"); return; }
    if (!strcmp(c, "PING")) {
        if (argc == 1) send_str(fd, "+PONG\r\n"); else bulk(fd, av[1], al[1]);
    } else if (!strcmp(c, "SET")) {
        ARITY(3);
        e = find(s, av[1]);
        if (e && e->is_list) { free_entry_body(e); e->is_list = 0; }
        if (!e) e = create(s, av[1]);
        set_str(e, av[2], al[2]);
        send_str(fd, "+OK\r\n");
    } else if (!strcmp(c, "GET")) {
        ARITY(2);
        e = find(s, av[1]);
        if (!e) send_str(fd, "$-1\r\n");
        else if (e->is_list) send_str(fd, "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
        else bulk(fd, e->str, e->slen);
    } else if (!strcmp(c, "DEL") || !strcmp(c, "EXISTS")) {
        int del = !strcmp(c, "DEL"), cnt = 0;
        for (int i = 1; i < argc; i++)
            if ((e = find(s, av[i]))) { cnt++; if (del) remove_entry(s, e); }
        sendf(fd, ":%d\r\n", cnt);
    } else if (!strcmp(c, "INCR") || !strcmp(c, "DECR") || !strcmp(c, "INCRBY")) {
        long by = !strcmp(c, "INCR") ? 1 : !strcmp(c, "DECR") ? -1 : 0;
        if (!strcmp(c, "INCRBY")) { ARITY(3); if (!parse_int(av[2], al[2], &by)) { send_str(fd, "-ERR value is not an integer or out of range\r\n"); return; } }
        else ARITY(2);
        long v = 0;
        e = find(s, av[1]);
        if (e && (e->is_list || !parse_int(e->str, e->slen, &v))) { send_str(fd, "-ERR value is not an integer or out of range\r\n"); return; }
        v += by;
        char t[32];
        int n = snprintf(t, sizeof t, "%ld", v);
        if (!e) e = create(s, av[1]);
        set_str(e, t, (size_t)n);
        sendf(fd, ":%ld\r\n", v);
    } else if (!strcmp(c, "APPEND")) {
        ARITY(3);
        e = find(s, av[1]);
        if (!e) { e = create(s, av[1]); set_str(e, "", 0); }
        char *nb = malloc(e->slen + al[2] + 1);
        memcpy(nb, e->str, e->slen);
        memcpy(nb + e->slen, av[2], al[2]);
        set_str(e, nb, e->slen + al[2]);
        free(nb);
        sendf(fd, ":%zu\r\n", e->slen);
    } else if (!strcmp(c, "STRLEN")) {
        ARITY(2);
        e = find(s, av[1]);
        sendf(fd, ":%zu\r\n", e && !e->is_list ? e->slen : 0);
    } else if (!strcmp(c, "MSET")) {
        if (argc < 3 || argc % 2 == 0) { send_str(fd, "-ERR wrong number of arguments\r\n"); return; }
        for (int i = 1; i < argc; i += 2) {
            e = find(s, av[i]);
            if (!e) e = create(s, av[i]);
            set_str(e, av[i + 1], al[i + 1]);
        }
        send_str(fd, "+OK\r\n");
    } else if (!strcmp(c, "MGET")) {
        sendf(fd, "*%d\r\n", argc - 1);
        for (int i = 1; i < argc; i++) {
            e = find(s, av[i]);
            if (e && !e->is_list) bulk(fd, e->str, e->slen); else send_str(fd, "$-1\r\n");
        }
    } else if (!strcmp(c, "LPUSH") || !strcmp(c, "RPUSH")) {
        if (argc < 3) { send_str(fd, "-ERR wrong number of arguments\r\n"); return; }
        e = find(s, av[1]);
        if (e && !e->is_list) { send_str(fd, "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"); return; }
        if (!e) { e = create(s, av[1]); e->is_list = 1; }
        for (int i = 2; i < argc; i++) push(e, av[i], al[i], c[0] == 'L');
        sendf(fd, ":%d\r\n", e->n);
    } else if (!strcmp(c, "LPOP")) {
        ARITY(2);
        e = find(s, av[1]);
        if (!e || !e->is_list || e->n == 0) { send_str(fd, "$-1\r\n"); return; }
        bulk(fd, e->items[0], e->ilen[0]);
        free(e->items[0]);
        memmove(e->items, e->items + 1, (size_t)(e->n - 1) * sizeof *e->items);
        memmove(e->ilen, e->ilen + 1, (size_t)(e->n - 1) * sizeof *e->ilen);
        if (--e->n == 0) remove_entry(s, e);
    } else if (!strcmp(c, "LLEN")) {
        ARITY(2);
        e = find(s, av[1]);
        sendf(fd, ":%d\r\n", e && e->is_list ? e->n : 0);
    } else if (!strcmp(c, "LRANGE")) {
        ARITY(4);
        e = find(s, av[1]);
        long a = atol(av[2]), b = atol(av[3]);
        int n = e && e->is_list ? e->n : 0;
        if (a < 0) a += n;
        if (b < 0) b += n;
        if (a < 0) a = 0;
        if (b >= n) b = n - 1;
        int cnt = b >= a ? (int)(b - a + 1) : 0;
        sendf(fd, "*%d\r\n", cnt);
        for (int i = 0; i < cnt; i++) bulk(fd, e->items[a + i], e->ilen[a + i]);
    } else if (!strcmp(c, "TYPE")) {
        ARITY(2);
        e = find(s, av[1]);
        sendf(fd, "+%s\r\n", !e ? "none" : e->is_list ? "list" : "string");
    } else if (!strcmp(c, "KEYS")) {
        ARITY(2);
        char **k = malloc((size_t)(s->ne + 1) * sizeof *k);
        for (int i = 0; i < s->ne; i++) k[i] = s->e[i].key;
        qsort(k, (size_t)s->ne, sizeof *k, cmp_str);
        sendf(fd, "*%d\r\n", s->ne);
        for (int i = 0; i < s->ne; i++) bulk(fd, k[i], strlen(k[i]));
        free(k);
    } else {
        sendf(fd, "-ERR unknown command '%s'\r\n", av[0]);
    }
#undef ARITY
}

static void *server_main(void *arg) {
    Server *s = arg;
    int fd = accept_lo(s->lfd);
    if (fd < 0) return NULL;
    Conn c;
    conn_init(&c, fd);
    for (;;) {
        char *av[16] = {0};
        size_t al[16] = {0};
        int argc = read_cmd(&c, av, al, 16);
        if (argc < 0) {
            for (int i = 0; i < 16; i++) free(av[i]);
            break;
        }
        s->cmds++;
        exec_cmd(s, fd, argc, av, al);
        for (int i = 0; i < argc; i++) free(av[i]);
    }
    close(fd);
    return NULL;
}

/* ---- client ---- */
static size_t enc_cmd(char *out, size_t cap, int argc, const char **av, const size_t *al) {
    size_t o = (size_t)snprintf(out, cap, "*%d\r\n", argc);
    for (int i = 0; i < argc; i++) {
        o += (size_t)snprintf(out + o, cap - o, "$%zu\r\n", al[i]);
        memcpy(out + o, av[i], al[i]);
        o += al[i];
        out[o++] = '\r';
        out[o++] = '\n';
    }
    return o;
}

static void print_reply(Conn *c, int depth) {
    char line[256];
    if (conn_readline(c, line, sizeof line) < 0) die("eof reply");
    switch (line[0]) {
    case '+': printf("%*s%s\n", depth * 2, "", line); break;
    case '-': printf("%*s%s\n", depth * 2, "", line); break;
    case ':': printf("%*s(int) %s\n", depth * 2, "", line + 1); break;
    case '$': {
        int n = atoi(line + 1);
        if (n < 0) { printf("%*s(nil)\n", depth * 2, ""); break; }
        char buf[256];
        CHECK(n < 250 && conn_readn(c, buf, (size_t)n + 2) == 0);
        printf("%*s\"", depth * 2, "");
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\r') printf("\\r"); else if (buf[i] == '\n') printf("\\n"); else putchar(buf[i]);
        }
        printf("\"\n");
        break;
    }
    case '*': {
        int n = atoi(line + 1);
        printf("%*s[%d items]\n", depth * 2, "", n);
        for (int i = 0; i < n; i++) print_reply(c, depth + 1);
        break;
    }
    default: die("bad reply type");
    }
}

int main(void) {
    net_init();
    Server s;
    memset(&s, 0, sizeof s);
    int port;
    s.lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, &s)) die("thread");
    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);

    const char *script[][6] = {
        {"SET", "greeting", "hello", 0}, {"GET", "greeting"}, {"APPEND", "greeting", " world"},
        {"STRLEN", "greeting"}, {"INCR", "hits"}, {"INCRBY", "hits", "41"}, {"DECR", "hits"},
        {"INCR", "greeting"}, {"LPUSH", "q", "a", "b", "c"}, {"RPUSH", "q", "z"},
        {"LRANGE", "q", "0", "-1"}, {"LRANGE", "q", "1", "2"}, {"LLEN", "q"}, {"LPOP", "q"},
        {"GET", "q"}, {"TYPE", "q"}, {"TYPE", "hits"}, {"TYPE", "nothing"}, {"MSET", "k1", "v1", "k2", "v2"},
        {"MGET", "k1", "nope", "k2"}, {"EXISTS", "k1", "k2", "k3"}, {"DEL", "k1", "k3", "hits"},
        {"KEYS", "*"}, {"LPUSH", "greeting", "x"}, {"BOGUS"}, {"GET"}, {"PING"}, {"PING", "pong-me"},
    };
    int n = (int)(sizeof script / sizeof script[0]);
    static char wire[8192];
    size_t w = 0;
    for (int i = 0; i < n; i++) {
        const char *av[6];
        size_t al[6];
        int ac = 0;
        while (ac < 6 && script[i][ac]) { av[ac] = script[i][ac]; al[ac] = strlen(av[ac]); ac++; }
        w += enc_cmd(wire + w, sizeof wire - w, ac, av, al);
    }
    CHECK(send_all(fd, wire, w) == 0); /* the whole batch in one write */
    for (int i = 0; i < n; i++) {
        printf("> %s", script[i][0]);
        for (int j = 1; j < 6 && script[i][j]; j++) printf(" %s", script[i][j]);
        printf("\n");
        print_reply(&c, 1);
    }
    /* binary-safe value with CRLF, delivered one byte at a time */
    const char *av[3] = {"SET", "bin", "a\r\nb\r\n\r\nc"};
    size_t al[3] = {3, 3, 9};
    char one[128];
    size_t ol = enc_cmd(one, sizeof one, 3, av, al);
    for (size_t i = 0; i < ol; i++) CHECK(send_all(fd, one + i, 1) == 0);
    printf("> SET bin <9 bytes with CRLF, sent bytewise>\n");
    print_reply(&c, 1);
    CHECK(send_str(fd, "*2\r\n$3\r\nGET\r\n$3\r\nbin\r\n") == 0);
    print_reply(&c, 1);
    close(fd);
    pthread_join(th, NULL);
    close(s.lfd);
    printf("server executed %d commands, %d keys remain\n", s.cmds, s.ne);
    for (int i = 0; i < s.ne; i++) {
        free_entry_body(&s.e[i]);
        free(s.e[i].key);
    }
    free(s.e);
    return 0;
}
