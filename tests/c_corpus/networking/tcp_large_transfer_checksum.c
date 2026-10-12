/*
 * title: Large TCP transfer with partial reads and writes
 * topic: networking
 * covers: send return counts, recv variable sizes, 1 MiB stream, FNV checksum, uneven chunking, reader thread
 * deps: libc, posix, pthread, sockets
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 16);
}
static void set_timeout(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    check(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0, "SO_RCVTIMEO");
    check(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0, "SO_SNDTIMEO");
}
static int wait_fd(int fd, short ev, int ms) {
    struct pollfd p;
    p.fd = fd;
    p.events = ev;
    p.revents = 0;
    int r;
    do {
        r = poll(&p, 1, ms);
    } while (r < 0 && errno == EINTR);
    return r > 0;
}
/* socket bound to 127.0.0.1:0; stream sockets also listen */
static int make_socket(int type, int backlog) {
    int fd = socket(AF_INET, type, 0);
    check(fd >= 0, "socket");
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    check(bind(fd, (struct sockaddr *)&a, sizeof a) == 0, "bind");
    if (type == SOCK_STREAM)
        check(listen(fd, backlog) == 0, "listen");
    return fd;
}
static struct sockaddr_in local_addr(int fd) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    memset(&a, 0, sizeof a);
    check(getsockname(fd, (struct sockaddr *)&a, &l) == 0, "getsockname");
    check(l == sizeof a && a.sin_family == AF_INET, "sockname family");
    return a;
}
static int connect_to(struct sockaddr_in a) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    check(fd >= 0, "client socket");
    set_timeout(fd, 2000);
    check(connect(fd, (struct sockaddr *)&a, sizeof a) == 0, "connect");
    return fd;
}
static int accept_to(int ls) {
    check(wait_fd(ls, POLLIN, 2000), "accept ready");
    int c = accept(ls, NULL, NULL);
    check(c >= 0, "accept");
    set_timeout(c, 2000);
    return c;
}

enum { TOTAL = 1 << 20 };

typedef struct {
    int fd;
    size_t bytes;
    uint32_t hash;
    unsigned reads;
    size_t min_read, max_read;
    int short_reads;
} Recv;

static uint32_t fnv_step(uint32_t h, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t rx_state = 0x1234567u;
static size_t next_chunk(uint32_t *st, size_t limit) {
    *st ^= *st << 13;
    *st ^= *st >> 17;
    *st ^= *st << 5;
    size_t n = 1 + (*st % 9000);
    return n < limit ? n : limit;
}

/* reader asks for random sizes; counts how often the kernel returns less than requested */
static void *reader_main(void *arg) {
    Recv *r = arg;
    static unsigned char buf[9000];
    r->hash = 2166136261u;
    r->min_read = (size_t)-1;
    uint32_t st = rx_state;
    for (;;) {
        size_t want = next_chunk(&st, sizeof buf);
        ssize_t n = recv(r->fd, buf, want, 0);
        if (n <= 0)
            break;
        r->hash = fnv_step(r->hash, buf, (size_t)n);
        r->bytes += (size_t)n;
        r->reads++;
        if ((size_t)n < want)
            r->short_reads++;
        if ((size_t)n < r->min_read)
            r->min_read = (size_t)n;
        if ((size_t)n > r->max_read)
            r->max_read = (size_t)n;
    }
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(ls);
    int c = connect_to(la);
    int s = accept_to(ls);
    Recv rc;
    memset(&rc, 0, sizeof rc);
    rc.fd = s;
    pthread_t th;
    check(pthread_create(&th, NULL, reader_main, &rc) == 0, "thread");

    static unsigned char data[TOTAL];
    for (size_t i = 0; i < TOTAL; i += 4) {
        uint32_t v = rnd();
        data[i] = (unsigned char)v;
        data[i + 1] = (unsigned char)(v >> 8);
        data[i + 2] = (unsigned char)(v >> 16);
        data[i + 3] = (unsigned char)(v >> 24);
    }
    uint32_t want = fnv_step(2166136261u, data, TOTAL);

    /* writer: raw send() with random request sizes, handling partial writes itself */
    size_t off = 0;
    uint32_t st = 0xdeadbeefu;
    unsigned writes = 0;
    while (off < TOTAL) {
        size_t n = next_chunk(&st, TOTAL - off) * 5;
        if (n > TOTAL - off)
            n = TOTAL - off;
        ssize_t w = send(c, data + off, n, 0);
        if (w < 0) {
            check(errno == EINTR, "send error");
            continue;
        }
        check(w > 0 && (size_t)w <= n, "send count");
        off += (size_t)w;
        writes++;
    }
    close(c);
    pthread_join(th, NULL);
    check(rc.bytes == TOTAL, "all bytes received");
    check(rc.hash == want, "checksum");
    check(writes > 1 && rc.reads > 1, "chunked");
    printf("transferred %zu bytes\n", rc.bytes);
    printf("checksum %08x matches sender: yes\n", (unsigned)rc.hash);
    printf("received in multiple reads: %s\n", rc.reads > 100 ? "yes" : "no");
    printf("sent in multiple writes: %s\n", writes > 10 ? "yes" : "no");
    printf("no read exceeded the largest request: %s\n", rc.max_read <= 9000 ? "yes" : "no");
    close(s);
    close(ls);
    return 0;
}
