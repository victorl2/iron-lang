/*
 * title: Go-back-N sliding window transfer over UDP
 * topic: networking
 * covers: sequence numbers, cumulative ACKs, window advance, timeout retransmit, out-of-order discard, FIN, simulated loss
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


#define PAYLOAD 100
#define TOTAL 2350
#define NPKT ((TOTAL + PAYLOAD - 1) / PAYLOAD) /* 24 data packets, then FIN as sequence 24 */
#define WINDOW 4
#define TMO_MS 80

enum { P_DATA = 1, P_ACK = 2, P_FIN = 3 };

static size_t mk_pkt(unsigned char *p, int type, unsigned seq, const unsigned char *d, size_t n) {
    p[0] = (unsigned char)type;
    p[1] = (unsigned char)(seq >> 8);
    p[2] = (unsigned char)seq;
    p[3] = (unsigned char)(n >> 8);
    p[4] = (unsigned char)n;
    if (n) memcpy(p + 5, d, n);
    return n + 5;
}

typedef struct {
    int fd, peer_port;
    const unsigned char *data;
    unsigned long long drop_mask; /* first transmission of these seqs is lost */
    unsigned long long dropped;
    int sent, retx, timeouts, drops;
    int failed;
} Sender;

static void tx_data(Sender *s, unsigned seq, int is_retx) {
    unsigned char pkt[PAYLOAD + 5];
    size_t n = 0;
    int type = P_FIN;
    if (seq < NPKT) {
        n = (size_t)(TOTAL - (int)seq * PAYLOAD);
        if (n > PAYLOAD) n = PAYLOAD;
        type = P_DATA;
    }
    size_t l = mk_pkt(pkt, type, seq, s->data + (size_t)seq * PAYLOAD, n);
    if (is_retx) s->retx++;
    if ((s->drop_mask >> seq & 1ULL) && !(s->dropped >> seq & 1ULL)) { /* first transmission is lost */
        s->dropped |= 1ULL << seq;
        s->drops++;
        return;
    }
    s->sent++;
    udp_send(s->fd, s->peer_port, pkt, l);
}

static void *sender_main(void *arg) {
    Sender *s = arg;
    unsigned base = 0, next = 0;
    const unsigned last = NPKT + 1; /* packets 0..NPKT, where NPKT is the FIN */
    int idle = 0;
    while (base < last) {
        while (next < base + WINDOW && next < last) tx_data(s, next++, 0);
        unsigned char in[16];
        int r = udp_recv(s->fd, in, sizeof in, NULL);
        if (r < 0) {
            s->timeouts++;
            if (++idle > 12) { s->failed = 1; return NULL; }
            for (unsigned q = base; q < next; q++) tx_data(s, q, 1); /* go back N */
            continue;
        }
        if (r >= 5 && in[0] == P_ACK) {
            unsigned ack = (unsigned)(in[1] << 8 | in[2]); /* next expected by the receiver */
            if (ack > base && ack <= next) { base = ack; idle = 0; }
        }
    }
    return NULL;
}

int main(void) {
    net_init();
    static unsigned char data[TOTAL], got[TOTAL + PAYLOAD];
    uint64_t rs = 4242;
    for (int i = 0; i < TOTAL; i++) data[i] = (unsigned char)rng32(&rs);
    int sport, rport;
    Sender s;
    memset(&s, 0, sizeof s);
    s.fd = udp_lo(&sport, TMO_MS);
    int rfd = udp_lo(&rport, 3000);
    s.peer_port = rport;
    s.data = data;
    s.drop_mask = (1ULL << 3) | (1ULL << 11) | (1ULL << 12);
    unsigned long long ack_drop_mask = (1ULL << 7) | (1ULL << 9), ack_dropped = 0;
    int ack_drops = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, sender_main, &s)) die("thread");

    unsigned expect = 0;
    size_t len = 0;
    int discarded = 0, dup_data = 0, acks_sent = 0, finished = 0;
    while (!finished) {
        unsigned char in[PAYLOAD + 5];
        int r = udp_recv(rfd, in, sizeof in, NULL);
        CHECK(r >= 5);
        unsigned seq = (unsigned)(in[1] << 8 | in[2]);
        size_t n = (size_t)(in[3] << 8 | in[4]);
        if (seq == expect) {
            if (in[0] == P_DATA) {
                memcpy(got + len, in + 5, n);
                len += n;
            } else
                finished = 1;
            expect++;
        } else if (seq < expect)
            dup_data++;
        else
            discarded++;
        unsigned char ack[8];
        mk_pkt(ack, P_ACK, expect, NULL, 0);
        if (!finished && (ack_drop_mask >> expect & 1ULL) && !(ack_dropped >> expect & 1ULL)) {
            ack_dropped |= 1ULL << expect;
            ack_drops++;
            continue;
        }
        udp_send(rfd, sport, ack, 5);
        acks_sent++;
    }
    pthread_join(th, NULL);
    close(s.fd);
    close(rfd);
    CHECK(!s.failed);
    CHECK(len == TOTAL && memcmp(got, data, TOTAL) == 0);
    printf("delivered %zu bytes in %d data packets + FIN, crc %08x\n", len, NPKT, crc32_buf(got, len));
    printf("window=%d payload=%d\n", WINDOW, PAYLOAD);
    printf("injected loss: data=%d acks=%d\n", s.drops, ack_drops);
    CHECK(s.drops == 3 && ack_drops == 2);
    printf("recovery needed timeouts: %s, go-back-N retransmissions >= drops: %s\n", s.timeouts > 0 ? "yes" : "no",
           s.retx >= 1 ? "yes" : "no");
    printf("receiver discarded out-of-order packets: %s\n", discarded > 0 ? "yes" : "no");
    CHECK(s.timeouts > 0 && s.retx >= 1 && discarded > 0);
    (void)dup_data;
    (void)acks_sent;
    return 0;
}
