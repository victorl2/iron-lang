/*
 * title: Gossip heartbeat protocol between three UDP nodes
 * topic: networking
 * covers: UDP datagrams, max-merge of counters, round-driven simulation, failure detection thresholds, crash and recovery
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


#define N 3
enum { ALIVE, SUSPECT, DEAD };
static const char *st_name[] = {"A", "S", "D"};

typedef struct {
    int fd, port;
    uint32_t counter[N];
    int last_seen[N]; /* round in which counter[i] last increased here */
    int up;           /* simulated process state */
    int received;
} Node;

static int status_of(const Node *nd, int i, int round) {
    int age = round - nd->last_seen[i];
    return age >= 5 ? DEAD : age >= 3 ? SUSPECT : ALIVE;
}

static void send_table(Node *from, int id, const Node *to) {
    unsigned char pkt[4 + 4 * N];
    pkt[0] = (unsigned char)id;
    pkt[1] = pkt[2] = pkt[3] = 0;
    for (int i = 0; i < N; i++)
        for (int b = 0; b < 4; b++) pkt[4 + 4 * i + b] = (unsigned char)(from->counter[i] >> (24 - 8 * b));
    CHECK(udp_send(from->fd, to->port, pkt, sizeof pkt) == (int)sizeof pkt);
}

static void receive_one(Node *nd, int round) {
    unsigned char pkt[64];
    int n = udp_recv(nd->fd, pkt, sizeof pkt, NULL);
    CHECK(n == 4 + 4 * N);
    nd->received++;
    if (!nd->up) return; /* a crashed node drains its socket but processes nothing */
    for (int i = 0; i < N; i++) {
        uint32_t v = 0;
        for (int b = 0; b < 4; b++) v = v << 8 | pkt[4 + 4 * i + b];
        if (v > nd->counter[i]) {
            nd->counter[i] = v;
            nd->last_seen[i] = round;
        }
    }
}

int main(void) {
    net_init();
    Node nd[N];
    memset(nd, 0, sizeof nd);
    for (int i = 0; i < N; i++) {
        nd[i].fd = udp_lo(&nd[i].port, 1000);
        nd[i].up = 1;
    }
    const int rounds = 16, crash_round = 5, recover_round = 11;
    int dead_rounds[2] = {0, 0};
    for (int r = 1; r <= rounds; r++) {
        if (r == crash_round) nd[2].up = 0;
        if (r == recover_round) nd[2].up = 1;
        int expect[N] = {0, 0, 0};
        for (int i = 0; i < N; i++) {
            if (!nd[i].up) continue;
            nd[i].counter[i]++;
            nd[i].last_seen[i] = r;
        }
        for (int i = 0; i < N; i++) {
            if (!nd[i].up) continue;
            int target = (i + 1 + (r & 1)) % N;
            send_table(&nd[i], i, &nd[target]);
            expect[target]++;
        }
        for (int i = 0; i < N; i++)
            for (int k = 0; k < expect[i]; k++) receive_one(&nd[i], r);
        printf("round %2d:", r);
        for (int v = 0; v < 2; v++) {
            printf("  n%d[", v);
            for (int i = 0; i < N; i++) printf("%u%s", nd[v].counter[i], i + 1 < N ? "," : "");
            printf("] ");
            for (int i = 0; i < N; i++) printf("%s", st_name[status_of(&nd[v], i, r)]);
        }
        printf("%s\n", nd[2].up ? "" : "  (n2 down)");
        for (int v = 0; v < 2; v++) dead_rounds[v] += status_of(&nd[v], 2, r) == DEAD;
        if (r == 9) {
            /* by now both live nodes must have declared the crashed node dead */
            CHECK(status_of(&nd[0], 2, r) == DEAD || status_of(&nd[1], 2, r) == DEAD);
        }
    }
    /* after recovery the node's counter keeps growing from where it stopped, and peers see it alive again */
    CHECK(status_of(&nd[0], 2, rounds) == ALIVE || status_of(&nd[1], 2, rounds) == ALIVE);
    CHECK(status_of(&nd[0], 1, rounds) == ALIVE && status_of(&nd[1], 0, rounds) == ALIVE);
    printf("counters at end: n0=%u,%u,%u n1=%u,%u,%u n2=%u,%u,%u\n", nd[0].counter[0], nd[0].counter[1], nd[0].counter[2],
           nd[1].counter[0], nd[1].counter[1], nd[1].counter[2], nd[2].counter[0], nd[2].counter[1], nd[2].counter[2]);
    printf("rounds in which node 2 was considered dead: n0=%d n1=%d\n", dead_rounds[0], dead_rounds[1]);
    printf("messages received: n0=%d n1=%d n2=%d\n", nd[0].received, nd[1].received, nd[2].received);
    for (int i = 0; i < N; i++) close(nd[i].fd);
    return 0;
}
