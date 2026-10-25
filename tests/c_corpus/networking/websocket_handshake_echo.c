/*
 * title: WebSocket handshake (SHA-1 + base64) and masked frame echo
 * topic: networking
 * covers: RFC 6455 accept key, frame encode/decode, masking, 16/64-bit lengths, fragmentation, ping/pong, close codes
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

/* ---- minimal HTTP/1.1 helpers ---- */
typedef struct {
    int n;
    char k[24][40];
    char v[24][256];
} Hdrs;

const char *hget(const Hdrs *h, const char *name) {
    for (int i = 0; i < h->n; i++)
        if (strcasecmp(h->k[i], name) == 0) return h->v[i];
    return NULL;
}

/* reads header lines until blank line; returns 0 ok, -1 error */
int read_headers(Conn *c, Hdrs *h) {
    char line[1024];
    h->n = 0;
    for (;;) {
        int n = conn_readline(c, line, sizeof line);
        if (n < 0) return -1;
        if (n == 0) return 0;
        char *colon = strchr(line, ':');
        if (!colon || h->n >= 24) return -1;
        *colon++ = 0;
        while (*colon == ' ') colon++;
        snprintf(h->k[h->n], sizeof h->k[0], "%s", line);
        snprintf(h->v[h->n], sizeof h->v[0], "%s", colon);
        h->n++;
    }
}

typedef struct {
    char method[16], target[256], version[16];
    Hdrs h;
} ReqHead;

/* returns 0 ok, -1 eof/error, -2 malformed request line */
int read_req_head(Conn *c, ReqHead *r) {
    char line[1024];
    int n = conn_readline(c, line, sizeof line);
    if (n < 0) return -1;
    char *sp1 = strchr(line, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2) {
        r->h.n = 0;
        return -2;
    }
    *sp1 = 0;
    *sp2 = 0;
    snprintf(r->method, sizeof r->method, "%s", line);
    snprintf(r->target, sizeof r->target, "%s", sp1 + 1);
    snprintf(r->version, sizeof r->version, "%s", sp2 + 1);
    return read_headers(c, &r->h);
}

/* reads "HTTP/1.1 200 OK" plus headers. returns status code or -1 */
int read_status(Conn *c, Hdrs *h, char *reason, size_t rcap) {
    char line[1024];
    if (conn_readline(c, line, sizeof line) < 0) return -1;
    if (strncmp(line, "HTTP/1.", 7) != 0) return -1;
    int code = atoi(line + 9);
    if (reason) snprintf(reason, rcap, "%s", line + 13);
    if (read_headers(c, h) < 0) return -1;
    return code;
}

/* decodes a chunked body into out; returns length or -1 */
long read_chunked(Conn *c, unsigned char *out, size_t cap, Hdrs *trailers) {
    size_t total = 0;
    char line[256];
    for (;;) {
        if (conn_readline(c, line, sizeof line) < 0) return -1;
        char *semi = strchr(line, ';');
        if (semi) *semi = 0;
        char *end;
        unsigned long sz = strtoul(line, &end, 16);
        if (end == line) return -1;
        if (sz == 0) break;
        if (total + sz > cap) return -1;
        if (conn_readn(c, out + total, sz) < 0) return -1;
        total += sz;
        if (conn_readline(c, line, sizeof line) != 0) return -1;
    }
    Hdrs tmp;
    if (read_headers(c, trailers ? trailers : &tmp) < 0) return -1;
    return (long)total;
}

/* reads body per Content-Length or chunked; returns length or -1 */
long read_body(Conn *c, const Hdrs *h, unsigned char *out, size_t cap) {
    const char *te = hget(h, "Transfer-Encoding");
    if (te && strcasecmp(te, "chunked") == 0) return read_chunked(c, out, cap, NULL);
    const char *cl = hget(h, "Content-Length");
    if (!cl) return 0;
    long n = atol(cl);
    if (n < 0 || (size_t)n > cap) return -1;
    if (conn_readn(c, out, (size_t)n) < 0) return -1;
    return n;
}

const char *reason_of(int code) {
    switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    default: return "Unknown";
    }
}

/* sends a response with Content-Length body and optional extra header lines
   (each terminated with \r\n) */
int send_resp(int fd, int code, const char *extra, const void *body, size_t n) {
    char head[1024];
    int hl = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n%s\r\n", code,
                      reason_of(code), n, extra ? extra : "");
    if (hl < 0 || hl >= (int)sizeof head) die("head overflow");
    if (send_all(fd, head, (size_t)hl) < 0) return -1;
    if (n && send_all(fd, body, n) < 0) return -1;
    return 0;
}

typedef struct {
    int code;
    char reason[64];
    Hdrs h;
    unsigned char body[16384];
    long blen;
} Resp;

/* sends a request (Content-Length set when bl>0 or method is POST/PUT) and reads the reply */
int client_req(Conn *c, const char *method, const char *target, const char *extra, const void *body,
               size_t bl, Resp *r) {
    char head[1024];
    int post = strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0;
    int n = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: iron.test\r\n%s", method, target,
                     extra ? extra : "");
    if (post || bl) n += snprintf(head + n, sizeof head - (size_t)n, "Content-Length: %zu\r\n", bl);
    n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
    if (send_all(c->fd, head, (size_t)n) < 0) return -1;
    if (bl && send_all(c->fd, body, bl) < 0) return -1;
    r->code = read_status(c, &r->h, r->reason, sizeof r->reason);
    if (r->code < 0) return -1;
    r->blen = 0;
    r->body[0] = 0;
    if (strcmp(method, "HEAD") == 0 || r->code == 304 || r->code == 204) return 0;
    r->blen = read_body(c, &r->h, r->body, sizeof r->body - 1);
    if (r->blen < 0) return -1;
    r->body[r->blen] = 0;
    return 0;
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


#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
enum { OP_CONT = 0, OP_TEXT = 1, OP_BIN = 2, OP_CLOSE = 8, OP_PING = 9, OP_PONG = 10 };

static void accept_key(const char *key, char *out) {
    char cat[128];
    snprintf(cat, sizeof cat, "%s%s", key, WS_GUID);
    unsigned char d[20];
    sha1_buf((const unsigned char *)cat, strlen(cat), d);
    b64_enc(d, 20, out);
}

/* encodes a frame into out, returns its length; mask==NULL means unmasked */
static size_t ws_encode(unsigned char *out, int op, int fin, const unsigned char *mask, const unsigned char *d, size_t n) {
    size_t o = 0;
    out[o++] = (unsigned char)((fin ? 0x80 : 0) | op);
    unsigned mbit = mask ? 0x80 : 0;
    if (n < 126) out[o++] = (unsigned char)(mbit | n);
    else if (n < 65536) { out[o++] = (unsigned char)(mbit | 126); out[o++] = (unsigned char)(n >> 8); out[o++] = (unsigned char)n; }
    else {
        out[o++] = (unsigned char)(mbit | 127);
        for (int i = 7; i >= 0; i--) out[o++] = (unsigned char)((uint64_t)n >> (8 * i));
    }
    if (mask) { memcpy(out + o, mask, 4); o += 4; }
    for (size_t i = 0; i < n; i++) out[o + i] = mask ? (unsigned char)(d[i] ^ mask[i & 3]) : d[i];
    return o + n;
}

typedef struct { int op, fin, masked; size_t len; } Frame;

/* reads a frame; payload is unmasked into buf. returns 0 or -1 */
static int ws_read(Conn *c, Frame *f, unsigned char *buf, size_t cap) {
    unsigned char h[2];
    if (conn_readn(c, h, 2) < 0) return -1;
    f->fin = h[0] >> 7;
    f->op = h[0] & 15;
    f->masked = h[1] >> 7;
    uint64_t n = h[1] & 127;
    if (n == 126) { unsigned char e[2]; if (conn_readn(c, e, 2) < 0) return -1; n = (uint64_t)(e[0] << 8 | e[1]); }
    else if (n == 127) { unsigned char e[8]; if (conn_readn(c, e, 8) < 0) return -1; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | e[i]; }
    unsigned char mk[4] = {0};
    if (f->masked && conn_readn(c, mk, 4) < 0) return -1;
    if (n > cap) return -1;
    if (conn_readn(c, buf, (size_t)n) < 0) return -1;
    if (f->masked) for (size_t i = 0; i < n; i++) buf[i] ^= mk[i & 3];
    f->len = (size_t)n;
    return 0;
}

typedef struct {
    int lfd;
    int frames, messages, pings, protocol_errors;
    size_t bytes;
} Server;

#define MAXMSG 80000

static void ws_serve(Server *s, int fd, Conn *c) {
    unsigned char *msg = malloc(MAXMSG), *buf = malloc(MAXMSG), *out = malloc(MAXMSG + 16);
    size_t mlen = 0;
    int mop = 0;
    for (;;) {
        Frame f;
        if (ws_read(c, &f, buf, MAXMSG) < 0) break;
        s->frames++;
        if (!f.masked) {
            unsigned char cl[2] = {1002 >> 8, 1002 & 255};
            size_t n = ws_encode(out, OP_CLOSE, 1, NULL, cl, 2);
            send_all(fd, out, n);
            s->protocol_errors++;
            break;
        }
        if (f.op == OP_PING) {
            s->pings++;
            size_t n = ws_encode(out, OP_PONG, 1, NULL, buf, f.len);
            send_all(fd, out, n);
        } else if (f.op == OP_CLOSE) {
            size_t n = ws_encode(out, OP_CLOSE, 1, NULL, buf, f.len >= 2 ? 2 : 0);
            send_all(fd, out, n);
            break;
        } else if (f.op == OP_TEXT || f.op == OP_BIN || f.op == OP_CONT) {
            if (f.op != OP_CONT) { mop = f.op; mlen = 0; }
            memcpy(msg + mlen, buf, f.len);
            mlen += f.len;
            if (f.fin) {
                size_t n = ws_encode(out, mop, 1, NULL, msg, mlen);
                send_all(fd, out, n);
                s->messages++;
                s->bytes += mlen;
                mlen = 0;
            }
        }
    }
    free(msg); free(buf); free(out);
}

static void *server_main(void *arg) {
    Server *s = arg;
    for (int i = 0; i < 3; i++) {
        int fd = accept_lo(s->lfd);
        if (fd < 0) return NULL;
        Conn c;
        conn_init(&c, fd);
        ReqHead r;
        if (read_req_head(&c, &r) != 0) { close(fd); continue; }
        const char *up = hget(&r.h, "Upgrade"), *key = hget(&r.h, "Sec-WebSocket-Key"), *ver = hget(&r.h, "Sec-WebSocket-Version");
        if (!up || strcasecmp(up, "websocket") != 0 || !key || !ver || strcmp(ver, "13") != 0) {
            send_resp(fd, 400, "Connection: close\r\n", NULL, 0);
            close(fd);
            continue;
        }
        char acc[64];
        accept_key(key, acc);
        sendf(fd, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
        ws_serve(s, fd, &c);
        close(fd);
    }
    return NULL;
}

static void hexline(const unsigned char *d, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x%s", d[i], i + 1 < n ? " " : "");
    printf("\n");
}

static int ws_connect(int port, const char *key, Conn *c) {
    int fd = connect_lo(port);
    conn_init(c, fd);
    sendf(fd, "GET /chat HTTP/1.1\r\nHost: iron.test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", key);
    Hdrs h;
    int code = read_status(c, &h, NULL, 0);
    CHECK(code == 101);
    char want[64];
    accept_key(key, want);
    CHECK(hget(&h, "Sec-WebSocket-Accept") && strcmp(hget(&h, "Sec-WebSocket-Accept"), want) == 0);
    printf("handshake: 101, accept=%s\n", want);
    return fd;
}

static void send_frame(int fd, uint64_t *rs, int op, int fin, const unsigned char *d, size_t n) {
    unsigned char mk[4];
    uint32_t r = rng32(rs);
    for (int b = 0; b < 4; b++) mk[b] = (unsigned char)(r >> (8 * b));
    unsigned char *buf = malloc(n + 16);
    size_t l = ws_encode(buf, op, fin, mk, d, n);
    CHECK(send_all(fd, buf, l) == 0);
    free(buf);
}

int main(void) {
    net_init();
    /* known answers from RFC 6455 */
    char acc[64];
    accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc);
    printf("rfc6455 accept: %s\n", acc);
    CHECK(strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
    unsigned char fr[16];
    const unsigned char mk[4] = {0x37, 0xfa, 0x21, 0x3d};
    size_t fl = ws_encode(fr, OP_TEXT, 1, mk, (const unsigned char *)"Hello", 5);
    printf("masked Hello: ");
    hexline(fr, fl);
    const unsigned char expect_masked[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
    CHECK(fl == sizeof expect_masked && memcmp(fr, expect_masked, fl) == 0);
    fl = ws_encode(fr, OP_TEXT, 1, NULL, (const unsigned char *)"Hello", 5);
    printf("unmasked Hello: ");
    hexline(fr, fl);
    CHECK(fl == 7 && fr[0] == 0x81 && fr[1] == 5);

    Server *s = calloc(1, sizeof *s);
    if (!s) die("oom");
    int port;
    s->lfd = listen_lo(&port);
    pthread_t th;
    if (pthread_create(&th, NULL, server_main, s)) die("thread");

    /* not an upgrade request */
    int fd = connect_lo(port);
    Conn c;
    conn_init(&c, fd);
    send_str(fd, "GET /chat HTTP/1.1\r\nHost: iron.test\r\n\r\n");
    Hdrs h;
    printf("plain GET: %d\n", read_status(&c, &h, NULL, 0));
    close(fd);

    /* main session */
    uint64_t rs = 6455;
    fd = ws_connect(port, "dGhlIHNhbXBsZSBub25jZQ==", &c);
    unsigned char *buf = malloc(MAXMSG), *big = malloc(MAXMSG);
    Frame f;
    const struct { int op; size_t len; const char *name; } sizes[] = {
        {OP_TEXT, 5, "text 5"}, {OP_BIN, 125, "binary 125"}, {OP_BIN, 126, "binary 126"},
        {OP_BIN, 300, "binary 300"}, {OP_BIN, 65535, "binary 65535"}, {OP_BIN, 65536, "binary 65536"}, {OP_BIN, 70000, "binary 70000"},
    };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        for (size_t k = 0; k < sizes[i].len; k++) big[k] = (unsigned char)(sizes[i].op == OP_TEXT ? 'a' + k % 26 : rng32(&rs));
        send_frame(fd, &rs, sizes[i].op, 1, big, sizes[i].len);
        CHECK(ws_read(&c, &f, buf, MAXMSG) == 0);
        CHECK(f.op == sizes[i].op && f.fin && !f.masked && f.len == sizes[i].len && memcmp(buf, big, f.len) == 0);
        printf("echo %-13s ok (crc %08x)\n", sizes[i].name, crc32_buf(buf, f.len));
    }
    /* ping with payload */
    send_frame(fd, &rs, OP_PING, 1, (const unsigned char *)"keepalive", 9);
    CHECK(ws_read(&c, &f, buf, MAXMSG) == 0 && f.op == OP_PONG);
    buf[f.len] = 0;
    printf("pong payload: %s\n", (char *)buf);
    /* fragmented text with a ping in between */
    send_frame(fd, &rs, OP_TEXT, 0, (const unsigned char *)"Hel", 3);
    send_frame(fd, &rs, OP_CONT, 0, (const unsigned char *)"lo, ", 4);
    send_frame(fd, &rs, OP_PING, 1, (const unsigned char *)"mid", 3);
    send_frame(fd, &rs, OP_CONT, 1, (const unsigned char *)"fragments", 9);
    CHECK(ws_read(&c, &f, buf, MAXMSG) == 0 && f.op == OP_PONG);
    CHECK(ws_read(&c, &f, buf, MAXMSG) == 0 && f.op == OP_TEXT && f.fin);
    buf[f.len] = 0;
    printf("reassembled after interleaved ping: %s\n", (char *)buf);
    /* orderly close */
    unsigned char cc[5] = {1000 >> 8, 1000 & 255, 'b', 'y', 'e'};
    send_frame(fd, &rs, OP_CLOSE, 1, cc, 5);
    CHECK(ws_read(&c, &f, buf, MAXMSG) == 0 && f.op == OP_CLOSE && f.len == 2);
    printf("close echoed with code %d\n", buf[0] << 8 | buf[1]);
    CHECK(conn_getc(&c) < 0);
    close(fd);

    /* protocol violation: unmasked client frame */
    fd = ws_connect(port, "x3JJHMbDL1EzLkh9GBhXDw==", &c);
    fl = ws_encode(fr, OP_TEXT, 1, NULL, (const unsigned char *)"nomask", 6);
    CHECK(send_all(fd, fr, fl) == 0);
    CHECK(ws_read(&c, &f, buf, MAXMSG) == 0 && f.op == OP_CLOSE);
    printf("unmasked frame -> close code %d\n", buf[0] << 8 | buf[1]);
    close(fd);
    pthread_join(th, NULL);
    close(s->lfd);
    printf("server: frames=%d messages=%d pings=%d protocol_errors=%d bytes=%zu\n", s->frames, s->messages, s->pings,
           s->protocol_errors, s->bytes);
    free(buf); free(big); free(s);
    return 0;
}
