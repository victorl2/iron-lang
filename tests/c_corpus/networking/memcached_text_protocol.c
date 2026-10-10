/*
 * title: memcached text protocol subset
 * topic: networking
 * covers: set/add/replace/append/prepend, get/gets multi-key, cas tokens, incr/decr, expiry with a fake clock, noreply
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


#define MAXI 16
typedef struct {
    int used;
    char key[64];
    unsigned flags;
    long expires; /* 0 = never, else fake-clock seconds */
    unsigned long cas;
    unsigned char *data;
    size_t len;
} Item;

typedef struct {
    int lfd;
    Item items[MAXI];
    long now;
    unsigned long next_cas;
    int hits, misses;
} Server;

static Item *lookup(Server *s, const char *k) {
    for (int i = 0; i < MAXI; i++) {
        Item *it = &s->items[i];
        if (!it->used || strcmp(it->key, k)) continue;
        if (it->expires && it->expires <= s->now) {
            free(it->data);
            it->used = 0;
            return NULL;
        }
        return it;
    }
    return NULL;
}

static Item *store(Server *s, const char *k, unsigned flags, long exp, const unsigned char *d, size_t n) {
    Item *it = lookup(s, k);
    if (!it) {
        for (int i = 0; i < MAXI && !it; i++)
            if (!s->items[i].used) it = &s->items[i];
        if (!it) die("cache full");
        it->used = 1;
        snprintf(it->key, sizeof it->key, "%s", k);
    } else
        free(it->data);
    it->flags = flags;
    it->expires = exp > 0 ? s->now + exp : 0;
    it->cas = ++s->next_cas;
    it->data = malloc(n + 1);
    memcpy(it->data, d, n);
    it->data[n] = 0;
    it->len = n;
    return it;
}

static void serve(Server *s, int fd) {
    Conn c;
    conn_init(&c, fd);
    char line[512];
    while (conn_readline(&c, line, sizeof line) >= 0) {
        if (!line[0]) continue;
        char cmd[16] = "", key[300] = "";
        unsigned flags = 0;
        long exp = 0, bytes = 0;
        unsigned long casid = 0;
        int noreply = strstr(line, " noreply") != NULL;
        sscanf(line, "%15s %299s", cmd, key);
        int is_store = !strcmp(cmd, "set") || !strcmp(cmd, "add") || !strcmp(cmd, "replace") ||
                       !strcmp(cmd, "append") || !strcmp(cmd, "prepend") || !strcmp(cmd, "cas");
        if (is_store) {
            int got = !strcmp(cmd, "cas") ? sscanf(line, "%*s %*s %u %ld %ld %lu", &flags, &exp, &bytes, &casid)
                                          : sscanf(line, "%*s %*s %u %ld %ld", &flags, &exp, &bytes);
            if (got < 3 || bytes < 0 || bytes > 1000 || strlen(key) > 250) {
                send_str(fd, strlen(key) > 250 ? "CLIENT_ERROR key too long\r\n" : "CLIENT_ERROR bad command line format\r\n");
                continue;
            }
            unsigned char *buf = malloc((size_t)bytes + 2);
            if (conn_readn(&c, buf, (size_t)bytes + 2) < 0) { free(buf); return; }
            if (buf[bytes] != '\r' || buf[bytes + 1] != '\n') {
                free(buf);
                send_str(fd, "CLIENT_ERROR bad data chunk\r\n");
                continue;
            }
            Item *it = lookup(s, key);
            const char *res = "STORED";
            if (!strcmp(cmd, "add") && it) res = "NOT_STORED";
            else if ((!strcmp(cmd, "replace") || !strcmp(cmd, "append") || !strcmp(cmd, "prepend")) && !it) res = "NOT_STORED";
            else if (!strcmp(cmd, "cas") && !it) res = "NOT_FOUND";
            else if (!strcmp(cmd, "cas") && it->cas != casid) res = "EXISTS";
            else if (!strcmp(cmd, "append") || !strcmp(cmd, "prepend")) {
                unsigned char *nd = malloc(it->len + (size_t)bytes);
                if (cmd[0] == 'a') { memcpy(nd, it->data, it->len); memcpy(nd + it->len, buf, (size_t)bytes); }
                else { memcpy(nd, buf, (size_t)bytes); memcpy(nd + bytes, it->data, it->len); }
                store(s, key, it->flags, 0, nd, it->len + (size_t)bytes);
                free(nd);
            } else
                store(s, key, flags, exp, buf, (size_t)bytes);
            free(buf);
            if (!noreply) sendf(fd, "%s\r\n", res);
        } else if (!strcmp(cmd, "get") || !strcmp(cmd, "gets")) {
            char *p = line + strlen(cmd);
            char k[300];
            int off;
            while (sscanf(p, " %299s%n", k, &off) == 1) {
                p += off;
                Item *it = lookup(s, k);
                if (!it) { s->misses++; continue; }
                s->hits++;
                if (cmd[3] == 's') sendf(fd, "VALUE %s %u %zu %lu\r\n", k, it->flags, it->len, it->cas);
                else sendf(fd, "VALUE %s %u %zu\r\n", k, it->flags, it->len);
                send_all(fd, it->data, it->len);
                send_str(fd, "\r\n");
            }
            send_str(fd, "END\r\n");
        } else if (!strcmp(cmd, "delete")) {
            Item *it = lookup(s, key);
            if (it) { free(it->data); it->used = 0; }
            if (!noreply) send_str(fd, it ? "DELETED\r\n" : "NOT_FOUND\r\n");
        } else if (!strcmp(cmd, "incr") || !strcmp(cmd, "decr")) {
            unsigned long by = 0;
            sscanf(line, "%*s %*s %lu", &by);
            Item *it = lookup(s, key);
            if (!it) { send_str(fd, "NOT_FOUND\r\n"); continue; }
            char *end;
            unsigned long v = strtoul((char *)it->data, &end, 10);
            if (*end || it->len == 0) { send_str(fd, "CLIENT_ERROR cannot increment or decrement non-numeric value\r\n"); continue; }
            if (cmd[0] == 'i') v += by; else v = by > v ? 0 : v - by;
            char t[32];
            int n = snprintf(t, sizeof t, "%lu", v);
            store(s, key, it->flags, 0, (unsigned char *)t, (size_t)n);
            sendf(fd, "%s\r\n", t);
        } else if (!strcmp(cmd, "touch")) {
            long e = 0;
            sscanf(line, "%*s %*s %ld", &e);
            Item *it = lookup(s, key);
            if (it) it->expires = e > 0 ? s->now + e : 0;
            send_str(fd, it ? "TOUCHED\r\n" : "NOT_FOUND\r\n");
        } else if (!strcmp(cmd, "flush_all")) {
            for (int i = 0; i < MAXI; i++)
                if (s->items[i].used) { free(s->items[i].data); s->items[i].used = 0; }
            send_str(fd, "OK\r\n");
        } else if (!strcmp(cmd, "advance")) { /* test hook: move the fake clock */
            s->now += atol(key);
            send_str(fd, "OK\r\n");
        } else if (!strcmp(cmd, "quit")) {
            return;
        } else
            send_str(fd, "ERROR\r\n");
    }
}

static void *server_main(void *arg) {
    Server *s = arg;
    int fd = accept_lo(s->lfd);
    if (fd < 0) return NULL;
    serve(s, fd);
    close(fd);
    return NULL;
}

/* send raw text, print the reply. multi=1 reads until END */
static void call(Conn *c, const char *req, int multi) {
    CHECK(send_str(c->fd, req) == 0);
    char shown[128];
    size_t k = 0;
    for (const char *p = req; *p && k < 100; p++) {
        if (*p == '\r') { shown[k++] = '\\'; shown[k++] = 'r'; }
        else if (*p == '\n') { shown[k++] = '\\'; shown[k++] = 'n'; }
        else shown[k++] = *p;
    }
    shown[k] = 0;
    printf(">> %s\n", shown);
    char line[300];
    if (multi == 2) { printf("   (no reply)\n"); return; }
    for (;;) {
        if (conn_readline(c, line, sizeof line) < 0) die("eof");
        printf("   %s\n", line);
        if (!multi || !strcmp(line, "END")) break;
        if (!strncmp(line, "VALUE", 5)) {
            char d[300];
            int len;
            sscanf(line, "%*s %*s %*s %d", &len);
            CHECK(conn_readn(c, d, (size_t)len + 2) == 0);
            printf("   <%d data bytes>\n", len);
        }
    }
}

int main(void) {
    net_init();
    Server *s = calloc(1, sizeof *s);
    if (!s) die("oom");
    int port;
    s->lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, s)) die("thread");
    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);
    call(&c, "set a 5 0 3\r\nabc\r\n", 0);
    call(&c, "add a 0 0 1\r\nx\r\n", 0);
    call(&c, "add b 7 0 2\r\nhi\r\n", 0);
    call(&c, "replace zz 0 0 1\r\nx\r\n", 0);
    call(&c, "append a 0 0 2\r\nde\r\n", 0);
    call(&c, "prepend a 0 0 2\r\n<<\r\n", 0);
    call(&c, "get a b nothing\r\n", 1);
    call(&c, "gets a\r\n", 1);
    call(&c, "cas a 5 0 4 1\r\nnope\r\n", 0);
    call(&c, "cas a 5 0 4 4\r\nWXYZ\r\n", 0);
    call(&c, "cas gone 0 0 1 1\r\nx\r\n", 0);
    call(&c, "gets a\r\n", 1);
    call(&c, "set n 0 0 2\r\n10\r\n", 0);
    call(&c, "incr n 5\r\n", 0);
    call(&c, "decr n 100\r\n", 0);
    call(&c, "incr a 1\r\n", 0);
    call(&c, "incr missing 1\r\n", 0);
    call(&c, "set bin 0 0 7\r\nx\r\ny\r\nz\r\n", 0);
    call(&c, "get bin\r\n", 1);
    call(&c, "set bad 0 0 3\r\nabcde\r\n", 0);
    call(&c, "set quiet 0 0 1 noreply\r\nq\r\n", 2);
    call(&c, "get quiet\r\n", 1);
    call(&c, "set ttl 1 10 3\r\nnew\r\n", 0);
    call(&c, "advance 9\r\n", 0);
    call(&c, "get ttl\r\n", 1);
    call(&c, "touch ttl 100\r\n", 0);
    call(&c, "advance 50\r\n", 0);
    call(&c, "get ttl\r\n", 1);
    call(&c, "advance 100\r\n", 0);
    call(&c, "get ttl\r\n", 1);
    call(&c, "delete a\r\n", 0);
    call(&c, "delete a\r\n", 0);
    call(&c, "frobnicate\r\n", 0);
    call(&c, "flush_all\r\n", 0);
    call(&c, "get b n bin\r\n", 1);
    CHECK(send_str(fd, "quit\r\n") == 0);
    close(fd);
    pthread_join(th, NULL);
    close(s->lfd);
    printf("hits=%d misses=%d\n", s->hits, s->misses);
    for (int i = 0; i < MAXI; i++)
        if (s->items[i].used) free(s->items[i].data);
    free(s);
    return 0;
}
