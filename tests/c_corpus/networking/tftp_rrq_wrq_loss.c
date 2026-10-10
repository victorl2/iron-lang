/*
 * title: TFTP read and write over UDP with simulated loss
 * topic: networking
 * covers: RRQ/WRQ, DATA/ACK block numbers, transfer ids, timeouts, retransmission, duplicate handling, error packets
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


enum { OP_RRQ = 1, OP_WRQ, OP_DATA, OP_ACK, OP_ERROR };
#define BLK 512
#define TMO_MS 80

/* deterministic loss: the first transmission of chosen block numbers is swallowed */
typedef struct {
    unsigned mask;   /* bit b set: drop first send of block b */
    unsigned done;
    int drops;
    int retx;
} Loss;

static void tx(int fd, int port, const unsigned char *p, size_t n, Loss *l, int blk) {
    if (blk >= 0 && blk < 32 && (l->mask >> blk & 1u) && !(l->done >> blk & 1u)) {
        l->done |= 1u << blk;
        l->drops++;
        return;
    }
    udp_send(fd, port, p, n);
}

static size_t mk_data(unsigned char *p, unsigned blk, const unsigned char *d, size_t n) {
    p[0] = 0; p[1] = OP_DATA; p[2] = (unsigned char)(blk >> 8); p[3] = (unsigned char)blk;
    memcpy(p + 4, d, n);
    return n + 4;
}
static size_t mk_ack(unsigned char *p, unsigned blk) {
    p[0] = 0; p[1] = OP_ACK; p[2] = (unsigned char)(blk >> 8); p[3] = (unsigned char)blk;
    return 4;
}
static size_t mk_req(unsigned char *p, int op, const char *name) {
    p[0] = 0;
    p[1] = (unsigned char)op;
    size_t l = strlen(name);
    memcpy(p + 2, name, l + 1);
    memcpy(p + 3 + l, "octet", 6);
    return l + 9;
}
static size_t mk_err(unsigned char *p, unsigned code, const char *msg) {
    p[0] = 0; p[1] = OP_ERROR; p[2] = 0; p[3] = (unsigned char)code;
    size_t l = strlen(msg);
    memcpy(p + 4, msg, l + 1);
    return l + 5;
}

/* sends data as blocks 1.. and waits for each ACK; returns block count or -1 */
static int send_blocks(int fd, int peer, const unsigned char *d, size_t len, Loss *l) {
    unsigned blk = 1;
    size_t off = 0;
    for (;;) {
        size_t n = len - off < BLK ? len - off : BLK;
        unsigned char pkt[BLK + 4];
        size_t pl = mk_data(pkt, blk, d + off, n);
        int tries = 0;
        tx(fd, peer, pkt, pl, l, (int)blk);
        for (;;) {
            unsigned char in[600];
            int from;
            int r = udp_recv(fd, in, sizeof in, &from);
            if (r < 0) {
                if (++tries > 10) return -1;
                l->retx++;
                tx(fd, peer, pkt, pl, l, (int)blk); /* only a timeout triggers a resend */
                continue;
            }
            if (from != peer || r < 4 || in[1] != OP_ACK) continue;
            unsigned ab = (unsigned)(in[2] << 8 | in[3]);
            if (ab == blk) break; /* stale ACKs are ignored, never trigger resends */
        }
        off += n;
        if (n < BLK) return (int)blk;
        blk++;
    }
}

/* receives blocks; first_ack_blk0: send ACK 0 first (WRQ). returns length or -1; *err set on ERROR */
static long recv_blocks(int fd, int *peer, unsigned char *out, size_t cap, Loss *l, int send_ack0, int *nblocks) {
    unsigned expect = 1;
    size_t len = 0;
    unsigned char ack[4];
    size_t al = mk_ack(ack, 0);
    if (send_ack0) tx(fd, *peer, ack, al, l, 0);
    int tries = 0;
    for (;;) {
        unsigned char in[600];
        int from;
        int r = udp_recv(fd, in, sizeof in, &from);
        if (r < 0) {
            if (++tries > 10) return -1;
            l->retx++;
            if (expect > 1 || send_ack0) tx(fd, *peer, ack, al, l, -1); /* resend last ACK unconditionally */
            continue;
        }
        if (*peer == 0) *peer = from;
        if (from != *peer || r < 4) continue;
        if (in[1] == OP_ERROR) return -2;
        if (in[1] != OP_DATA) continue;
        unsigned b = (unsigned)(in[2] << 8 | in[3]);
        if (b == expect) {
            size_t n = (size_t)r - 4;
            if (len + n > cap) return -1;
            memcpy(out + len, in + 4, n);
            len += n;
            al = mk_ack(ack, b);
            tx(fd, *peer, ack, al, l, (int)b);
            expect++;
            tries = 0;
            if (n < BLK) {
                if (nblocks) *nblocks = (int)b;
                return (long)len;
            }
        } else if (b == expect - 1) {
            tx(fd, *peer, ack, al, l, -1); /* duplicate DATA: re-ACK */
        }
    }
}

typedef struct {
    int fd;
    unsigned char boot[2600];
    unsigned char up[4096];
    long up_len;
    Loss rrq_loss, wrq_loss;
    int handled;
    int errors;
} Server;

static void *server_main(void *arg) {
    Server *s = arg;
    for (int i = 0; i < 3; i++) {
        unsigned char req[600];
        int from;
        int n = udp_recv(s->fd, req, sizeof req, &from);
        if (n < 4) break;
        int tport;
        int tfd = udp_lo(&tport, TMO_MS);
        char fname[64];
        snprintf(fname, sizeof fname, "%s", (char *)req + 2);
        if (req[1] == OP_RRQ) {
            if (strcmp(fname, "boot.img") != 0) {
                unsigned char e[64];
                size_t el = mk_err(e, 1, "File not found");
                udp_send(tfd, from, e, el);
                s->errors++;
            } else
                send_blocks(tfd, from, s->boot, sizeof s->boot, &s->rrq_loss);
        } else if (req[1] == OP_WRQ) {
            int peer = from, nb = 0;
            s->up_len = recv_blocks(tfd, &peer, s->up, sizeof s->up, &s->wrq_loss, 1, &nb);
        }
        s->handled++;
        close(tfd);
    }
    return NULL;
}

int main(void) {
    net_init();
    Server *s = calloc(1, sizeof *s);
    if (!s) die("oom");
    uint64_t rs = 69;
    for (size_t i = 0; i < sizeof s->boot; i++) s->boot[i] = (unsigned char)rng32(&rs);
    s->rrq_loss.mask = (1u << 2) | (1u << 4);   /* server drops first DATA 2 and 4 */
    s->wrq_loss.mask = (1u << 0) | (1u << 2);   /* server drops first ACK 0 and 2 */
    int sport, cport;
    s->fd = udp_lo(&sport, 3000);
    int cfd = udp_lo(&cport, TMO_MS);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, s)) die("thread");

    /* 1. read boot.img; the client also drops its first ACK 3 */
    Loss cl = {0};
    cl.mask = 1u << 3;
    unsigned char req[80];
    size_t rl = mk_req(req, OP_RRQ, "boot.img");
    CHECK(udp_send(cfd, sport, req, rl) == (int)rl);
    int peer = 0, nb = 0;
    static unsigned char got[4096];
    long n = recv_blocks(cfd, &peer, got, sizeof got, &cl, 0, &nb);
    CHECK(n == (long)sizeof s->boot && memcmp(got, s->boot, (size_t)n) == 0);
    CHECK(peer != sport); /* data came from the transfer TID, not the request port */
    printf("RRQ boot.img: %ld bytes in %d blocks, crc %08x, from new TID: yes\n", n, nb, crc32_buf(got, (size_t)n));

    /* 2. write 1024 bytes (exactly two full blocks, so a final empty block follows) */
    unsigned char up[1024];
    for (size_t i = 0; i < sizeof up; i++) up[i] = (unsigned char)(i * 7 + 3);
    rl = mk_req(req, OP_WRQ, "upload.bin");
    CHECK(udp_send(cfd, sport, req, rl) == (int)rl);
    /* wait for ACK 0 from the transfer TID; the server drops its first one and resends after its timeout */
    Loss cl2 = {0};
    cl2.mask = 1u << 2; /* client drops its first DATA 2 */
    unsigned char in[600];
    int wport = 0, tries = 0;
    for (;;) {
        int r = udp_recv(cfd, in, sizeof in, &wport);
        if (r >= 4 && in[1] == OP_ACK && in[3] == 0) break;
        CHECK(++tries < 6);
        cl2.retx++;
    }
    int wb = send_blocks(cfd, wport, up, sizeof up, &cl2);
    CHECK(wb == 3);
    printf("WRQ upload.bin: %zu bytes acked through block %d, client drops=%d\n", sizeof up, wb, cl2.drops);

    /* 3. missing file */
    rl = mk_req(req, OP_RRQ, "nofile.txt");
    CHECK(udp_send(cfd, sport, req, rl) == (int)rl);
    int r = udp_recv(cfd, in, sizeof in, NULL);
    CHECK(r > 4 && in[1] == OP_ERROR);
    printf("RRQ nofile.txt: ERROR code %d \"%s\"\n", in[3], (char *)in + 4);
    pthread_join(th, NULL);
    close(s->fd);
    close(cfd);
    CHECK(s->up_len == (long)sizeof up && memcmp(s->up, up, sizeof up) == 0);
    printf("server stored %ld bytes, crc %08x\n", s->up_len, crc32_buf(s->up, (size_t)s->up_len));
    printf("loss injected: server data=%d, server acks=%d, client acks=%d, client data=%d\n", s->rrq_loss.drops,
           s->wrq_loss.drops, cl.drops, cl2.drops);
    CHECK(s->rrq_loss.retx >= s->rrq_loss.drops && cl2.retx >= cl2.drops && s->wrq_loss.retx >= 1);
    printf("retransmissions covered every drop: yes\n");
    printf("server handled %d requests, %d errors\n", s->handled, s->errors);
    free(s);
    return 0;
}
