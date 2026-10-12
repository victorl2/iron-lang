/*
 * title: SMTP session with dot-stuffing in a forked server
 * topic: networking
 * covers: SMTP command state machine, multi-line replies, DATA dot-stuffing, RCPT policy, spool file, fork
 * deps: libc, posix, sockets
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


#define SPOOL "smtp_spool.txt"

/* ---------- server (child process) ---------- */
enum State { S_INIT, S_GREETED, S_MAIL, S_RCPT };

static int server_child(int lfd) {
    int fd = accept_lo(lfd);
    if (fd < 0) return 90;
    FILE *sp = fopen(SPOOL, "w");
    if (!sp) return 91;
    Conn c;
    conn_init(&c, fd);
    send_str(fd, "220 iron.test ESMTP ready\r\n");
    enum State st = S_INIT;
    char from[64] = "", rcpts[4][64];
    int nr = 0, mails = 0;
    char line[1100];
    while (conn_readline(&c, line, sizeof line) >= 0) {
        if (!strncasecmp(line, "EHLO ", 5)) {
            sendf(fd, "250-iron.test greets %s\r\n250-8BITMIME\r\n250 SIZE 10000\r\n", line + 5);
            st = S_GREETED;
        } else if (!strncasecmp(line, "HELO ", 5)) {
            sendf(fd, "250 iron.test greets %s\r\n", line + 5);
            st = S_GREETED;
        } else if (!strncasecmp(line, "MAIL FROM:", 10)) {
            if (st != S_GREETED) { send_str(fd, "503 bad sequence of commands\r\n"); continue; }
            sscanf(line + 10, "<%63[^>]>", from);
            nr = 0;
            st = S_MAIL;
            send_str(fd, "250 sender ok\r\n");
        } else if (!strncasecmp(line, "RCPT TO:", 8)) {
            if (st != S_MAIL && st != S_RCPT) { send_str(fd, "503 need MAIL first\r\n"); continue; }
            char to[64] = "";
            sscanf(line + 8, "<%63[^>]>", to);
            const char *at = strchr(to, '@');
            if (!at || strcmp(at, "@iron.test") != 0) { send_str(fd, "550 relay denied\r\n"); continue; }
            if (nr == 4) { send_str(fd, "452 too many recipients\r\n"); continue; }
            snprintf(rcpts[nr++], 64, "%s", to);
            st = S_RCPT;
            send_str(fd, "250 recipient ok\r\n");
        } else if (!strncasecmp(line, "DATA", 4)) {
            if (st != S_RCPT) { send_str(fd, "503 need RCPT first\r\n"); continue; }
            send_str(fd, "354 end data with <CRLF>.<CRLF>\r\n");
            fprintf(sp, "FROM %s\n", from);
            for (int i = 0; i < nr; i++) fprintf(sp, "TO %s\n", rcpts[i]);
            fprintf(sp, "BEGIN\n");
            int lines = 0;
            while (conn_readline(&c, line, sizeof line) >= 0) {
                if (strcmp(line, ".") == 0) break;
                const char *t = line[0] == '.' ? line + 1 : line; /* remove stuffing */
                fprintf(sp, "%s\n", t);
                lines++;
            }
            fprintf(sp, "END\n");
            mails++;
            sendf(fd, "250 queued %d lines as message %d\r\n", lines, mails);
            st = S_GREETED;
        } else if (!strncasecmp(line, "RSET", 4)) {
            st = S_GREETED;
            send_str(fd, "250 reset\r\n");
        } else if (!strncasecmp(line, "NOOP", 4)) {
            send_str(fd, "250 ok\r\n");
        } else if (!strncasecmp(line, "QUIT", 4)) {
            send_str(fd, "221 bye\r\n");
            break;
        } else {
            send_str(fd, "500 unrecognized command\r\n");
        }
    }
    fclose(sp);
    close(fd);
    return mails;
}

/* ---------- client ---------- */
static int reply(Conn *c, char *last) {
    char line[256];
    for (;;) {
        if (conn_readline(c, line, sizeof line) < 0) die("eof in reply");
        if (line[3] == ' ' || line[3] == 0) {
            snprintf(last, 256, "%s", line);
            return atoi(line);
        }
    }
}

static int cmd(Conn *c, const char *text) {
    char last[256];
    CHECK(sendf(c->fd, "%s\r\n", text) == 0);
    int code = reply(c, last);
    printf("C: %-32s S: %s\n", text, last);
    return code;
}

int main(void) {
    net_init();
    int port;
    int lfd = listen_lo(&port);
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        int rc = server_child(lfd);
        _exit(rc);
    }
    close(lfd);
    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);
    char last[256];
    CHECK(reply(&c, last) == 220);
    printf("S: %s\n", last);

    CHECK(cmd(&c, "MAIL FROM:<early@x.org>") == 503);
    CHECK(cmd(&c, "EHLO client.example") == 250);
    CHECK(cmd(&c, "MAIL FROM:<alice@example.org>") == 250);
    CHECK(cmd(&c, "RCPT TO:<bob@iron.test>") == 250);
    CHECK(cmd(&c, "RCPT TO:<eve@elsewhere.net>") == 550);
    CHECK(cmd(&c, "RCPT TO:<carol@iron.test>") == 250);
    CHECK(cmd(&c, "DATA") == 354);

    const char *msg[] = {"Subject: dots", "", "line one", ".hidden line", "..two dots", ".", "", "last line."};
    int nmsg = 8;
    for (int i = 0; i < nmsg; i++) {
        /* dot-stuff: any line starting with '.' gets another '.' */
        CHECK(sendf(fd, "%s%s\r\n", msg[i][0] == '.' ? "." : "", msg[i]) == 0);
    }
    CHECK(send_str(fd, ".\r\n") == 0);
    CHECK(reply(&c, last) == 250);
    printf("end of data: %s\n", last);

    CHECK(cmd(&c, "DATA") == 503);
    CHECK(cmd(&c, "MAIL FROM:<dave@example.org>") == 250);
    CHECK(cmd(&c, "RSET") == 250);
    CHECK(cmd(&c, "RCPT TO:<bob@iron.test>") == 503);
    CHECK(cmd(&c, "MAIL FROM:<dave@example.org>") == 250);
    CHECK(cmd(&c, "RCPT TO:<bob@iron.test>") == 250);
    CHECK(cmd(&c, "DATA") == 354);
    CHECK(send_str(fd, "single line\r\n.\r\n") == 0);
    CHECK(reply(&c, last) == 250);
    printf("end of data: %s\n", last);
    CHECK(cmd(&c, "VRFY bob") == 500);
    CHECK(cmd(&c, "NOOP") == 250);
    CHECK(cmd(&c, "QUIT") == 221);
    close(fd);

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) die("waitpid");
    CHECK(WIFEXITED(status));
    int code = WEXITSTATUS(status);
    printf("server child exit: mails=%d\n", code);
    CHECK(code == 2);

    /* verify the spool file: unstuffing restored the original message */
    FILE *f = fopen(SPOOL, "r");
    CHECK(f != NULL);
    char line[256];
    int in_body = 0, bi = 0, ok = 1, msgno = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "FROM ", 5) || !strncmp(line, "TO ", 3)) printf("spool %s\n", line);
        else if (!strcmp(line, "BEGIN")) { in_body = 1; bi = 0; msgno++; }
        else if (!strcmp(line, "END")) { in_body = 0; if (msgno == 1) CHECK(bi == nmsg); }
        else if (in_body && msgno == 1) { if (bi >= nmsg || strcmp(line, msg[bi]) != 0) ok = 0; bi++; }
    }
    fclose(f);
    unlink(SPOOL);
    printf("first message round-trips through dot-stuffing: %s\n", ok ? "yes" : "no");
    CHECK(ok);
    return 0;
}
