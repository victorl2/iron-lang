/*
 * title: FTP-like control and data channels (PASV and PORT)
 * topic: networking
 * covers: control connection replies, passive listener, active PORT connect, LIST/RETR/STOR, cwd, login state
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


#define MAXF 8
typedef struct { char path[32]; unsigned char *data; size_t len; int used; } VFile;

typedef struct {
    int lfd;
    VFile fs[MAXF];
    int data_conns;
} Server;

static VFile *vf_find(Server *s, const char *p) {
    for (int i = 0; i < MAXF; i++)
        if (s->fs[i].used && !strcmp(s->fs[i].path, p)) return &s->fs[i];
    return NULL;
}

static void vf_put(Server *s, const char *p, const unsigned char *d, size_t n) {
    VFile *f = vf_find(s, p);
    if (!f)
        for (int i = 0; i < MAXF && !f; i++)
            if (!s->fs[i].used) f = &s->fs[i];
    if (!f) die("fs full");
    free(f->data);
    f->data = malloc(n + 1);
    memcpy(f->data, d, n);
    f->len = n;
    f->used = 1;
    snprintf(f->path, sizeof f->path, "%s", p);
}

static void full_path(const char *cwd, const char *name, char *out, size_t cap) {
    if (!strcmp(cwd, "/")) snprintf(out, cap, "%s", name);
    else snprintf(out, cap, "%s/%s", cwd + 1, name);
}

static void *server_main(void *arg) {
    Server *s = arg;
    int fd = accept_lo(s->lfd);
    if (fd < 0) return NULL;
    Conn c;
    conn_init(&c, fd);
    send_str(fd, "220 iron ftpd ready\r\n");
    int authed = 0, user_ok = 0;
    char cwd[32] = "/";
    int pasv_fd = -1, active_port = -1;
    char line[200];
    while (conn_readline(&c, line, sizeof line) >= 0) {
        char cmd[8] = "", arg1[80] = "";
        sscanf(line, "%7s %79[^\n]", cmd, arg1);
        for (char *p = cmd; *p; p++) *p = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
        if (!strcmp(cmd, "USER")) { user_ok = !strcmp(arg1, "anon"); send_str(fd, "331 password please\r\n"); continue; }
        if (!strcmp(cmd, "PASS")) {
            if (user_ok && arg1[0]) { authed = 1; send_str(fd, "230 logged in\r\n"); }
            else send_str(fd, "530 login incorrect\r\n");
            continue;
        }
        if (!strcmp(cmd, "QUIT")) { send_str(fd, "221 goodbye\r\n"); break; }
        if (!authed) { send_str(fd, "530 not logged in\r\n"); continue; }
        if (!strcmp(cmd, "SYST")) send_str(fd, "215 UNIX Type: L8\r\n");
        else if (!strcmp(cmd, "TYPE")) sendf(fd, "200 type set to %s\r\n", arg1);
        else if (!strcmp(cmd, "PWD")) sendf(fd, "257 \"%s\" is current directory\r\n", cwd);
        else if (!strcmp(cmd, "CWD")) {
            if (!strcmp(arg1, "/") || !strcmp(arg1, "/pub")) { snprintf(cwd, sizeof cwd, "%s", arg1); send_str(fd, "250 directory changed\r\n"); }
            else send_str(fd, "550 no such directory\r\n");
        } else if (!strcmp(cmd, "PASV")) {
            if (pasv_fd >= 0) close(pasv_fd);
            int p;
            pasv_fd = listen_lo(&p);
            active_port = -1;
            sendf(fd, "227 Entering Passive Mode (127,0,0,1,%d,%d)\r\n", p >> 8, p & 255);
        } else if (!strcmp(cmd, "PORT")) {
            int a, b, c1, d, p1, p2;
            if (sscanf(arg1, "%d,%d,%d,%d,%d,%d", &a, &b, &c1, &d, &p1, &p2) != 6) { send_str(fd, "501 bad PORT\r\n"); continue; }
            active_port = p1 * 256 + p2;
            if (pasv_fd >= 0) { close(pasv_fd); pasv_fd = -1; }
            send_str(fd, "200 PORT ok\r\n");
        } else if (!strcmp(cmd, "LIST") || !strcmp(cmd, "RETR") || !strcmp(cmd, "STOR")) {
            char path[64];
            full_path(cwd, arg1, path, sizeof path);
            VFile *f = vf_find(s, path);
            int is_stor = cmd[0] == 'S', is_list = cmd[0] == 'L';
            if (pasv_fd < 0 && active_port < 0) { send_str(fd, "425 use PASV or PORT first\r\n"); continue; }
            if (cmd[0] == 'R' && !f) { send_str(fd, "550 no such file\r\n"); continue; }
            send_str(fd, "150 opening data connection\r\n");
            int dfd;
            if (pasv_fd >= 0) { dfd = accept_lo(pasv_fd); close(pasv_fd); pasv_fd = -1; }
            else { dfd = connect_try(active_port); active_port = -1; }
            if (dfd < 0) { send_str(fd, "425 cannot open data connection\r\n"); continue; }
            s->data_conns++;
            if (is_stor) {
                unsigned char *buf = malloc(20000);
                size_t n = 0;
                ssize_t r;
                while (n < 20000 && (r = recv(dfd, buf + n, 20000 - n, 0)) > 0) n += (size_t)r;
                vf_put(s, path, buf, n);
                free(buf);
            } else if (is_list) {
                const char *prefix = !strcmp(cwd, "/") ? "" : cwd + 1;
                size_t pl = strlen(prefix);
                char names[MAXF][48];
                int nn = 0;
                for (int i = 0; i < MAXF; i++) {
                    if (!s->fs[i].used) continue;
                    const char *p = s->fs[i].path;
                    if (pl) { if (strncmp(p, prefix, pl) || p[pl] != '/') continue; p += pl + 1; }
                    const char *slash = strchr(p, '/');
                    if (slash) snprintf(names[nn], sizeof names[0], "d %.*s", (int)(slash - p), p);
                    else snprintf(names[nn], sizeof names[0], "- %s %zu", p, s->fs[i].len);
                    int dup = 0;
                    for (int k = 0; k < nn; k++) dup |= !strcmp(names[k], names[nn]);
                    if (!dup) nn++;
                }
                for (int a = 0; a < nn; a++)
                    for (int b2 = a + 1; b2 < nn; b2++)
                        if (strcmp(names[a] + 2, names[b2] + 2) > 0) { char t[48]; strcpy(t, names[a]); strcpy(names[a], names[b2]); strcpy(names[b2], t); }
                for (int a = 0; a < nn; a++) sendf(dfd, "%s\r\n", names[a]);
            } else
                send_all(dfd, f->data, f->len);
            close(dfd);
            send_str(fd, "226 transfer complete\r\n");
        } else if (!strcmp(cmd, "SIZE")) {
            char path[64];
            full_path(cwd, arg1, path, sizeof path);
            VFile *f = vf_find(s, path);
            if (f) sendf(fd, "213 %zu\r\n", f->len); else send_str(fd, "550 no such file\r\n");
        } else if (!strcmp(cmd, "DELE")) {
            char path[64];
            full_path(cwd, arg1, path, sizeof path);
            VFile *f = vf_find(s, path);
            if (f) { free(f->data); f->data = NULL; f->used = 0; send_str(fd, "250 deleted\r\n"); }
            else send_str(fd, "550 no such file\r\n");
        } else
            send_str(fd, "502 command not implemented\r\n");
    }
    if (pasv_fd >= 0) close(pasv_fd);
    close(fd);
    return NULL;
}

static int ctl(Conn *c, const char *cmd, char *reply, size_t cap) {
    CHECK(sendf(c->fd, "%s\r\n", cmd) == 0);
    CHECK(conn_readline(c, reply, cap) >= 0);
    /* addresses and ports are masked so the output is stable */
    const char *shown_cmd = strncmp(cmd, "PORT", 4) == 0 ? "PORT <addr>" : cmd;
    const char *shown_reply = strncmp(reply, "227", 3) == 0 ? "227 Entering Passive Mode (<addr>)" : reply;
    printf("%-22s %s\n", shown_cmd, shown_reply);
    return atoi(reply);
}

static int open_pasv(Conn *c) {
    char r[200];
    CHECK(ctl(c, "PASV", r, sizeof r) == 227);
    int a, b, cc, d, p1, p2;
    CHECK(sscanf(strchr(r, '(') + 1, "%d,%d,%d,%d,%d,%d", &a, &b, &cc, &d, &p1, &p2) == 6);
    return connect_lo(p1 * 256 + p2);
}

static size_t slurp(int dfd, unsigned char *buf, size_t cap) {
    size_t n = 0;
    ssize_t r;
    while (n < cap && (r = recv(dfd, buf + n, cap - n, 0)) > 0) n += (size_t)r;
    return n;
}

int main(void) {
    net_init();
    Server *s = calloc(1, sizeof *s);
    if (!s) die("oom");
    vf_put(s, "readme.txt", (const unsigned char *)"Welcome to iron ftp.\n", 21);
    unsigned char blob[3000];
    uint64_t rs = 8;
    for (size_t i = 0; i < sizeof blob; i++) blob[i] = (unsigned char)rng32(&rs);
    vf_put(s, "pub/data.bin", blob, sizeof blob);
    vf_put(s, "pub/notes.txt", (const unsigned char *)"notes\n", 6);
    int port;
    s->lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, s)) die("thread");
    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);
    char r[200];
    CHECK(conn_readline(&c, r, sizeof r) > 0);
    printf("%-22s %s\n", "(connect)", r);
    CHECK(ctl(&c, "PWD", r, sizeof r) == 530);
    CHECK(ctl(&c, "USER anon", r, sizeof r) == 331);
    CHECK(ctl(&c, "PASS", r, sizeof r) == 530);
    CHECK(ctl(&c, "USER anon", r, sizeof r) == 331);
    CHECK(ctl(&c, "PASS guest@", r, sizeof r) == 230);
    CHECK(ctl(&c, "SYST", r, sizeof r) == 215);
    CHECK(ctl(&c, "TYPE I", r, sizeof r) == 200);
    CHECK(ctl(&c, "LIST", r, sizeof r) == 425);

    /* passive LIST of root */
    int dfd = open_pasv(&c);
    CHECK(ctl(&c, "LIST", r, sizeof r) == 150);
    unsigned char buf[8192];
    size_t n = slurp(dfd, buf, sizeof buf - 1);
    close(dfd);
    buf[n] = 0;
    CHECK(conn_readline(&c, r, sizeof r) > 0);
    printf("  data: %zu bytes, reply %s\n%s", n, r, (char *)buf);

    CHECK(ctl(&c, "CWD /pub", r, sizeof r) == 250);
    CHECK(ctl(&c, "CWD /nowhere", r, sizeof r) == 550);
    CHECK(ctl(&c, "PWD", r, sizeof r) == 257);
    dfd = open_pasv(&c);
    CHECK(ctl(&c, "LIST", r, sizeof r) == 150);
    n = slurp(dfd, buf, sizeof buf - 1);
    close(dfd);
    buf[n] = 0;
    CHECK(conn_readline(&c, r, sizeof r) > 0);
    printf("  data: %zu bytes, reply %s\n%s", n, r, (char *)buf);

    /* passive RETR of binary data */
    CHECK(ctl(&c, "SIZE data.bin", r, sizeof r) == 213);
    dfd = open_pasv(&c);
    CHECK(ctl(&c, "RETR data.bin", r, sizeof r) == 150);
    n = slurp(dfd, buf, sizeof buf);
    close(dfd);
    CHECK(conn_readline(&c, r, sizeof r) > 0);
    printf("  retrieved %zu bytes, reply %s, identical=%d\n", n, r, n == sizeof blob && !memcmp(buf, blob, n));
    CHECK(n == sizeof blob && !memcmp(buf, blob, n));
    dfd = open_pasv(&c);
    close(dfd);
    CHECK(ctl(&c, "RETR missing.bin", r, sizeof r) == 550);

    /* active-mode STOR: the client listens, the server connects */
    int dport;
    int dl = listen_lo(&dport);
    char pc[64];
    snprintf(pc, sizeof pc, "PORT 127,0,0,1,%d,%d", dport >> 8, dport & 255);
    CHECK(ctl(&c, pc, r, sizeof r) == 200);
    CHECK(ctl(&c, "STOR upload.txt", r, sizeof r) == 150);
    dfd = accept_lo(dl);
    CHECK(dfd >= 0);
    const char *up = "uploaded through an active data channel\n";
    CHECK(send_str(dfd, up) == 0);
    close(dfd);
    close(dl);
    CHECK(conn_readline(&c, r, sizeof r) > 0);
    printf("  upload reply %s\n", r);
    CHECK(ctl(&c, "SIZE upload.txt", r, sizeof r) == 213);
    CHECK(ctl(&c, "DELE notes.txt", r, sizeof r) == 250);
    CHECK(ctl(&c, "DELE notes.txt", r, sizeof r) == 550);
    CHECK(ctl(&c, "MKD x", r, sizeof r) == 502);
    CHECK(ctl(&c, "QUIT", r, sizeof r) == 221);
    close(fd);
    pthread_join(th, NULL);
    close(s->lfd);
    printf("data connections: %d\n", s->data_conns);
    for (int i = 0; i < MAXF; i++) free(s->fs[i].data);
    free(s);
    return 0;
}
