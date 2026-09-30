/*
 * title: RPN calculator line protocol with transactional lines
 * topic: networking
 * covers: line-oriented request/reply, tokenizer, checked int64 arithmetic, rollback on error, user words, long-line discard
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

#include <limits.h>

#define MAXSTACK 16
#define MAXDEFS 4

typedef struct {
    long long st[MAXSTACK];
    int n;
    char defname[MAXDEFS][12];
    char defbody[MAXDEFS][64];
    int nd;
} Session;

static int add_ok(long long a, long long b, long long *r) {
    if ((b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b)) return 0;
    *r = a + b;
    return 1;
}
static int mul_ok(long long a, long long b, long long *r) {
    if (a == 0 || b == 0) { *r = 0; return 1; }
    if ((a == -1 && b == LLONG_MIN) || (b == -1 && a == LLONG_MIN)) return 0;
    if (a > 0 ? (b > 0 ? a > LLONG_MAX / b : b < LLONG_MIN / a) : (b > 0 ? a < LLONG_MIN / b : a < LLONG_MAX / b)) return 0;
    *r = a * b;
    return 1;
}

/* evaluates tokens; returns NULL or an error string */
static const char *eval(Session *s, char *text, int depth, char *out, size_t ocap, size_t *olen) {
    if (depth > 4) return "recursion too deep";
    for (char *tok = strtok(text, " "); tok; tok = strtok(NULL, " ")) {
        char *end;
        errno = 0;
        long long v = strtoll(tok, &end, 10);
        if (*end == 0 && end != tok) {
            if (errno) return "number out of range";
            if (s->n == MAXSTACK) return "stack full";
            s->st[s->n++] = v;
            continue;
        }
        int def = -1;
        for (int i = 0; i < s->nd; i++)
            if (!strcmp(tok, s->defname[i])) def = i;
        if (def >= 0) {
            /* strtok is not re-entrant: run the body on a copy after saving our position */
            char body[64];
            snprintf(body, sizeof body, "%s", s->defbody[def]);
            char *save = strtok(NULL, "");
            char rest[128];
            snprintf(rest, sizeof rest, "%s", save ? save : "");
            const char *e = eval(s, body, depth + 1, out, ocap, olen);
            if (e) return e;
            /* continue with the remaining tokens of the outer line */
            return rest[0] ? eval(s, rest, depth, out, ocap, olen) : NULL;
        }
        long long a, b, r;
        if (!strcmp(tok, "+") || !strcmp(tok, "-") || !strcmp(tok, "*") || !strcmp(tok, "/") || !strcmp(tok, "%")) {
            if (s->n < 2) return "stack underflow";
            b = s->st[--s->n];
            a = s->st[--s->n];
            int ok = 1;
            switch (tok[0]) {
            case '+': ok = add_ok(a, b, &r); break;
            case '-': ok = b == LLONG_MIN ? 0 : add_ok(a, -b, &r); break;
            case '*': ok = mul_ok(a, b, &r); break;
            default:
                if (b == 0) return "division by zero";
                if (a == LLONG_MIN && b == -1) return "overflow";
                r = tok[0] == '/' ? a / b : a % b;
            }
            if (!ok) return "overflow";
            s->st[s->n++] = r;
        } else if (!strcmp(tok, "dup")) {
            if (s->n < 1) return "stack underflow";
            if (s->n == MAXSTACK) return "stack full";
            s->st[s->n] = s->st[s->n - 1];
            s->n++;
        } else if (!strcmp(tok, "swap")) {
            if (s->n < 2) return "stack underflow";
            long long t = s->st[s->n - 1];
            s->st[s->n - 1] = s->st[s->n - 2];
            s->st[s->n - 2] = t;
        } else if (!strcmp(tok, "over")) {
            if (s->n < 2) return "stack underflow";
            if (s->n == MAXSTACK) return "stack full";
            s->st[s->n] = s->st[s->n - 2];
            s->n++;
        } else if (!strcmp(tok, "drop")) {
            if (s->n < 1) return "stack underflow";
            s->n--;
        } else if (!strcmp(tok, "neg")) {
            if (s->n < 1) return "stack underflow";
            if (s->st[s->n - 1] == LLONG_MIN) return "overflow";
            s->st[s->n - 1] = -s->st[s->n - 1];
        } else if (!strcmp(tok, "sum")) {
            r = 0;
            for (int i = 0; i < s->n; i++)
                if (!add_ok(r, s->st[i], &r)) return "overflow";
            s->n = 0;
            s->st[s->n++] = r;
        } else if (!strcmp(tok, "clear")) {
            s->n = 0;
        } else if (!strcmp(tok, ".")) {
            if (s->n < 1) return "stack underflow";
            *olen += (size_t)snprintf(out + *olen, ocap - *olen, "= %lld ", s->st[s->n - 1]);
        } else
            return "unknown token";
    }
    return NULL;
}

static void serve_conn(int fd) {
    Conn c;
    conn_init(&c, fd);
    Session s;
    memset(&s, 0, sizeof s);
    for (;;) {
        char line[100];
        size_t n = 0;
        int ch, too_long = 0;
        while ((ch = conn_getc(&c)) >= 0 && ch != '\n') {
            if (n + 1 < sizeof line) line[n++] = (char)ch;
            else too_long = 1;
        }
        if (ch < 0) return;
        line[n] = 0;
        if (n && line[n - 1] == '\r') line[--n] = 0;
        if (too_long) { send_str(fd, "ERR line too long\n"); continue; }
        if (!strcmp(line, "quit")) { send_str(fd, "bye\n"); return; }
        if (!strncmp(line, "def ", 4)) {
            char name[12], body[64];
            if (sscanf(line + 4, "%11s %63[^\n]", name, body) == 2 && s.nd < MAXDEFS) {
                snprintf(s.defname[s.nd], 12, "%s", name);
                snprintf(s.defbody[s.nd++], 64, "%s", body);
                sendf(fd, "defined %s\n", name);
            } else
                send_str(fd, "ERR bad definition\n");
            continue;
        }
        if (!strcmp(line, "stack")) {
            char b[300];
            int o = snprintf(b, sizeof b, "[");
            for (int i = 0; i < s.n; i++) o += snprintf(b + o, sizeof b - (size_t)o, "%s%lld", i ? " " : "", s.st[i]);
            sendf(fd, "%s]\n", b);
            continue;
        }
        Session backup = s; /* every line is all-or-nothing */
        char out[256] = "";
        size_t ol = 0;
        char work[100];
        snprintf(work, sizeof work, "%s", line);
        const char *err = eval(&s, work, 0, out, sizeof out, &ol);
        if (err) {
            s = backup;
            sendf(fd, "ERR %s\n", err);
        } else {
            if (ol && out[ol - 1] == ' ') out[ol - 1] = 0;
            sendf(fd, "OK %s\n", out);
        }
    }
}

static void *server_main(void *arg) {
    int lfd = *(int *)arg;
    for (int i = 0; i < 2; i++) {
        int fd = accept_lo(lfd);
        if (fd < 0) return NULL;
        serve_conn(fd);
        close(fd);
    }
    return NULL;
}

int main(void) {
    net_init();
    int port;
    int lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, &lfd)) die("thread");
    const char *script[] = {
        "3 4 + .", "stack", "2 *", "stack", ". ", "10 3 / . 10 3 % .", "-7 2 / . -7 2 % .", "1 0 /", "stack",
        "5 6 swap - .", "1 2 3 4 5 sum .", "stack", "clear 1 2 over . drop drop drop", "stack", "9223372036854775807 1 +",
        "9223372036854775807 neg 1 - .", "9223372036854775807 neg 2 -", "4611686018427387904 2 *", "99999999999999999999",
        "1 2 + frob", "stack", "def sq dup *", "def cube dup sq *", "7 sq . 3 cube .", "2 cube cube .", "def loop loop",
        "loop", "quit",
    };
    for (int sess = 0; sess < 2; sess++) {
        int fd = connect_lo(port);
        Conn c;
        conn_init(&c, fd);
        printf("--- connection %d ---\n", sess + 1);
        if (sess == 0) {
            for (size_t i = 0; i < sizeof script / sizeof script[0]; i++) {
                char r[200];
                CHECK(sendf(fd, "%s\n", script[i]) == 0);
                CHECK(conn_readline(&c, r, sizeof r) >= 0);
                printf("%-38s => %s\n", script[i], r);
            }
        } else {
            /* fresh session: no definitions or stack carried over; an overlong line is discarded whole */
            char big[300], r[200];
            memset(big, '1', sizeof big - 1);
            big[sizeof big - 1] = 0;
            CHECK(sendf(fd, "%s\n", big) == 0);
            CHECK(conn_readline(&c, r, sizeof r) >= 0);
            printf("300-digit line => %s\n", r);
            CHECK(sendf(fd, "stack\n") == 0);
            CHECK(conn_readline(&c, r, sizeof r) >= 0);
            printf("stack => %s\n", r);
            CHECK(sendf(fd, "5 sq\n") == 0);
            CHECK(conn_readline(&c, r, sizeof r) >= 0);
            printf("5 sq (undefined here) => %s\n", r);
            CHECK(sendf(fd, "quit\n") == 0);
            CHECK(conn_readline(&c, r, sizeof r) >= 0);
            printf("quit => %s\n", r);
        }
        close(fd);
    }
    pthread_join(th, NULL);
    close(lfd);
    return 0;
}
