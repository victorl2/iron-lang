/*
 * title: IRC-style server with channels and numeric replies
 * topic: networking
 * covers: NICK/USER registration, JOIN/PART/PRIVMSG/TOPIC/NAMES/KICK/QUIT, channel membership, numerics, prefixes
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


#define MAXC 3
#define MAXCH 3

typedef struct {
    Conn c;
    int live, registered, have_nick, have_user;
    char nick[16];
} User;

typedef struct {
    char name[16], topic[48];
    int member[MAXC]; /* 0 none, 1 member, 2 operator */
    int used;
} Channel;

typedef struct {
    int lfd;
    User u[MAXC];
    Channel ch[MAXCH];
    int nu, closed, lines;
} Server;

static void out(User *u, const char *fmt, ...) {
    char b[300], line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "%s\r\n", b);
    send_str(u->c.fd, line);
}

static Channel *chan_find(Server *s, const char *n) {
    for (int i = 0; i < MAXCH; i++)
        if (s->ch[i].used && !strcmp(s->ch[i].name, n)) return &s->ch[i];
    return NULL;
}

static int user_idx(Server *s, const char *nick) {
    for (int i = 0; i < s->nu; i++)
        if (s->u[i].live && s->u[i].registered && !strcasecmp(s->u[i].nick, nick)) return i;
    return -1;
}

static void to_channel(Server *s, Channel *c, int except, const char *line) {
    for (int i = 0; i < MAXC; i++)
        if (c->member[i] && i != except && s->u[i].live) out(&s->u[i], "%s", line);
}

static void drop_user(Server *s, int i, const char *reason) {
    User *u = &s->u[i];
    if (u->registered) {
        char l[128];
        snprintf(l, sizeof l, ":%s QUIT :%s", u->nick, reason);
        for (int k = 0; k < MAXC; k++) {
            if (k == i || !s->u[k].live) continue;
            int shared = 0;
            for (int h = 0; h < MAXCH; h++) shared |= s->ch[h].used && s->ch[h].member[i] && s->ch[h].member[k];
            if (shared) out(&s->u[k], "%s", l);
        }
    }
    for (int h = 0; h < MAXCH; h++) s->ch[h].member[i] = 0;
    u->live = 0;
    close(u->c.fd);
    s->closed++;
}

static void names_reply(Server *s, User *u, int ui, Channel *c) {
    char names[MAXC][20];
    int n = 0;
    for (int i = 0; i < MAXC; i++)
        if (c->member[i]) snprintf(names[n++], 20, "%s%s", c->member[i] == 2 ? "@" : "", s->u[i].nick);
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++) {
            const char *x = names[a] + (names[a][0] == '@'), *y = names[b] + (names[b][0] == '@');
            if (strcmp(x, y) > 0) { char t[20]; strcpy(t, names[a]); strcpy(names[a], names[b]); strcpy(names[b], t); }
        }
    char l[200];
    int o = snprintf(l, sizeof l, ":irc.iron 353 %s = %s :", u->nick, c->name);
    for (int a = 0; a < n; a++) o += snprintf(l + o, sizeof l - (size_t)o, "%s%s", a ? " " : "", names[a]);
    out(u, "%s", l);
    out(u, ":irc.iron 366 %s %s :End of /NAMES list", u->nick, c->name);
    (void)ui;
}

static void handle(Server *s, int ui, char *line) {
    User *u = &s->u[ui];
    char *cmd = line, *rest = strchr(line, ' ');
    if (rest) *rest++ = 0; else rest = line + strlen(line);
    for (char *p = cmd; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    char l[300];
    if (!strcmp(cmd, "NICK")) {
        if (user_idx(s, rest) >= 0) { out(u, ":irc.iron 433 * %s :Nickname is already in use", rest); return; }
        snprintf(u->nick, sizeof u->nick, "%s", rest);
        u->have_nick = 1;
    } else if (!strcmp(cmd, "USER")) {
        u->have_user = 1;
    } else if (!strcmp(cmd, "PING")) {
        out(u, ":irc.iron PONG irc.iron :%s", rest);
        return;
    } else if (!u->registered && strcmp(cmd, "QUIT")) {
        /* fallthrough to registration check below for non-registration commands */
        if (strcmp(cmd, "NICK") && strcmp(cmd, "USER")) { out(u, ":irc.iron 451 * :You have not registered"); return; }
    }
    if (!u->registered && u->have_nick && u->have_user) {
        u->registered = 1;
        out(u, ":irc.iron 001 %s :Welcome to the Iron IRC network, %s", u->nick, u->nick);
        return;
    }
    if (!u->registered) return;
    if (!strcmp(cmd, "JOIN")) {
        Channel *c = chan_find(s, rest);
        if (!c) {
            for (int i = 0; i < MAXCH && !c; i++) if (!s->ch[i].used) c = &s->ch[i];
            if (!c) return;
            memset(c, 0, sizeof *c);
            c->used = 1;
            snprintf(c->name, sizeof c->name, "%s", rest);
            c->member[ui] = 2; /* first joiner is operator */
        } else
            c->member[ui] = 1;
        snprintf(l, sizeof l, ":%s JOIN %s", u->nick, c->name);
        to_channel(s, c, -1, l);
        if (c->topic[0]) out(u, ":irc.iron 332 %s %s :%s", u->nick, c->name, c->topic);
        names_reply(s, u, ui, c);
    } else if (!strcmp(cmd, "PART")) {
        Channel *c = chan_find(s, rest);
        if (!c || !c->member[ui]) { out(u, ":irc.iron 442 %s %s :You're not on that channel", u->nick, rest); return; }
        snprintf(l, sizeof l, ":%s PART %s", u->nick, c->name);
        to_channel(s, c, -1, l);
        c->member[ui] = 0;
    } else if (!strcmp(cmd, "PRIVMSG") || !strcmp(cmd, "NOTICE")) {
        char *sp = strchr(rest, ' ');
        if (!sp) return;
        *sp++ = 0;
        snprintf(l, sizeof l, ":%s %s %s %s", u->nick, cmd, rest, sp);
        if (rest[0] == '#') {
            Channel *c = chan_find(s, rest);
            if (!c) { out(u, ":irc.iron 403 %s %s :No such channel", u->nick, rest); return; }
            if (!c->member[ui]) { out(u, ":irc.iron 404 %s %s :Cannot send to channel", u->nick, rest); return; }
            to_channel(s, c, ui, l);
        } else {
            int t = user_idx(s, rest);
            if (t < 0) out(u, ":irc.iron 401 %s %s :No such nick", u->nick, rest);
            else out(&s->u[t], "%s", l);
        }
    } else if (!strcmp(cmd, "TOPIC")) {
        char *sp = strchr(rest, ' ');
        if (sp) *sp++ = 0;
        Channel *c = chan_find(s, rest);
        if (!c || !c->member[ui]) { out(u, ":irc.iron 442 %s %s :You're not on that channel", u->nick, rest); return; }
        if (sp && sp[0] == ':') {
            snprintf(c->topic, sizeof c->topic, "%s", sp + 1);
            snprintf(l, sizeof l, ":%s TOPIC %s :%s", u->nick, c->name, c->topic);
            to_channel(s, c, -1, l);
        } else if (c->topic[0]) out(u, ":irc.iron 332 %s %s :%s", u->nick, c->name, c->topic);
        else out(u, ":irc.iron 331 %s %s :No topic is set", u->nick, c->name);
    } else if (!strcmp(cmd, "NAMES")) {
        Channel *c = chan_find(s, rest);
        if (c) names_reply(s, u, ui, c);
    } else if (!strcmp(cmd, "KICK")) {
        char *sp = strchr(rest, ' ');
        if (!sp) return;
        *sp++ = 0;
        Channel *c = chan_find(s, rest);
        int t = user_idx(s, sp);
        if (!c || t < 0 || !c->member[t]) { out(u, ":irc.iron 441 %s %s :They aren't on that channel", u->nick, sp); return; }
        if (c->member[ui] != 2) { out(u, ":irc.iron 482 %s %s :You're not channel operator", u->nick, c->name); return; }
        snprintf(l, sizeof l, ":%s KICK %s %s", u->nick, c->name, sp);
        to_channel(s, c, -1, l);
        c->member[t] = 0;
    } else if (!strcmp(cmd, "QUIT")) {
        drop_user(s, ui, rest[0] == ':' ? rest + 1 : "Quit");
    } else if (strcmp(cmd, "NICK") && strcmp(cmd, "USER")) {
        out(u, ":irc.iron 421 %s %s :Unknown command", u->nick, cmd);
    }
}

static void *server_main(void *arg) {
    Server *s = arg;
    while (s->closed < MAXC) {
        struct pollfd pf[MAXC + 1];
        int map[MAXC + 1], k = 0;
        pf[k].fd = s->lfd; pf[k].events = POLLIN; pf[k].revents = 0; map[k++] = -1;
        for (int i = 0; i < s->nu; i++)
            if (s->u[i].live) { pf[k].fd = s->u[i].c.fd; pf[k].events = POLLIN; pf[k].revents = 0; map[k++] = i; }
        if (poll(pf, (nfds_t)k, 5000) <= 0) break;
        for (int j = 0; j < k; j++) {
            if (!(pf[j].revents & (POLLIN | POLLHUP))) continue;
            if (map[j] < 0) {
                int fd = accept_lo(s->lfd);
                if (fd < 0 || s->nu == MAXC) { if (fd >= 0) close(fd); continue; }
                conn_init(&s->u[s->nu].c, fd);
                s->u[s->nu++].live = 1;
                continue;
            }
            int ui = map[j];
            do {
                char line[300];
                if (conn_readline(&s->u[ui].c, line, sizeof line) < 0) { drop_user(s, ui, "Connection closed"); break; }
                s->lines++;
                handle(s, ui, line);
            } while (s->u[ui].live && s->u[ui].c.pos < s->u[ui].c.len);
        }
    }
    return NULL;
}

static Conn cl[MAXC];
static const char *nm[MAXC] = {"ann", "bob", "cy"};

/* sends a line from client `from`, then reads counts[i] lines from each client */
static void step(int from, const char *line, int n0, int n1, int n2) {
    printf("%s> %s\n", nm[from], line);
    CHECK(sendf(cl[from].fd, "%s\r\n", line) == 0);
    int cnt[MAXC] = {n0, n1, n2};
    for (int i = 0; i < MAXC; i++)
        for (int k = 0; k < cnt[i]; k++) {
            char r[300];
            CHECK(conn_readline(&cl[i], r, sizeof r) >= 0);
            printf("  %s< %s\n", nm[i], r);
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
    for (int i = 0; i < MAXC; i++) conn_init(&cl[i], connect_lo(port));
    step(0, "JOIN #early", 1, 0, 0); /* not registered yet */
    step(0, "NICK ann", 0, 0, 0);
    step(0, "USER ann 0 * :Ann A", 1, 0, 0);
    step(1, "NICK ann", 0, 1, 0);
    step(1, "NICK bob", 0, 0, 0);
    step(1, "USER bob 0 * :Bob B", 0, 1, 0);
    step(2, "NICK cy", 0, 0, 0);
    step(2, "USER cy 0 * :Cy C", 0, 0, 1);
    step(0, "JOIN #iron", 3, 0, 0);
    step(0, "TOPIC #iron :all about iron", 1, 0, 0);
    step(1, "JOIN #iron", 1, 4, 0);
    step(2, "JOIN #iron", 1, 1, 4);
    step(2, "JOIN #other", 0, 0, 3);
    step(1, "PRIVMSG #iron :hello channel", 1, 0, 1);
    step(2, "PRIVMSG #other :talking to myself", 0, 0, 0);
    step(1, "PRIVMSG cy :psst", 0, 0, 1);
    step(1, "PRIVMSG nobody :hi", 0, 1, 0);
    step(1, "PRIVMSG #nochan :hi", 0, 1, 0);
    step(1, "PRIVMSG #other :not a member", 0, 1, 0);
    step(2, "TOPIC #iron", 0, 0, 1);
    step(2, "KICK #iron ann", 0, 0, 1);
    step(0, "KICK #iron cy", 1, 1, 1);
    step(2, "PING token42", 0, 0, 1);
    step(1, "DANCE", 0, 1, 0);
    step(1, "PART #iron", 1, 1, 0);
    step(1, "PART #iron", 0, 1, 0);
    step(1, "NAMES #iron", 0, 2, 0);
    step(2, "QUIT :gone fishing", 0, 0, 0);
    /* ann shares no channel with cy any more (kicked), bob left #iron: nobody hears the QUIT */
    step(0, "QUIT :bye", 0, 0, 0);
    close(cl[0].fd);
    close(cl[1].fd);
    close(cl[2].fd);
    pthread_join(th, NULL);
    close(s->lfd);
    printf("server processed %d lines\n", s->lines);
    free(s);
    return 0;
}
