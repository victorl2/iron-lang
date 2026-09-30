/*
 * title: File transfer with checksum verification and resume
 * topic: networking
 * covers: line-framed control with raw data, crc32, resume from offset, prefix hash validation, dropped connections
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

#include <sys/stat.h>

#define FILE_LEN 30000
static unsigned char content[FILE_LEN];

typedef struct {
    int lfd;
    int gets, conns;
    int done;
} Server;

static void *server_main(void *arg) {
    Server *s = arg;
    while (!s->done) {
        int fd = accept_lo(s->lfd);
        if (fd < 0) break;
        s->conns++;
        Conn c;
        conn_init(&c, fd);
        char line[80];
        while (conn_readline(&c, line, sizeof line) >= 0) {
            long a = 0;
            if (!strcmp(line, "STAT")) {
                sendf(fd, "OK %d %08x\n", FILE_LEN, crc32_buf(content, FILE_LEN));
            } else if (sscanf(line, "HASH %ld", &a) == 1) {
                if (a < 0 || a > FILE_LEN) send_str(fd, "ERR range\n");
                else sendf(fd, "OK %08x\n", crc32_buf(content, (size_t)a));
            } else if (sscanf(line, "GET %ld", &a) == 1) {
                if (a < 0 || a > FILE_LEN) { send_str(fd, "ERR range\n"); continue; }
                size_t n = (size_t)(FILE_LEN - a);
                s->gets++;
                sendf(fd, "OK %zu %08x\n", n, crc32_buf(content + a, n));
                /* fault injection: the first two transfers die part way */
                size_t send_n = n;
                if (s->gets == 1 && n > 7000) send_n = 7000;
                if (s->gets == 2 && n > 9000) send_n = 9000;
                send_all(fd, content + a, send_n);
                if (send_n < n) break; /* drop the connection */
            } else if (!strcmp(line, "DONE")) {
                send_str(fd, "BYE\n");
                s->done = 1;
                break;
            } else
                send_str(fd, "ERR unknown\n");
        }
        close(fd);
    }
    return NULL;
}

static long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long)st.st_size;
}

static uint32_t file_prefix_crc(const char *path, long n) {
    unsigned char *buf = malloc((size_t)n + 1);
    FILE *f = fopen(path, "rb");
    if (!f || fread(buf, 1, (size_t)n, f) != (size_t)n) die("read prefix");
    fclose(f);
    uint32_t c = crc32_buf(buf, (size_t)n);
    free(buf);
    return c;
}

/* one download attempt appending to path; returns 1 when complete */
static int attempt(int port, const char *path, int no) {
    long have = file_size(path);
    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);
    char line[80];
    /* validate the partial file against the server before trusting it */
    if (have > 0) {
        sendf(fd, "HASH %ld\n", have);
        CHECK(conn_readline(&c, line, sizeof line) > 0);
        char want[16];
        snprintf(want, sizeof want, "OK %08x", file_prefix_crc(path, have));
        if (strcmp(line, want) != 0) {
            printf("attempt %d: partial file (%ld bytes) fails prefix check, restarting from 0\n", no, have);
            FILE *t = fopen(path, "wb");
            fclose(t);
            have = 0;
        } else
            printf("attempt %d: partial file (%ld bytes) verified, resuming\n", no, have);
    } else
        printf("attempt %d: starting from 0\n", no);
    sendf(fd, "GET %ld\n", have);
    CHECK(conn_readline(&c, line, sizeof line) > 0);
    long n;
    unsigned crc;
    CHECK(sscanf(line, "OK %ld %x", &n, &crc) == 2);
    FILE *f = fopen(path, "ab");
    CHECK(f != NULL);
    long got = 0;
    unsigned char *whole = malloc((size_t)n + 1);
    while (got < n) {
        int ch = conn_getc(&c);
        if (ch < 0) break;
        whole[got] = (unsigned char)ch;
        got++;
    }
    fwrite(whole, 1, (size_t)got, f);
    fclose(f);
    int complete = got == n;
    if (complete) CHECK(crc32_buf(whole, (size_t)n) == crc);
    printf("attempt %d: received %ld of %ld bytes%s\n", no, got, n, complete ? ", segment checksum ok" : ", connection dropped");
    free(whole);
    close(fd);
    return complete;
}

int main(void) {
    net_init();
    uint64_t rs = 31337;
    for (int i = 0; i < FILE_LEN; i++) content[i] = (unsigned char)rng32(&rs);
    Server s = {0};
    int port;
    s.lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, &s)) die("thread");

    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);
    char line[80];
    sendf(fd, "STAT\n");
    CHECK(conn_readline(&c, line, sizeof line) > 0);
    long size;
    unsigned whole_crc;
    CHECK(sscanf(line, "OK %ld %x", &size, &whole_crc) == 2);
    printf("remote file: %ld bytes, crc %08x\n", size, whole_crc);
    sendf(fd, "HASH 99999\n");
    CHECK(conn_readline(&c, line, sizeof line) > 0);
    printf("HASH beyond end: %s\n", line);
    close(fd);

    unlink("dl.part");
    int no = 1;
    while (!attempt(port, "dl.part", no)) {
        no++;
        CHECK(no < 6);
    }
    CHECK(file_size("dl.part") == size);
    CHECK(file_prefix_crc("dl.part", size) == whole_crc);
    printf("download 1 complete after %d attempts, crc %08x\n", no, file_prefix_crc("dl.part", size));
    unlink("dl.part");

    /* a partial file with one flipped byte must not be resumed */
    FILE *f = fopen("dl2.part", "wb");
    unsigned char bad[5000];
    memcpy(bad, content, sizeof bad);
    bad[1234] ^= 0x40;
    fwrite(bad, 1, sizeof bad, f);
    fclose(f);
    no = 1;
    while (!attempt(port, "dl2.part", no)) no++;
    CHECK(file_prefix_crc("dl2.part", size) == whole_crc);
    printf("download 2 complete, crc %08x\n", file_prefix_crc("dl2.part", size));
    unlink("dl2.part");

    fd = connect_lo(port);
    conn_init(&c, fd);
    sendf(fd, "DONE\n");
    CHECK(conn_readline(&c, line, sizeof line) > 0);
    close(fd);
    pthread_join(th, NULL);
    close(s.lfd);
    printf("server: gets=%d\n", s.gets);
    return 0;
}
