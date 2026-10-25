/*
 * title: Pipelined fixed-size requests over one TCP connection
 * topic: networking
 * covers: request pipelining, response ordering, stateful server, send window, binary record codec, bulk send before any read
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
static int send_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
/* reads until n bytes, EOF, or error; returns bytes read */
static size_t recv_all(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    return got;
}

enum { REC = 12, NREQ = 300 };

typedef struct {
    uint32_t id, op, arg;
} Req;

/* the same state machine runs in the server and in the local reference */
typedef struct {
    uint32_t acc;
    uint32_t count;
    uint32_t max;
} State;

static uint32_t apply(State *st, const Req *r) {
    st->count++;
    switch (r->op % 4) {
    case 0:
        st->acc += r->arg;
        break;
    case 1:
        st->acc ^= r->arg * 2654435761u;
        break;
    case 2:
        st->acc = (st->acc << 3) | (st->acc >> 29);
        st->acc += r->id;
        break;
    default:
        if (r->arg > st->max)
            st->max = r->arg;
        st->acc -= st->max;
        break;
    }
    return st->acc;
}

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}
static uint32_t get32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

typedef struct {
    int fd;
    State st;
    int served;
} Server;

static void *server_main(void *arg) {
    Server *s = arg;
    unsigned char in[REC], out[8];
    while (recv_all(s->fd, in, REC) == REC) {
        Req r = {get32(in), get32(in + 4), get32(in + 8)};
        uint32_t v = apply(&s->st, &r);
        put32(out, r.id);
        put32(out + 4, v);
        if (send_all(s->fd, out, sizeof out) < 0)
            break;
        s->served++;
    }
    close(s->fd);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ls = make_socket(SOCK_STREAM, 2);
    struct sockaddr_in la = local_addr(ls);
    int c = connect_to(la);
    Server srv;
    memset(&srv, 0, sizeof srv);
    srv.fd = accept_to(ls);
    pthread_t th;
    check(pthread_create(&th, NULL, server_main, &srv) == 0, "thread");

    Req reqs[NREQ];
    State ref;
    memset(&ref, 0, sizeof ref);
    uint32_t want[NREQ];
    for (int i = 0; i < NREQ; i++) {
        reqs[i].id = (uint32_t)i;
        reqs[i].op = rnd();
        reqs[i].arg = rnd() % 100000;
        want[i] = apply(&ref, &reqs[i]);
    }

    /* phase 1: the whole batch goes out before a single response is read */
    unsigned char wire[NREQ * REC];
    for (int i = 0; i < NREQ; i++) {
        put32(wire + i * REC, reqs[i].id);
        put32(wire + i * REC + 4, reqs[i].op);
        put32(wire + i * REC + 8, reqs[i].arg);
    }
    int batch = 100;
    check(send_all(c, wire, (size_t)batch * REC) == 0, "batch send");
    uint32_t digest = 0;
    for (int i = 0; i < batch; i++) {
        unsigned char rsp[8];
        check(recv_all(c, rsp, 8) == 8, "batch response");
        check(get32(rsp) == (uint32_t)i && get32(rsp + 4) == want[i], "in-order response");
        digest = digest * 31u + get32(rsp + 4);
    }
    printf("batch of %d requests answered in order, digest %08x\n", batch, (unsigned)digest);

    /* phase 2: a sliding window of 16 outstanding requests over the remaining ones */
    int next = batch, done = batch, window = 16, max_inflight = 0;
    while (done < NREQ) {
        while (next < NREQ && next - done < window) {
            check(send_all(c, wire + next * REC, REC) == 0, "window send");
            next++;
        }
        if (next - done > max_inflight)
            max_inflight = next - done;
        unsigned char rsp[8];
        check(recv_all(c, rsp, 8) == 8, "window response");
        check(get32(rsp) == (uint32_t)done && get32(rsp + 4) == want[done], "window order");
        digest = digest * 31u + get32(rsp + 4);
        done++;
    }
    printf("window phase max in flight: %d\n", max_inflight);
    printf("final digest %08x\n", (unsigned)digest);
    close(c);
    pthread_join(th, NULL);
    check(srv.served == NREQ, "served");
    check(srv.st.acc == ref.acc && srv.st.count == ref.count && srv.st.max == ref.max, "server state");
    printf("server processed %d requests, final accumulator %08x, max arg %u\n", srv.served, (unsigned)srv.st.acc,
           (unsigned)srv.st.max);
    close(ls);
    return 0;
}
