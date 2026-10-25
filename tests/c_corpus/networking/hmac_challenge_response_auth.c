/*
 * title: HMAC-SHA256 challenge-response authentication
 * topic: networking
 * covers: inline SHA-256 and HMAC with known-answer tests, nonce challenge, replay rejection, lockout, constant-time compare
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

/* ---- SHA-1, SHA-256, HMAC, base64, hex ---- */
static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static unsigned char *md_pad(const unsigned char *d, size_t len, size_t *total) {
    size_t t = ((len + 8) / 64 + 1) * 64;
    unsigned char *m = calloc(t, 1);
    if (!m) die("oom");
    if (len) memcpy(m, d, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[t - 1 - (size_t)i] = (unsigned char)(bits >> (8 * i));
    *total = t;
    return m;
}

void sha1_buf(const unsigned char *d, size_t len, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    size_t total;
    unsigned char *m = md_pad(d, len, &total);
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)m[off + 4 * i] << 24) | ((uint32_t)m[off + 4 * i + 1] << 16) |
                   ((uint32_t)m[off + 4 * i + 2] << 8) | (uint32_t)m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & dd); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ dd; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & dd) | (c & dd); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ dd; k = 0xCA62C1D6u; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = dd; dd = c; c = rol32(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e;
    }
    free(m);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (unsigned char)(h[i] >> (24 - 8 * j));
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void sha256_buf(const unsigned char *d, size_t len, unsigned char out[32]) {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    size_t total;
    unsigned char *m = md_pad(d, len, &total);
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)m[off + 4 * i] << 24) | ((uint32_t)m[off + 4 * i + 1] << 16) |
                   ((uint32_t)m[off + 4 * i + 2] << 8) | (uint32_t)m[off + 4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t v[8];
        memcpy(v, h, sizeof v);
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror32(v[4], 6) ^ ror32(v[4], 11) ^ ror32(v[4], 25);
            uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            uint32_t t1 = v[7] + S1 + ch + K256[i] + w[i];
            uint32_t S0 = ror32(v[0], 2) ^ ror32(v[0], 13) ^ ror32(v[0], 22);
            uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            uint32_t t2 = S0 + mj;
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
            v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) h[i] += v[i];
    }
    free(m);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (unsigned char)(h[i] >> (24 - 8 * j));
}

void hmac_sha256(const unsigned char *key, size_t kl, const unsigned char *msg, size_t ml,
                 unsigned char out[32]) {
    unsigned char k0[64], ipad[64], opad[64], inner[32];
    memset(k0, 0, sizeof k0);
    if (kl > 64) sha256_buf(key, kl, k0);
    else if (kl) memcpy(k0, key, kl);
    for (int i = 0; i < 64; i++) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }
    unsigned char *b = malloc(64 + ml + 32);
    if (!b) die("oom");
    memcpy(b, ipad, 64);
    if (ml) memcpy(b + 64, msg, ml);
    sha256_buf(b, 64 + ml, inner);
    memcpy(b, opad, 64);
    memcpy(b + 64, inner, 32);
    sha256_buf(b, 96, out);
    free(b);
}

void to_hex(const unsigned char *d, size_t n, char *out) {
    static const char *hx = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15];
    }
    out[2 * n] = 0;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void b64_enc(const unsigned char *d, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

/* returns decoded length or -1 */
int b64_dec(const char *s, unsigned char *out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (; *s && *s != '='; s++) {
        const char *p = strchr(B64, *s);
        if (!p) return -1;
        acc = (acc << 6) | (uint32_t)(p - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (unsigned char)(acc >> bits);
            acc &= (1u << bits) - 1u;
        }
    }
    return (int)o;
}


typedef struct { const char *user, *secret; int failures; } Account;

typedef struct {
    int lfd;
    Account acct[3];
    uint64_t rng;
    int ok, fail, locked;
    char last_nonce[40];
} Server;

static int ct_eq(const char *a, const char *b) {
    size_t la = strlen(a);
    unsigned d = (unsigned)(la ^ strlen(b));
    for (size_t i = 0; i < la && b[i]; i++) d |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
    return d == 0;
}

static void compute(const char *secret, const char *tag, const char *nonce_hex, const char *user, char *out_hex) {
    char msg[128];
    int ml = snprintf(msg, sizeof msg, "%s|%s|%s", tag, nonce_hex, user);
    unsigned char mac[32];
    hmac_sha256((const unsigned char *)secret, strlen(secret), (const unsigned char *)msg, (size_t)ml, mac);
    to_hex(mac, 32, out_hex);
}

static void *server_main(void *arg) {
    Server *s = arg;
    for (int i = 0; i < 7; i++) {
        int fd = accept_lo(s->lfd);
        if (fd < 0) return NULL;
        Conn c;
        conn_init(&c, fd);
        char line[200], user[24];
        if (conn_readline(&c, line, sizeof line) < 0 || sscanf(line, "HELLO %23s", user) != 1) { close(fd); continue; }
        Account *a = NULL;
        for (int k = 0; k < 3; k++)
            if (!strcmp(s->acct[k].user, user)) a = &s->acct[k];
        if (a && a->failures >= 3) {
            s->locked++;
            send_str(fd, "LOCKED\n");
            close(fd);
            continue;
        }
        unsigned char nb[16];
        for (int k = 0; k < 16; k += 4) {
            uint32_t r = rng32(&s->rng);
            for (int b = 0; b < 4; b++) nb[k + b] = (unsigned char)(r >> (8 * b));
        }
        char nonce[40];
        to_hex(nb, 16, nonce);
        snprintf(s->last_nonce, sizeof s->last_nonce, "%s", nonce);
        sendf(fd, "CHALLENGE %s\n", nonce);
        if (conn_readline(&c, line, sizeof line) < 0 || strncmp(line, "RESPONSE ", 9) != 0) { close(fd); continue; }
        /* unknown users are checked against a dummy secret so timing and replies look the same */
        char want[80];
        compute(a ? a->secret : "no-such-user-secret", "auth", nonce, user, want);
        if (a && ct_eq(want, line + 9)) {
            a->failures = 0;
            char sess[80];
            compute(a->secret, "session", nonce, user, sess);
            sess[16] = 0;
            sendf(fd, "OK session=%s\n", sess);
            s->ok++;
        } else {
            if (a) a->failures++;
            send_str(fd, "FAIL\n");
            s->fail++;
        }
        close(fd);
    }
    return NULL;
}

static void hexcheck(const char *what, const unsigned char *mac, size_t n, const char *want) {
    char h[80];
    to_hex(mac, n, h);
    printf("%s: %s\n", what, h);
    CHECK(strcmp(h, want) == 0);
}

/* runs one login attempt. secret may differ from the real one. returns reply, records nonce/response */
static void attempt(int port, const char *user, const char *secret, const char *replay, char *reply, size_t rcap,
                    char *nonce_out, char *resp_out) {
    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);
    sendf(fd, "HELLO %s\n", user);
    char line[200];
    CHECK(conn_readline(&c, line, sizeof line) > 0);
    if (!strncmp(line, "LOCKED", 6)) {
        snprintf(reply, rcap, "%s", line);
        close(fd);
        return;
    }
    CHECK(!strncmp(line, "CHALLENGE ", 10));
    snprintf(nonce_out, 40, "%s", line + 10);
    char resp[80];
    if (replay) snprintf(resp, sizeof resp, "%s", replay);
    else compute(secret, "auth", nonce_out, user, resp);
    snprintf(resp_out, 80, "%s", resp);
    sendf(fd, "RESPONSE %s\n", resp);
    CHECK(conn_readline(&c, reply, rcap) > 0);
    close(fd);
}

int main(void) {
    net_init();
    unsigned char mac[32];
    /* known answers: FIPS 180-2 and RFC 4231 */
    sha256_buf((const unsigned char *)"abc", 3, mac);
    hexcheck("sha256(abc)", mac, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256_buf((const unsigned char *)"", 0, mac);
    hexcheck("sha256(empty)", mac, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    unsigned char k1[20];
    memset(k1, 0x0b, sizeof k1);
    hmac_sha256(k1, sizeof k1, (const unsigned char *)"Hi There", 8, mac);
    hexcheck("rfc4231 case 1", mac, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    hmac_sha256((const unsigned char *)"Jefe", 4, (const unsigned char *)"what do ya want for nothing?", 28, mac);
    hexcheck("rfc4231 case 2", mac, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    Server *s = calloc(1, sizeof *s);
    if (!s) die("oom");
    s->acct[0] = (Account){"ada", "correct horse", 0};
    s->acct[1] = (Account){"bob", "battery staple", 0};
    s->acct[2] = (Account){"eve", "hunter2", 0};
    s->rng = 0x1234ABCDu;
    int port;
    s->lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, s)) die("thread");

    char reply[100], nonce[40], resp[80], nonce2[40], resp2[80];
    attempt(port, "ada", "correct horse", NULL, reply, sizeof reply, nonce, resp);
    printf("ada with the right secret: %s\n", reply);
    CHECK(!strncmp(reply, "OK ", 3));
    attempt(port, "ada", "", resp, reply, sizeof reply, nonce2, resp2);
    printf("ada replaying the captured response on a new challenge: %s (nonces differ: %s)\n", reply,
           strcmp(nonce, nonce2) ? "yes" : "no");
    CHECK(!strcmp(reply, "FAIL") && strcmp(nonce, nonce2) != 0);
    attempt(port, "bob", "wrong secret", NULL, reply, sizeof reply, nonce, resp);
    printf("bob with a wrong secret: %s\n", reply);
    attempt(port, "mallory", "anything", NULL, reply, sizeof reply, nonce, resp);
    printf("unknown user gets a normal challenge and: %s\n", reply);
    for (int i = 0; i < 2; i++) attempt(port, "bob", "guess", NULL, reply, sizeof reply, nonce, resp);
    /* bob has now failed three times: even the right secret is refused */
    attempt(port, "bob", "battery staple", NULL, reply, sizeof reply, nonce, resp);
    printf("bob after 3 failures, right secret: %s\n", reply);
    CHECK(!strcmp(reply, "LOCKED"));
    pthread_join(th, NULL);
    close(s->lfd);
    printf("server: ok=%d fail=%d locked=%d\n", s->ok, s->fail, s->locked);
    CHECK(s->ok == 1 && s->fail == 5 && s->locked == 1);
    free(s);
    return 0;
}
