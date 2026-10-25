/*
 * title: Key-value server with append-only log persistence
 * topic: networking
 * covers: line protocol, append-only file, per-record crc, replay on restart, torn tail truncation, compaction
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


#define LOGFILE "kv.log"
#define MAXK 32

typedef struct { char k[24], v[64]; int used; } Slot;

typedef struct {
    int lfd;
    Slot t[MAXK];
    FILE *log;
    int replayed, dropped_tail, appended;
} Server;

static Slot *find(Server *s, const char *k) {
    for (int i = 0; i < MAXK; i++)
        if (s->t[i].used && !strcmp(s->t[i].k, k)) return &s->t[i];
    return NULL;
}

static void put(Server *s, const char *k, const char *v) {
    Slot *e = find(s, k);
    for (int i = 0; i < MAXK && !e; i++)
        if (!s->t[i].used) e = &s->t[i];
    if (!e) die("table full");
    e->used = 1;
    snprintf(e->k, sizeof e->k, "%s", k);
    snprintf(e->v, sizeof e->v, "%s", v);
}

static void del(Server *s, const char *k) {
    Slot *e = find(s, k);
    if (e) e->used = 0;
}

static void write_rec(FILE *f, char op, const char *k, const char *v) {
    char body[128];
    snprintf(body, sizeof body, "%c\t%s\t%s", op, k, v);
    fprintf(f, "%s\t%08x\n", body, crc32_buf((unsigned char *)body, strlen(body)));
    fflush(f);
}

/* replays the log; stops at the first bad record and truncates the file there */
static void replay(Server *s) {
    FILE *f = fopen(LOGFILE, "r");
    if (!f) return;
    long good = 0;
    char line[256];
    for (;;) {
        long start = ftell(f);
        if (!fgets(line, sizeof line, f)) break;
        size_t l = strlen(line);
        int ok = l > 10 && line[l - 1] == '\n';
        if (ok) {
            line[l - 1] = 0;
            char *last = strrchr(line, '\t');
            ok = last != NULL;
            if (ok) {
                *last++ = 0;
                char want[16];
                snprintf(want, sizeof want, "%08x", crc32_buf((unsigned char *)line, strlen(line)));
                ok = strcmp(want, last) == 0;
            }
        }
        if (!ok) { s->dropped_tail = 1; good = start; goto trunc; }
        char *op = line, *k = strchr(line, '\t'), *v = k ? strchr(k + 1, '\t') : NULL;
        if (!k || !v) { s->dropped_tail = 1; good = start; goto trunc; }
        *k++ = 0;
        *v++ = 0;
        if (op[0] == 'S') put(s, k, v); else del(s, k);
        s->replayed++;
        good = ftell(f);
    }
    fclose(f);
    return;
trunc:
    fclose(f);
    if (truncate(LOGFILE, good) != 0) die("truncate");
}

static void compact(Server *s) {
    fclose(s->log);
    FILE *f = fopen("kv.tmp", "w");
    for (int i = 0; i < MAXK; i++)
        if (s->t[i].used) write_rec(f, 'S', s->t[i].k, s->t[i].v);
    fclose(f);
    if (rename("kv.tmp", LOGFILE) != 0) die("rename");
    s->log = fopen(LOGFILE, "a");
}

static void *server_main(void *arg) {
    Server *s = arg;
    replay(s);
    s->log = fopen(LOGFILE, "a");
    int fd = accept_lo(s->lfd);
    if (fd < 0) return NULL;
    Conn c;
    conn_init(&c, fd);
    char line[200];
    while (conn_readline(&c, line, sizeof line) >= 0) {
        char cmd[12] = "", k[24] = "", v[64] = "";
        int n = sscanf(line, "%11s %23s %63[^\n]", cmd, k, v);
        if (!strcmp(cmd, "SET") && n == 3) {
            write_rec(s->log, 'S', k, v);
            put(s, k, v);
            s->appended++;
            send_str(fd, "OK\n");
        } else if (!strcmp(cmd, "GET") && n >= 2) {
            Slot *e = find(s, k);
            if (e) sendf(fd, "VAL %s\n", e->v); else send_str(fd, "NIL\n");
        } else if (!strcmp(cmd, "DEL") && n >= 2) {
            Slot *e = find(s, k);
            if (e) { write_rec(s->log, 'D', k, "-"); s->appended++; }
            del(s, k);
            send_str(fd, e ? "1\n" : "0\n");
        } else if (!strcmp(cmd, "COUNT")) {
            int cnt = 0;
            for (int i = 0; i < MAXK; i++) cnt += s->t[i].used;
            sendf(fd, "%d\n", cnt);
        } else if (!strcmp(cmd, "COMPACT")) {
            compact(s);
            send_str(fd, "OK\n");
        } else if (!strcmp(cmd, "QUIT")) {
            send_str(fd, "BYE\n");
            break;
        } else
            send_str(fd, "ERR\n");
    }
    fclose(s->log);
    close(fd);
    return NULL;
}

static int count_lines(void) {
    FILE *f = fopen(LOGFILE, "r");
    if (!f) return -1;
    int n = 0, ch;
    while ((ch = fgetc(f)) != EOF) n += ch == '\n';
    fclose(f);
    return n;
}

static void ask(Conn *c, const char *q) {
    char line[128];
    CHECK(sendf(c->fd, "%s\n", q) == 0);
    CHECK(conn_readline(c, line, sizeof line) >= 0);
    printf("  %-22s -> %s\n", q, line);
}

static void run_server(const char *title, void (*script)(Conn *), Server *out) {
    memset(out, 0, sizeof *out);
    int port;
    out->lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, out)) die("thread");
    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);
    printf("%s\n", title);
    script(&c);
    close(fd);
    pthread_join(th, NULL);
    close(out->lfd);
}

static void script1(Conn *c) {
    ask(c, "SET alpha one");
    ask(c, "SET beta two words");
    ask(c, "SET gamma 3");
    ask(c, "SET alpha uno");
    ask(c, "DEL beta");
    ask(c, "DEL beta");
    ask(c, "COUNT");
    ask(c, "QUIT");
}

static void script2(Conn *c) {
    ask(c, "GET alpha");
    ask(c, "GET beta");
    ask(c, "GET gamma");
    ask(c, "SET delta 4");
    ask(c, "COUNT");
    ask(c, "COMPACT");
    ask(c, "SET epsilon 5");
    ask(c, "QUIT");
}

static void script3(Conn *c) {
    ask(c, "GET alpha");
    ask(c, "GET delta");
    ask(c, "GET epsilon");
    ask(c, "COUNT");
    ask(c, "QUIT");
}

int main(void) {
    net_init();
    unlink(LOGFILE);
    Server *s = malloc(sizeof *s);
    if (!s) die("oom");
    run_server("session 1 (empty start)", script1, s);
    printf("  replayed=%d appended=%d log_lines=%d\n", s->replayed, s->appended, count_lines());
    CHECK(s->replayed == 0 && count_lines() == 5);

    /* simulate a crash mid-write: a record without newline or valid crc */
    FILE *f = fopen(LOGFILE, "a");
    fputs("S\tzeta\tpart", f);
    fclose(f);

    run_server("session 2 (restart after torn write)", script2, s);
    printf("  replayed=%d dropped_tail=%d appended=%d log_lines=%d\n", s->replayed, s->dropped_tail, s->appended, count_lines());
    CHECK(s->replayed == 5 && s->dropped_tail == 1);

    /* flip a byte in the middle of a record: replay stops there */
    run_server("session 3 (clean restart)", script3, s);
    printf("  replayed=%d dropped_tail=%d log_lines=%d\n", s->replayed, s->dropped_tail, count_lines());
    CHECK(s->replayed == 4 && s->dropped_tail == 0);

    f = fopen(LOGFILE, "r+");
    CHECK(f != NULL);
    fseek(f, 3, SEEK_SET);
    fputc('X', f);
    fclose(f);
    run_server("session 4 (corrupted first record)", script3, s);
    printf("  replayed=%d dropped_tail=%d log_lines=%d\n", s->replayed, s->dropped_tail, count_lines());
    CHECK(s->replayed == 0 && s->dropped_tail == 1);
    unlink(LOGFILE);
    free(s);
    return 0;
}
