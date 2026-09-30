/*
 * title: Chat relay with server-assigned sequence numbers
 * topic: networking
 * covers: poll-based multi-client server, broadcast, global ordering, nick registry, private messages, consistency check
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


#define MAXC 4

typedef struct {
    int lfd;
    int seq;
    int served;
} Server;

typedef struct {
    Conn c;
    int live;
    char nick[16];
} Peer;

static void bcast(Peer *p, int n, const char *line) {
    for (int i = 0; i < n; i++)
        if (p[i].live && p[i].nick[0]) sendf(p[i].c.fd, "%s\n", line);
}

static void *server_main(void *arg) {
    Server *s = arg;
    Peer *peers = calloc(MAXC, sizeof *peers);
    int np = 0, closed = 0;
    while (closed < 4) {
        struct pollfd pf[MAXC + 1];
        int map[MAXC + 1], k = 0;
        pf[k].fd = s->lfd; pf[k].events = POLLIN; pf[k].revents = 0; map[k++] = -1;
        for (int i = 0; i < np; i++)
            if (peers[i].live) { pf[k].fd = peers[i].c.fd; pf[k].events = POLLIN; pf[k].revents = 0; map[k++] = i; }
        if (poll(pf, (nfds_t)k, 5000) <= 0) break;
        for (int j = 0; j < k; j++) {
            if (!(pf[j].revents & (POLLIN | POLLHUP))) continue;
            if (map[j] < 0) {
                int fd = accept_lo(s->lfd);
                if (fd < 0 || np == MAXC) { if (fd >= 0) close(fd); continue; }
                conn_init(&peers[np].c, fd);
                peers[np].live = 1;
                np++;
                continue;
            }
            Peer *me = &peers[map[j]];
            do {
                char line[200];
                if (conn_readline(&me->c, line, sizeof line) < 0) {
                    if (me->nick[0]) {
                        char m[64];
                        snprintf(m, sizeof m, "#%d * %s left", ++s->seq, me->nick);
                        me->nick[0] = 0;
                        bcast(peers, np, m);
                    }
                    me->live = 0;
                    close(me->c.fd);
                    closed++;
                    break;
                }
                s->served++;
                char cmd[8] = "", arg1[16] = "";
                sscanf(line, "%7s %15s", cmd, arg1);
                char out[256];
                if (!strcmp(cmd, "NICK")) {
                    int taken = 0;
                    for (int i = 0; i < np; i++)
                        if (peers[i].live && !strcmp(peers[i].nick, arg1)) taken = 1;
                    if (taken || me->nick[0]) { send_str(me->c.fd, "ERR nick unavailable\n"); continue; }
                    snprintf(me->nick, sizeof me->nick, "%s", arg1);
                    sendf(me->c.fd, "WELCOME %s\n", arg1);
                    snprintf(out, sizeof out, "#%d * %s joined", ++s->seq, arg1);
                    bcast(peers, np, out);
                } else if (!me->nick[0]) {
                    send_str(me->c.fd, "ERR register first\n");
                } else if (!strcmp(cmd, "SAY")) {
                    snprintf(out, sizeof out, "#%d %s: %s", ++s->seq, me->nick, line + 4);
                    bcast(peers, np, out);
                } else if (!strcmp(cmd, "WHO")) {
                    char names[MAXC][16];
                    int nn = 0;
                    for (int i = 0; i < np; i++)
                        if (peers[i].live && peers[i].nick[0]) snprintf(names[nn++], 16, "%s", peers[i].nick);
                    for (int a = 0; a < nn; a++)
                        for (int b = a + 1; b < nn; b++)
                            if (strcmp(names[a], names[b]) > 0) { char t[16]; strcpy(t, names[a]); strcpy(names[a], names[b]); strcpy(names[b], t); }
                    int o = snprintf(out, sizeof out, "USERS");
                    for (int a = 0; a < nn; a++) o += snprintf(out + o, sizeof out - (size_t)o, " %s", names[a]);
                    sendf(me->c.fd, "%s\n", out);
                } else if (!strcmp(cmd, "PM")) {
                    Peer *to = NULL;
                    for (int i = 0; i < np; i++)
                        if (peers[i].live && !strcmp(peers[i].nick, arg1)) to = &peers[i];
                    if (!to) { send_str(me->c.fd, "ERR no such nick\n"); continue; }
                    const char *txt = strchr(line + 3, ' ');
                    snprintf(out, sizeof out, "#%d (pm) %s -> %s:%s", ++s->seq, me->nick, to->nick, txt ? txt : "");
                    sendf(me->c.fd, "%s\n", out);
                    if (to != me) sendf(to->c.fd, "%s\n", out);
                } else if (!strcmp(cmd, "QUIT")) {
                    snprintf(out, sizeof out, "#%d * %s left", ++s->seq, me->nick);
                    char nick[16];
                    snprintf(nick, sizeof nick, "%s", me->nick);
                    bcast(peers, np, out);
                    me->nick[0] = 0;
                    me->live = 0;
                    shutdown(me->c.fd, SHUT_RDWR);
                    close(me->c.fd);
                    closed++;
                    break;
                } else
                    send_str(me->c.fd, "ERR unknown command\n");
            } while (me->live && me->c.pos < me->c.len);
        }
    }
    free(peers);
    return NULL;
}

typedef struct {
    Conn c;
    char name[8];
    char log[24][96];
    int n;
} Cli;

static const char *rd(Cli *c) {
    if (c->n >= 24) die("log full");
    CHECK(conn_readline(&c->c, c->log[c->n], sizeof c->log[0]) >= 0);
    return c->log[c->n++];
}

static void say(Cli *c, const char *text) {
    CHECK(sendf(c->c.fd, "%s\n", text) == 0);
}

static void show(Cli *c, const char *what) {
    printf("%s <- %s\n", c->name, what);
}

int main(void) {
    net_init();
    Server s = {0};
    int port;
    s.lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, &s)) die("thread");
    Cli cl[4];
    memset(cl, 0, sizeof cl);
    const char *names[4] = {"ann", "bob", "cy", "dee"};
    for (int i = 0; i < 4; i++) {
        snprintf(cl[i].name, sizeof cl[i].name, "%s", names[i]);
        conn_init(&cl[i].c, connect_lo(port));
    }
    /* registration, one client at a time so join events have a known order */
    for (int i = 0; i < 3; i++) {
        char l[32];
        snprintf(l, sizeof l, "NICK %s", names[i]);
        say(&cl[i], l);
        show(&cl[i], rd(&cl[i]));
        for (int j = 0; j <= i; j++) show(&cl[j], rd(&cl[j]));
    }
    say(&cl[3], "NICK ann");
    show(&cl[3], rd(&cl[3]));
    say(&cl[3], "SAY not registered");
    show(&cl[3], rd(&cl[3]));

    /* one at a time */
    say(&cl[0], "SAY hello everyone");
    for (int j = 0; j < 3; j++) show(&cl[j], rd(&cl[j]));

    /* burst: three lines in a single write keep their order, and the server drains its buffer */
    CHECK(send_str(cl[1].c.fd, "SAY b1\nSAY b2\nSAY b3\n") == 0);
    for (int j = 0; j < 3; j++)
        for (int k = 0; k < 3; k++) rd(&cl[j]);
    for (int k = 0; k < 3; k++) show(&cl[0], cl[0].log[cl[0].n - 3 + k]);
    say(&cl[2], "SAY from cy");
    for (int j = 0; j < 3; j++) show(&cl[j], rd(&cl[j]));

    say(&cl[2], "WHO");
    show(&cl[2], rd(&cl[2]));
    say(&cl[0], "PM cy psst");
    show(&cl[0], rd(&cl[0]));
    show(&cl[2], rd(&cl[2]));
    say(&cl[0], "PM nobody hi");
    show(&cl[0], rd(&cl[0]));
    say(&cl[1], "DANCE");
    show(&cl[1], rd(&cl[1]));
    say(&cl[1], "QUIT");
    for (int j = 0; j < 3; j += 2) show(&cl[j], rd(&cl[j]));
    say(&cl[0], "SAY bye");
    show(&cl[0], rd(&cl[0]));
    show(&cl[2], rd(&cl[2]));

    /* every client saw identical text for every sequence number it received */
    int checked = 0;
    for (int a = 0; a < 3; a++)
        for (int b = a + 1; b < 3; b++)
            for (int i = 0; i < cl[a].n; i++)
                for (int j = 0; j < cl[b].n; j++)
                    if (cl[a].log[i][0] == '#' && cl[b].log[j][0] == '#' && atoi(cl[a].log[i] + 1) == atoi(cl[b].log[j] + 1)) {
                        CHECK(strcmp(cl[a].log[i], cl[b].log[j]) == 0);
                        checked++;
                    }
    printf("cross-client sequence checks: %d\n", checked);
    /* sequence numbers strictly increase in each client's log */
    for (int a = 0; a < 3; a++) {
        int last = 0;
        for (int i = 0; i < cl[a].n; i++)
            if (cl[a].log[i][0] == '#') {
                int q = atoi(cl[a].log[i] + 1);
                CHECK(q > last);
                last = q;
            }
    }
    for (int i = 0; i < 4; i++) close(cl[i].c.fd);
    pthread_join(th, NULL);
    close(s.lfd);
    printf("server assigned %d sequence numbers, handled %d lines\n", s.seq, s.served);
    return 0;
}
