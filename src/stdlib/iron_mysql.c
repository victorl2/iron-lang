/* iron_mysql.c -- native MySQL/MariaDB client for stdlib/mysql.iron.
 *
 * Implements the MySQL client/server protocol directly over iron_net's TCP
 * sockets and iron_tls's TLS streams: handshake v10, capability
 * negotiation, the SSLRequest upgrade, the mysql_native_password,
 * caching_sha2_password (fast and full paths, RSA key exchange without
 * TLS), sha256_password and mysql_clear_password plugins, auth switch
 * requests, COM_QUERY with text result sets, prepared statements with the
 * binary protocol, multi result sets, packet splitting for payloads of 16
 * MB and more, and a thread-safe connection pool. The hashing and RSA the
 * authentication needs come from libcrypto.
 *
 * Every result is read completely before the call returns and stored in
 * one data buffer plus a cell index (offset, length) per value, so reading
 * a value is a bounds check and a decode, and a connection is free for the
 * next command as soon as a call returns. Each connection keeps its read
 * and write buffers between commands, and a command (with any pending
 * COM_STMT_CLOSE packets) goes out in one write.
 *
 * Errors never panic: each native object carries a MysqlError. Server
 * errors keep the server's code and SQLSTATE; client errors use
 * IRON_ERR_NET_* / IRON_ERR_TLS_* / IRON_ERR_MYSQL_* codes.
 */

#include "iron_mysql.h"
#include "iron_net.h"
#include "iron_tls.h"
#include "runtime/iron_errors.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #ifndef POLLIN
    #define POLLIN  0x0100
  #endif
  #ifndef POLLOUT
    #define POLLOUT 0x0010
  #endif
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <poll.h>
  #include <unistd.h>
#endif

#ifdef IRON_HAVE_OPENSSL
  #include <openssl/bio.h>
  #include <openssl/err.h>
  #include <openssl/evp.h>
  #include <openssl/pem.h>
  #include <openssl/rsa.h>
#endif

/* ── Protocol constants ─────────────────────────────────────────────────── */

#define CLIENT_LONG_PASSWORD                  0x00000001u
#define CLIENT_FOUND_ROWS                     0x00000002u
#define CLIENT_LONG_FLAG                      0x00000004u
#define CLIENT_CONNECT_WITH_DB                0x00000008u
#define CLIENT_LOCAL_FILES                    0x00000080u
#define CLIENT_PROTOCOL_41                    0x00000200u
#define CLIENT_SSL                            0x00000800u
#define CLIENT_TRANSACTIONS                   0x00002000u
#define CLIENT_SECURE_CONNECTION              0x00008000u
#define CLIENT_MULTI_STATEMENTS               0x00010000u
#define CLIENT_MULTI_RESULTS                  0x00020000u
#define CLIENT_PS_MULTI_RESULTS               0x00040000u
#define CLIENT_PLUGIN_AUTH                    0x00080000u
#define CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA 0x00200000u
#define CLIENT_DEPRECATE_EOF                  0x01000000u

#define SERVER_STATUS_IN_TRANS             0x0001
#define SERVER_MORE_RESULTS_EXISTS         0x0008
#define SERVER_STATUS_NO_BACKSLASH_ESCAPES 0x0200

#define COM_QUIT             0x01
#define COM_INIT_DB          0x02
#define COM_QUERY            0x03
#define COM_PING             0x0e
#define COM_STMT_PREPARE     0x16
#define COM_STMT_EXECUTE     0x17
#define COM_STMT_CLOSE       0x19
#define COM_STMT_RESET       0x1a
#define COM_RESET_CONNECTION 0x1f

enum {
    T_DECIMAL = 0, T_TINY = 1, T_SHORT = 2, T_LONG = 3, T_FLOAT = 4,
    T_DOUBLE = 5, T_NULL = 6, T_TIMESTAMP = 7, T_LONGLONG = 8, T_INT24 = 9,
    T_DATE = 10, T_TIME = 11, T_DATETIME = 12, T_YEAR = 13, T_NEWDATE = 14,
    T_VARCHAR = 15, T_BIT = 16, T_TIMESTAMP2 = 17, T_DATETIME2 = 18,
    T_TIME2 = 19, T_VECTOR = 242, T_JSON = 245, T_NEWDECIMAL = 246,
    T_ENUM = 247, T_SET = 248, T_TINY_BLOB = 249, T_MEDIUM_BLOB = 250,
    T_LONG_BLOB = 251, T_BLOB = 252, T_VAR_STRING = 253, T_STRING = 254,
    T_GEOMETRY = 255
};

#define FLAG_UNSIGNED 32
#define COLLATION_BINARY 63

#define MAX_PACKET_CHUNK 0xFFFFFFu
#define CELL_NULL 0xFFFFFFFFu
#define STMT_CACHE_MAX 256
#define ERRMSG_MAX 512

/* ── Small utilities ────────────────────────────────────────────────────── */

typedef struct MyBuf {
    uint8_t *p;
    size_t len;
    size_t cap;
} MyBuf;

static int buf_reserve(MyBuf *b, size_t extra) {
    if (extra > SIZE_MAX - b->len) return 0;
    size_t need = b->len + extra;
    if (need <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    uint8_t *p = (uint8_t *)realloc(b->p, cap);
    if (!p) return 0;
    b->p = p;
    b->cap = cap;
    return 1;
}

static void buf_free(MyBuf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static int buf_put(MyBuf *b, const void *data, size_t n) {
    if (!buf_reserve(b, n)) return 0;
    if (n) memcpy(b->p + b->len, data, n);
    b->len += n;
    return 1;
}

static int buf_u8(MyBuf *b, uint8_t v) { return buf_put(b, &v, 1); }

static int buf_u16(MyBuf *b, uint16_t v) {
    uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return buf_put(b, x, 2);
}

static int buf_u32(MyBuf *b, uint32_t v) {
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 24) };
    return buf_put(b, x, 4);
}

static int buf_u64(MyBuf *b, uint64_t v) {
    uint8_t x[8];
    for (int i = 0; i < 8; i++) x[i] = (uint8_t)(v >> (8 * i));
    return buf_put(b, x, 8);
}

static int buf_lenenc(MyBuf *b, uint64_t v) {
    if (v < 251) return buf_u8(b, (uint8_t)v);
    if (v < 0x10000) return buf_u8(b, 0xfc) && buf_u16(b, (uint16_t)v);
    if (v < 0x1000000) {
        uint8_t x[4] = { 0xfd, (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16) };
        return buf_put(b, x, 4);
    }
    return buf_u8(b, 0xfe) && buf_u64(b, v);
}

static int buf_lenstr(MyBuf *b, const void *s, size_t n) {
    return buf_lenenc(b, n) && buf_put(b, s, n);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* A cursor over a packet payload; any overrun sets `bad`. */
typedef struct Rd {
    const uint8_t *p;
    size_t n;
    size_t i;
    int bad;
} Rd;

static int rd_need(Rd *r, size_t k) {
    if (r->bad || k > r->n - r->i) { r->bad = 1; return 0; }
    return 1;
}
static uint8_t rd_u8(Rd *r) { return rd_need(r, 1) ? r->p[r->i++] : 0; }
static uint16_t rd_u16(Rd *r) {
    if (!rd_need(r, 2)) return 0;
    uint16_t v = rd16(r->p + r->i);
    r->i += 2;
    return v;
}
static uint32_t rd_u32(Rd *r) {
    if (!rd_need(r, 4)) return 0;
    uint32_t v = rd32(r->p + r->i);
    r->i += 4;
    return v;
}
static void rd_skip(Rd *r, size_t k) { if (rd_need(r, k)) r->i += k; }

/* Length-encoded integer. *is_null is set for 0xfb. */
static uint64_t rd_lenenc(Rd *r, int *is_null) {
    if (is_null) *is_null = 0;
    uint8_t c = rd_u8(r);
    if (c < 0xfb) return c;
    if (c == 0xfb) { if (is_null) *is_null = 1; return 0; }
    if (c == 0xfc) return rd_u16(r);
    if (c == 0xfd) {
        if (!rd_need(r, 3)) return 0;
        uint64_t v = rd24(r->p + r->i);
        r->i += 3;
        return v;
    }
    if (c == 0xfe) {
        if (!rd_need(r, 8)) return 0;
        uint64_t v = rd64(r->p + r->i);
        r->i += 8;
        return v;
    }
    r->bad = 1;
    return 0;
}

/* Length-encoded string: returns a pointer into the packet. */
static const uint8_t *rd_lenstr(Rd *r, size_t *len) {
    uint64_t n = rd_lenenc(r, NULL);
    *len = 0;
    if (r->bad || n > r->n - r->i) { r->bad = 1; return NULL; }
    const uint8_t *s = r->p + r->i;
    r->i += (size_t)n;
    *len = (size_t)n;
    return s;
}

/* NUL-terminated string. */
static const uint8_t *rd_cstr(Rd *r, size_t *len) {
    if (r->bad) return NULL;
    const uint8_t *s = r->p + r->i;
    const uint8_t *z = (const uint8_t *)memchr(s, 0, r->n - r->i);
    if (!z) { r->bad = 1; return NULL; }
    *len = (size_t)(z - s);
    r->i += *len + 1;
    return s;
}

static char *dup_bytes(const void *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    if (n) memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static Iron_String str_of(const void *s, size_t n) {
    return iron_string_from_cstr(s ? (const char *)s : "", s ? n : 0);
}

static Iron_String str_c(const char *s) {
    return iron_string_from_cstr(s ? s : "", s ? strlen(s) : 0);
}

static int ascii_ieq(const char *a, size_t an, const char *b, size_t bn) {
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static uint64_t hash_bytes(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* ── Errors ─────────────────────────────────────────────────────────────── */

typedef struct MyErr {
    int64_t code;
    char sqlstate[6];
    bool server;
    char message[ERRMSG_MAX];
} MyErr;

static const char *client_message(int64_t code) {
    switch (code) {
        case IRON_ERR_MYSQL_PROTOCOL: return "malformed or unexpected packet from the server";
        case IRON_ERR_MYSQL_AUTH: return "authentication failed";
        case IRON_ERR_MYSQL_INVALID_ARGUMENT: return "invalid argument";
        case IRON_ERR_MYSQL_PACKET_TOO_LARGE: return "packet exceeds max_allowed_packet";
        case IRON_ERR_MYSQL_CLOSED: return "connection is closed";
        case IRON_ERR_MYSQL_NULL: return "value is NULL";
        case IRON_ERR_MYSQL_CONVERSION: return "value does not convert to the requested type";
        case IRON_ERR_MYSQL_RANGE: return "row or column out of range";
        case IRON_ERR_MYSQL_TLS_REQUIRED: return "TLS required but the server does not support it";
        case IRON_ERR_MYSQL_POOL_TIMEOUT: return "timed out waiting for a pooled connection";
        case IRON_ERR_MYSQL_POOL_CLOSED: return "connection pool is closed";
        case IRON_ERR_MYSQL_PARAM_COUNT: return "parameter count does not match the placeholders";
        case IRON_ERR_MYSQL_BAD_URL: return "invalid MySQL URL";
        case IRON_ERR_MYSQL_INSECURE_AUTH: return "the server requires the password to be sent securely: use TLS, set server_public_key, or allow_public_key_retrieval";
        case IRON_ERR_MYSQL_NO_MEMORY: return "out of memory";
        case IRON_ERR_MYSQL_STMT_CLOSED: return "statement is closed or its connection was released";
        case IRON_ERR_MYSQL_RESULT_TOO_LARGE: return "result set too large";
        case IRON_ERR_MYSQL_LOCAL_INFILE: return "LOAD DATA LOCAL INFILE is not supported";
        case IRON_ERR_MYSQL_UNAVAILABLE: return "the MySQL client was built without OpenSSL";
        case IRON_ERR_TLS_UNAVAILABLE: return "TLS backend is unavailable";
        case IRON_ERR_TLS_CONTEXT: return "could not create TLS context";
        case IRON_ERR_TLS_TRUST_STORE: return "could not load TLS trust roots";
        case IRON_ERR_TLS_HANDSHAKE: return "TLS handshake failed";
        case IRON_ERR_TLS_VERIFY: return "TLS certificate or hostname verification failed";
        case IRON_ERR_TLS_CLOSED: return "TLS connection closed";
        case IRON_ERR_TLS_IO: return "TLS input/output error";
        case IRON_ERR_NET_CONN_REFUSED: return "connection refused";
        case IRON_ERR_NET_CONN_RESET: return "connection reset by the server";
        case IRON_ERR_NET_CONN_ABORTED: return "connection aborted";
        case IRON_ERR_NET_TIMEOUT: return "network timeout";
        case IRON_ERR_NET_UNREACHABLE: return "network unreachable";
        case IRON_ERR_NET_ADDR_NOT_AVAIL: return "host not found or address not available";
        case IRON_ERR_NET_BAD_HOST: return "host not found";
        case IRON_ERR_NET_CLOSED: return "lost connection to the server";
        default: return "network error";
    }
}

static void err_clear(MyErr *e) {
    e->code = 0;
    e->sqlstate[0] = '\0';
    e->server = false;
    e->message[0] = '\0';
}

static void err_setf(MyErr *e, int64_t code, const char *fmt, ...) {
    e->code = code;
    e->server = false;
    const char *state = "HY000";
    if ((code >= 1000 && code < 1100) || (code >= 3000 && code < 3100) ||
        code == IRON_ERR_MYSQL_CLOSED || code == IRON_ERR_MYSQL_PROTOCOL)
        state = "08S01";
    else if (code == IRON_ERR_MYSQL_AUTH || code == IRON_ERR_MYSQL_INSECURE_AUTH)
        state = "28000";
    else if (code == IRON_ERR_MYSQL_NO_MEMORY)
        state = "HY001";
    memcpy(e->sqlstate, state, 6);
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->message, sizeof(e->message), fmt, ap);
        va_end(ap);
    } else {
        snprintf(e->message, sizeof(e->message), "%s", client_message(code));
    }
}

static void err_set(MyErr *e, int64_t code) { err_setf(e, code, NULL); }

static Iron_MysqlError err_to_iron(const MyErr *e) {
    Iron_MysqlError out;
    memset(&out, 0, sizeof(out));
    out.code = e ? e->code : 0;
    if (e && e->code) {
        out.sqlstate = str_c(e->sqlstate);
        out.message = str_c(e->message);
        out.server = e->server;
    }
    return out;
}

static Iron_MysqlError iron_err_code(int64_t code) {
    MyErr e;
    err_set(&e, code);
    return err_to_iron(&e);
}

/* Parse an ERR packet payload (starting at 0xff). */
static void err_from_packet(MyErr *e, const uint8_t *p, size_t n, uint32_t caps) {
    e->server = true;
    e->code = n >= 3 ? rd16(p + 1) : 2000;
    memcpy(e->sqlstate, "HY000", 6);
    size_t i = 3;
    if ((caps & CLIENT_PROTOCOL_41) && n >= 9 && p[3] == '#') {
        memcpy(e->sqlstate, p + 4, 5);
        e->sqlstate[5] = '\0';
        i = 9;
    }
    size_t m = n > i ? n - i : 0;
    if (m >= sizeof(e->message)) m = sizeof(e->message) - 1;
    memcpy(e->message, p + i, m);
    e->message[m] = '\0';
}

/* ── Hashing and RSA (libcrypto) ────────────────────────────────────────── */

#ifdef IRON_HAVE_OPENSSL
static int digest(const EVP_MD *md, const void *a, size_t an, const void *b,
                  size_t bn, uint8_t *out) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int len = 0;
    int ok = ctx && EVP_DigestInit_ex(ctx, md, NULL) == 1 &&
             EVP_DigestUpdate(ctx, a, an) == 1 &&
             (bn == 0 || EVP_DigestUpdate(ctx, b, bn) == 1) &&
             EVP_DigestFinal_ex(ctx, out, &len) == 1;
    EVP_MD_CTX_free(ctx);
    return ok;
}
#endif

bool iron_mysql_available(void) {
#ifdef IRON_HAVE_OPENSSL
    return true;
#else
    return false;
#endif
}

/* mysql_native_password: SHA1(pw) XOR SHA1(scramble || SHA1(SHA1(pw))). */
void iron_mysql_scramble_native(const uint8_t *scramble, size_t scramble_len,
                                const char *password, size_t password_len,
                                uint8_t out[20]) {
    memset(out, 0, 20);
#ifdef IRON_HAVE_OPENSSL
    uint8_t h1[20], h2[20], h3[20];
    if (!digest(EVP_sha1(), password, password_len, NULL, 0, h1) ||
        !digest(EVP_sha1(), h1, 20, NULL, 0, h2) ||
        !digest(EVP_sha1(), scramble, scramble_len, h2, 20, h3))
        return;
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(h1[i] ^ h3[i]);
#else
    (void)scramble; (void)scramble_len; (void)password; (void)password_len;
#endif
}

/* caching_sha2_password: SHA256(pw) XOR SHA256(SHA256(SHA256(pw)) || scramble). */
void iron_mysql_scramble_sha256(const uint8_t *scramble, size_t scramble_len,
                                const char *password, size_t password_len,
                                uint8_t out[32]) {
    memset(out, 0, 32);
#ifdef IRON_HAVE_OPENSSL
    uint8_t h1[32], h2[32], h3[32];
    if (!digest(EVP_sha256(), password, password_len, NULL, 0, h1) ||
        !digest(EVP_sha256(), h1, 32, NULL, 0, h2) ||
        !digest(EVP_sha256(), h2, 32, scramble, scramble_len, h3))
        return;
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(h1[i] ^ h3[i]);
#else
    (void)scramble; (void)scramble_len; (void)password; (void)password_len;
#endif
}

/* RSA-OAEP encryption of (password || NUL) XOR scramble with the server's
 * public key, as caching_sha2_password and sha256_password expect. */
static int rsa_encrypt_password(const char *pem, size_t pem_len,
                                const uint8_t *scramble, size_t scramble_len,
                                const char *password, size_t password_len,
                                MyBuf *out) {
#ifdef IRON_HAVE_OPENSSL
    int ok = 0;
    size_t plain_len = password_len + 1;
    uint8_t *plain = (uint8_t *)malloc(plain_len);
    BIO *bio = BIO_new_mem_buf(pem, (int)pem_len);
    EVP_PKEY *key = bio ? PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL) : NULL;
    EVP_PKEY_CTX *ctx = key ? EVP_PKEY_CTX_new(key, NULL) : NULL;
    if (plain && ctx && scramble_len > 0) {
        memcpy(plain, password, password_len);
        plain[password_len] = 0;
        for (size_t i = 0; i < plain_len; i++) plain[i] ^= scramble[i % scramble_len];
        size_t enc_len = 0;
        if (EVP_PKEY_encrypt_init(ctx) == 1 &&
            EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) == 1 &&
            EVP_PKEY_encrypt(ctx, NULL, &enc_len, plain, plain_len) == 1 &&
            buf_reserve(out, enc_len) &&
            EVP_PKEY_encrypt(ctx, out->p + out->len, &enc_len, plain, plain_len) == 1) {
            out->len += enc_len;
            ok = 1;
        }
    }
    if (plain) { memset(plain, 0, plain_len); free(plain); }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(key);
    BIO_free(bio);
    ERR_clear_error();
    return ok;
#else
    (void)pem; (void)pem_len; (void)scramble; (void)scramble_len;
    (void)password; (void)password_len; (void)out;
    return 0;
#endif
}

/* ── Result storage ─────────────────────────────────────────────────────── */

typedef struct MyCol {
    uint32_t name, name_len;       /* offsets into MyMeta.strings */
    uint32_t org_name, org_name_len;
    uint32_t table, table_len;
    uint32_t org_table, org_table_len;
    uint32_t schema, schema_len;
    uint32_t length;
    uint16_t charset;
    uint16_t flags;
    uint8_t type;
    uint8_t decimals;
} MyCol;

/* Column definitions of a result set. Immutable once built and shared by
 * reference count: a prepared statement keeps the last one it saw and the
 * next execution reuses it when the server sends the same definitions, so
 * a repeated query does not parse or allocate them again. */
typedef struct MyMeta {
    atomic_int refs;
    uint32_t ncols;
    MyCol *cols;
    MyBuf strings;
    MyBuf raw;                     /* the definition packets, length-prefixed */
} MyMeta;

static void meta_unref(MyMeta *m) {
    if (!m || atomic_fetch_sub(&m->refs, 1) != 1) return;
    free(m->cols);
    buf_free(&m->strings);
    buf_free(&m->raw);
    free(m);
}

static MyMeta *meta_ref(MyMeta *m) {
    if (m) atomic_fetch_add(&m->refs, 1);
    return m;
}

/* Small results (a point lookup) live in the set itself. */
#define SET_INLINE_DATA 192
#define SET_INLINE_CELLS 32

typedef struct MySet {
    uint32_t ncols;
    MyCol *cols;                   /* md->cols */
    MyMeta *md;
    uint64_t nrows;
    uint64_t rows_cap;
    uint32_t *cells;               /* 2 * ncols per row: offset, length */
    MyBuf data;
    int binary;
    uint64_t affected;
    uint64_t insert_id;
    uint16_t warnings;
    uint16_t status;
    char *info;
    uint32_t cells_inline[SET_INLINE_CELLS];
    uint8_t data_inline[SET_INLINE_DATA];
} MySet;

/* The first result set is part of the result; further ones (multi
 * statements, procedures) are allocated one by one, so a set never moves. */
typedef struct MyResult {
    MySet first;
    MySet **more;                  /* sets 1.. */
    uint32_t nsets;
    uint32_t more_cap;
    uint32_t cur;
    MyErr err;
} MyResult;

static const uint8_t *set_strings(const MySet *s) {
    return s->md ? s->md->strings.p : NULL;
}

static void set_free(MySet *s) {
    meta_unref(s->md);
    if (s->cells != s->cells_inline) free(s->cells);
    if (s->data.p != s->data_inline) buf_free(&s->data);
    free(s->info);
}

static MySet *res_set(MyResult *r, uint32_t i) {
    return i == 0 ? &r->first : r->more[i - 1];
}

/* Free every set and keep the result itself (empty). */
static void result_clear(MyResult *r) {
    for (uint32_t i = 0; i < r->nsets; i++) {
        MySet *s = res_set(r, i);
        set_free(s);
        if (i > 0) free(s);
    }
    free(r->more);
    r->more = NULL;
    r->more_cap = 0;
    r->nsets = 0;
    r->cur = 0;
}

static void result_destroy(MyResult *r) {
    if (!r) return;
    result_clear(r);
    free(r);
}

static MyResult *result_new(void) {
    MyResult *r = (MyResult *)malloc(sizeof(MyResult));
    if (!r) return NULL;
    memset(r, 0, offsetof(MyResult, err));
    err_clear(&r->err);
    return r;
}

static void set_init(MySet *s) {
    memset(s, 0, offsetof(MySet, cells_inline));
}

static MySet *result_add_set(MyResult *r) {
    if (r->nsets == 0) {
        set_init(&r->first);
        r->nsets = 1;
        return &r->first;
    }
    if (r->nsets - 1 == r->more_cap) {
        uint32_t cap = r->more_cap ? r->more_cap * 2 : 2;
        MySet **m = (MySet **)realloc(r->more, cap * sizeof(MySet *));
        if (!m) return NULL;
        r->more = m;
        r->more_cap = cap;
    }
    MySet *s = (MySet *)malloc(sizeof(MySet));
    if (!s) return NULL;
    set_init(s);
    r->more[r->nsets - 1] = s;
    r->nsets++;
    return s;
}

/* ── Connection ─────────────────────────────────────────────────────────── */

typedef struct MyPool MyPool;

/* A prepared statement on the server. */
typedef struct MyPrep {
    uint32_t id;
    uint16_t nparams;
    uint16_t ncols;
    uint8_t *types;                /* last parameter types sent (2 per param) */
    int types_sent;
    MyMeta *rmeta;                 /* result columns of the last execution */
} MyPrep;

typedef struct MyCacheEnt {
    char *sql;
    size_t len;
    uint64_t hash;
    uint64_t used;
    MyPrep prep;
} MyCacheEnt;

typedef struct MyConn {
    atomic_int refs;               /* owner (Iron handle or pool) + statements */
    int64_t fd;
    Iron_TlsStream *tls;
    int open;
    int broken;
    MyErr err;                     /* connect error, then the last error */
    uint32_t caps;
    uint16_t status;
    uint8_t seq;
    uint8_t collation;
    uint32_t thread_id;
    int mariadb;
    char version[64];
    char plugin[40];
    int64_t timeout_ms;
    Iron_Deadline deadline;
    uint64_t max_packet;           /* largest packet accepted from the server */
    int64_t server_max_packet;     /* server's max_allowed_packet, -1 unknown */
    MyBuf r;                       /* bytes received, consumed up to rpos */
    size_t rpos;
    MyBuf big;                     /* reassembled multi-packet payload */
    MyBuf w;                       /* outgoing packets */
    MyBuf colbuf;                  /* column definitions being read */
    uint64_t cmd_rx;               /* bytes received for the current command */
    int send_failed;               /* the current command's write failed */
    MyCacheEnt *cache;
    int ncache;
    uint64_t cache_clock;
    int cache_mru;                 /* index of the statement used last, or -1 */
    iron_mutex_t pend_lock;
    uint32_t *pend;                /* statement ids waiting for COM_STMT_CLOSE */
    int npend, pend_cap;
    atomic_int pend_count;         /* npend, readable without the lock */
    atomic_uint_fast64_t gen;      /* bumped when a pooled connection is released */
    MyPool *pool;
    uint64_t created_ms;
    uint64_t idle_since_ms;
    char *pubkey;                  /* server RSA public key (PEM) */
    size_t pubkey_len;
    int test_transport;            /* in-process transport for conformance */
    const uint8_t *test_in;
    size_t test_in_len;
    size_t test_in_pos;
    size_t test_read_size;
    MyBuf test_out;
} MyConn;

typedef struct MyStmt {
    MyConn *conn;
    uint64_t gen;
    MyPrep prep;
    int closed;
    MyErr err;
} MyStmt;

static void prep_free(MyPrep *p) {
    free(p->types);
    p->types = NULL;
    meta_unref(p->rmeta);
    p->rmeta = NULL;
}

static void conn_destroy(MyConn *c) {
    if (c->tls) iron_tls_stream_close(c->tls);
    if (c->fd >= 0) {
        Iron_TcpSocket s = { c->fd };
        Iron_tcpsocket_close(s);
    }
    buf_free(&c->r);
    buf_free(&c->big);
    buf_free(&c->w);
    buf_free(&c->colbuf);
    for (int i = 0; i < c->ncache; i++) {
        free(c->cache[i].sql);
        prep_free(&c->cache[i].prep);
    }
    free(c->cache);
    free(c->pend);
    iron_mutex_destroy(&c->pend_lock);
    if (c->pubkey) free(c->pubkey);
    buf_free(&c->test_out);
    free(c);
}

static void conn_unref(MyConn *c) {
    if (c && atomic_fetch_sub(&c->refs, 1) == 1) conn_destroy(c);
}

static MyConn *conn_new(void) {
    MyConn *c = (MyConn *)calloc(1, sizeof(MyConn));
    if (!c) return NULL;
    atomic_init(&c->refs, 1);
    atomic_init(&c->gen, 1);
    c->fd = -1;
    c->server_max_packet = -1;
    c->max_packet = 1073741824;
    c->cache_mru = -1;
    iron_mutex_init(&c->pend_lock);
    c->created_ms = Iron_monotonic_now_ms();
    return c;
}

/* Close the transport. The struct stays valid until its last reference. */
static void conn_shutdown(MyConn *c, int send_quit) {
    if (!c->open && c->fd < 0) return;
    if (send_quit && c->open && !c->broken) {
        uint8_t quit[5] = { 1, 0, 0, 0, COM_QUIT };
        Iron_Deadline dl = Iron_deadline_from_timeout_ms(1000);
        if (c->tls) {
            (void)iron_tls_write(c->tls, quit, 5, dl);
        } else {
#ifdef _WIN32
            (void)send((SOCKET)c->fd, (const char *)quit, 5, 0);
#else
            (void)send((int)c->fd, quit, 5, 0);
#endif
        }
    }
    if (c->tls) { iron_tls_stream_close(c->tls); c->tls = NULL; }
    if (c->fd >= 0) {
        Iron_TcpSocket s = { c->fd };
        Iron_tcpsocket_close(s);
        c->fd = -1;
    }
    c->open = 0;
}

/* ── Transport ──────────────────────────────────────────────────────────── */

static Iron_Deadline deadline_after(int64_t ms) {
    /* 0 means no limit: a deadline a year away (remaining time clamps to
     * INT_MAX milliseconds per wait). */
    if (ms <= 0) ms = 365LL * 24 * 3600 * 1000;
    return Iron_deadline_from_timeout_ms(ms);
}

static int net_errno_code(int e) {
#ifdef _WIN32
    switch (e) {
        case WSAECONNRESET: return IRON_ERR_NET_CONN_RESET;
        case WSAECONNABORTED: return IRON_ERR_NET_CONN_ABORTED;
        case WSAETIMEDOUT: return IRON_ERR_NET_TIMEOUT;
        case WSAENOTCONN: case WSAESHUTDOWN: return IRON_ERR_NET_CLOSED;
        case WSAENETUNREACH: case WSAEHOSTUNREACH: return IRON_ERR_NET_UNREACHABLE;
        default: return IRON_ERR_NET_UNKNOWN;
    }
#else
    switch (e) {
        case ECONNRESET: case EPIPE: return IRON_ERR_NET_CONN_RESET;
        case ECONNABORTED: return IRON_ERR_NET_CONN_ABORTED;
        case ETIMEDOUT: return IRON_ERR_NET_TIMEOUT;
        case ENOTCONN: return IRON_ERR_NET_CLOSED;
        case ENETUNREACH: case EHOSTUNREACH: return IRON_ERR_NET_UNREACHABLE;
        default: return IRON_ERR_NET_UNKNOWN;
    }
#endif
}

static int sock_wait(int64_t fd, int for_write, Iron_Deadline dl) {
    for (;;) {
        int remaining = Iron_deadline_remaining_ms(dl);
        if (remaining <= 0) return IRON_ERR_NET_TIMEOUT;
#ifdef _WIN32
        WSAPOLLFD p;
        p.fd = (SOCKET)fd;
        p.events = for_write ? POLLOUT : POLLIN;
        p.revents = 0;
        int rc = WSAPoll(&p, 1, remaining);
        if (rc == SOCKET_ERROR) return net_errno_code(WSAGetLastError());
#else
        struct pollfd p;
        p.fd = (int)fd;
        p.events = for_write ? POLLOUT : POLLIN;
        p.revents = 0;
        int rc = poll(&p, 1, remaining);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return net_errno_code(errno);
        }
#endif
        if (rc == 0) return IRON_ERR_NET_TIMEOUT;
        return 0;
    }
}

/* Fail the connection: it cannot be used after a transport or protocol
 * error because the packet stream is out of step. */
static int conn_fail(MyConn *c, MyErr *e, int64_t code) {
    c->broken = 1;
    if (e) err_set(e, code);
    return (int)code;
}

static int conn_write(MyConn *c, const uint8_t *p, size_t n) {
    if (c->test_transport)
        return buf_put(&c->test_out, p, n) ? 0 : IRON_ERR_MYSQL_NO_MEMORY;
    while (n > 0) {
        if (c->tls) {
            int64_t chunk = n > (size_t)INT_MAX ? INT_MAX : (int64_t)n;
            Iron_Result_Int_Error w = iron_tls_write(c->tls, p, chunk, c->deadline);
            if (w.v1.code != 0) return (int)w.v1.code;
            if (w.v0 <= 0) return IRON_ERR_TLS_CLOSED;
            p += w.v0;
            n -= (size_t)w.v0;
            continue;
        }
#ifdef _WIN32
        int chunk = n > (size_t)INT_MAX ? INT_MAX : (int)n;
        int k = send((SOCKET)c->fd, (const char *)p, chunk, 0);
        if (k == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
                int rc = sock_wait(c->fd, 1, c->deadline);
                if (rc) return rc;
                continue;
            }
            return net_errno_code(e);
        }
#else
        ssize_t k = send((int)c->fd, p, n, 0);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int rc = sock_wait(c->fd, 1, c->deadline);
                if (rc) return rc;
                continue;
            }
            return net_errno_code(errno);
        }
#endif
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* Read at least one byte into p[0..cap). Returns the count, or minus an
 * error code (IRON_ERR_NET_CLOSED when the server closed the connection). */
static int64_t conn_read_some(MyConn *c, uint8_t *p, size_t cap) {
    if (cap > (size_t)INT_MAX) cap = INT_MAX;
    if (c->test_transport) {
        size_t left = c->test_in_len - c->test_in_pos;
        if (left == 0) return -IRON_ERR_NET_CLOSED;
        if (cap > left) cap = left;
        if (c->test_read_size && cap > c->test_read_size) cap = c->test_read_size;
        memcpy(p, c->test_in + c->test_in_pos, cap);
        c->test_in_pos += cap;
        c->cmd_rx += cap;
        return (int64_t)cap;
    }
    for (;;) {
        if (c->tls) {
            Iron_Result_Int_Error rd = iron_tls_read(c->tls, p, (int64_t)cap, c->deadline);
            if (rd.v1.code != 0) return -rd.v1.code;
            if (rd.v0 == 0) return -IRON_ERR_NET_CLOSED;
            c->cmd_rx += (uint64_t)rd.v0;
            return rd.v0;
        }
#ifdef _WIN32
        int k = recv((SOCKET)c->fd, (char *)p, (int)cap, 0);
        if (k == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
                int rc = sock_wait(c->fd, 0, c->deadline);
                if (rc) return -rc;
                continue;
            }
            return -net_errno_code(e);
        }
#else
        ssize_t k = recv((int)c->fd, p, cap, 0);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int rc = sock_wait(c->fd, 0, c->deadline);
                if (rc) return -rc;
                continue;
            }
            return -net_errno_code(errno);
        }
#endif
        if (k == 0) return -IRON_ERR_NET_CLOSED;
        c->cmd_rx += (uint64_t)k;
        return k;
    }
}

/* Make at least `need` unconsumed bytes available in c->r. */
static int conn_fill(MyConn *c, size_t need) {
    while (c->r.len - c->rpos < need) {
        size_t have = c->r.len - c->rpos;
        if (c->rpos > 0 && (c->r.cap - c->r.len < need - have || c->rpos > c->r.cap / 2)) {
            memmove(c->r.p, c->r.p + c->rpos, have);
            c->r.len = have;
            c->rpos = 0;
        }
        size_t want = need - have;
        if (want < 16384) want = 16384;
        if (c->r.cap - c->r.len < want && !buf_reserve(&c->r, want))
            return IRON_ERR_MYSQL_NO_MEMORY;
        int64_t k = conn_read_some(c, c->r.p + c->r.len, c->r.cap - c->r.len);
        if (k < 0) return (int)-k;
        c->r.len += (size_t)k;
    }
    return 0;
}

/* Read one logical packet (reassembling 16 MB chunks). The payload pointer
 * stays valid until the next read. */
static int conn_read_packet(MyConn *c, const uint8_t **out, size_t *out_len, MyErr *e) {
    size_t total = 0;
    int multi = 0;
    for (;;) {
        int rc = conn_fill(c, 4);
        if (rc) return conn_fail(c, e, rc);
        const uint8_t *h = c->r.p + c->rpos;
        size_t len = rd24(h);
        uint8_t seq = h[3];
        if (seq != c->seq) {
            c->broken = 1;
            if (e) err_setf(e, IRON_ERR_MYSQL_PROTOCOL,
                            "packet out of order (expected %u, got %u)", c->seq, seq);
            return IRON_ERR_MYSQL_PROTOCOL;
        }
        c->seq++;
        if ((uint64_t)total + len > c->max_packet) {
            c->broken = 1;
            if (e) err_setf(e, IRON_ERR_MYSQL_PACKET_TOO_LARGE,
                            "packet from the server exceeds max_allowed_packet (%llu bytes)",
                            (unsigned long long)c->max_packet);
            return IRON_ERR_MYSQL_PACKET_TOO_LARGE;
        }
        rc = conn_fill(c, 4 + len);
        if (rc) return conn_fail(c, e, rc);
        const uint8_t *payload = c->r.p + c->rpos + 4;
        if (!multi && len < MAX_PACKET_CHUNK) {
            c->rpos += 4 + len;
            *out = payload;
            *out_len = len;
            return 0;
        }
        if (!multi) { c->big.len = 0; multi = 1; }
        if (!buf_put(&c->big, payload, len)) return conn_fail(c, e, IRON_ERR_MYSQL_NO_MEMORY);
        c->rpos += 4 + len;
        total += len;
        if (len < MAX_PACKET_CHUNK) break;
    }
    *out = c->big.p;
    *out_len = c->big.len;
    return 0;
}

/* Start an outgoing packet in c->w: reserve its 4-byte header. Returns the
 * header offset. */
static size_t pkt_begin(MyConn *c) {
    size_t at = c->w.len;
    if (buf_reserve(&c->w, 4)) c->w.len += 4;
    return at;
}

/* Send every queued packet. The last one starts at `at`; its payload may
 * be any size and is split into 16 MB chunks as the protocol requires. */
static int pkt_send(MyConn *c, size_t at, MyErr *e) {
    size_t payload = c->w.len - at - 4;
    int rc;
    if (payload < MAX_PACKET_CHUNK) {
        c->w.p[at] = (uint8_t)payload;
        c->w.p[at + 1] = (uint8_t)(payload >> 8);
        c->w.p[at + 2] = (uint8_t)(payload >> 16);
        c->w.p[at + 3] = c->seq++;
        rc = conn_write(c, c->w.p, c->w.len);
    } else {
        rc = at ? conn_write(c, c->w.p, at) : 0;
        size_t off = at + 4;
        size_t left = payload;
        while (!rc) {
            size_t chunk = left < MAX_PACKET_CHUNK ? left : MAX_PACKET_CHUNK;
            uint8_t h[4] = { (uint8_t)chunk, (uint8_t)(chunk >> 8),
                             (uint8_t)(chunk >> 16), c->seq++ };
            rc = conn_write(c, h, 4);
            if (!rc && chunk) rc = conn_write(c, c->w.p + off, chunk);
            off += chunk;
            left -= chunk;
            if (chunk < MAX_PACKET_CHUNK) break;
        }
    }
    c->w.len = 0;
    if (c->w.cap > (4u << 20)) {   /* do not keep a huge buffer around */
        buf_free(&c->w);
    }
    if (rc) {
        c->send_failed = 1;
        return conn_fail(c, e, rc);
    }
    return 0;
}

/* Begin a command: check the connection, start its deadline, and queue
 * COM_STMT_CLOSE packets for statements released since the last command
 * (they have no response, so they ride along with this write). Returns the
 * header offset of the command packet. */
static int cmd_begin(MyConn *c, MyErr *e, size_t *at) {
    if (!c->open || c->broken) {
        err_set(e, IRON_ERR_MYSQL_CLOSED);
        return IRON_ERR_MYSQL_CLOSED;
    }
    c->deadline = deadline_after(c->timeout_ms);
    c->cmd_rx = 0;
    c->send_failed = 0;
    if (c->rpos == c->r.len) {
        c->rpos = c->r.len = 0;
        if (c->r.cap > (4u << 20)) buf_free(&c->r);
    }
    if (c->big.cap > (4u << 20)) buf_free(&c->big);
    c->w.len = 0;
    if (atomic_load_explicit(&c->pend_count, memory_order_acquire) > 0) {
    iron_mutex_lock(&c->pend_lock);
    for (int i = 0; i < c->npend; i++) {
        uint8_t close_pkt[9] = { 5, 0, 0, 0, COM_STMT_CLOSE, 0, 0, 0, 0 };
        uint32_t id = c->pend[i];
        close_pkt[5] = (uint8_t)id;
        close_pkt[6] = (uint8_t)(id >> 8);
        close_pkt[7] = (uint8_t)(id >> 16);
        close_pkt[8] = (uint8_t)(id >> 24);
        buf_put(&c->w, close_pkt, 9);
    }
    c->npend = 0;
    atomic_store_explicit(&c->pend_count, 0, memory_order_release);
    iron_mutex_unlock(&c->pend_lock);
    }
    c->seq = 0;
    *at = pkt_begin(c);
    if (c->w.len < *at + 4) {
        err_set(e, IRON_ERR_MYSQL_NO_MEMORY);
        return IRON_ERR_MYSQL_NO_MEMORY;
    }
    return 0;
}

/* Parse an OK packet (0x00 or 0xfe header) into the set's summary and the
 * connection status. */
static int parse_ok(MyConn *c, const uint8_t *p, size_t n, MySet *s) {
    Rd r = { p, n, 1, 0 };
    uint64_t affected = rd_lenenc(&r, NULL);
    uint64_t insert_id = rd_lenenc(&r, NULL);
    uint16_t status = 0, warnings = 0;
    if (c->caps & CLIENT_PROTOCOL_41) {
        status = rd_u16(&r);
        warnings = rd_u16(&r);
    }
    if (r.bad) return 0;
    c->status = status;
    if (s) {
        s->affected = affected;
        s->insert_id = insert_id;
        s->status = status;
        s->warnings = warnings;
        if (r.i < n) {
            free(s->info);
            s->info = dup_bytes(p + r.i, n - r.i);
        }
    }
    return 1;
}

static int is_eof_packet(const uint8_t *p, size_t n) {
    return n > 0 && p[0] == 0xfe && n < 9;
}

static int is_terminator(MyConn *c, const uint8_t *p, size_t n) {
    if (n == 0 || p[0] != 0xfe) return 0;
    if (c->caps & CLIENT_DEPRECATE_EOF) return n < MAX_PACKET_CHUNK;
    return n < 9;
}

/* EOF packet (0xfe warnings status) or, with CLIENT_DEPRECATE_EOF, an OK
 * packet with a 0xfe header. */
static int parse_terminator(MyConn *c, const uint8_t *p, size_t n, MySet *s) {
    if ((c->caps & CLIENT_DEPRECATE_EOF)) return parse_ok(c, p, n, s);
    if (n >= 5) {
        uint16_t warnings = rd16(p + 1);
        c->status = rd16(p + 3);
        if (s) { s->warnings = warnings; s->status = c->status; }
    }
    return 1;
}

/* ── Handshake and authentication ───────────────────────────────────────── */

typedef struct MyCfg {
    char *host;
    int64_t port;
    char *user;
    char *password;
    size_t password_len;
    char *database;
    int tls;
    char *ca_file;
    char *server_public_key;
    int allow_pubkey_retrieval;
    int allow_cleartext;
    char *collation;
    int64_t connect_timeout;
    int64_t timeout;
    int multi_statements;
    int64_t max_packet;
} MyCfg;

static void cfg_free(MyCfg *k) {
    free(k->host);
    free(k->user);
    if (k->password) { memset(k->password, 0, k->password_len); free(k->password); }
    free(k->database);
    free(k->ca_file);
    free(k->server_public_key);
    free(k->collation);
    memset(k, 0, sizeof(*k));
}

static char *istr_dup(Iron_String s) {
    return dup_bytes(iron_string_cstr(&s), iron_string_byte_len(&s));
}

static int cfg_from_iron(MyCfg *k, const Iron_MysqlConfig *c) {
    memset(k, 0, sizeof(*k));
    k->host = istr_dup(c->host);
    k->port = c->port;
    k->user = istr_dup(c->user);
    k->password = istr_dup(c->password);
    k->password_len = iron_string_byte_len(&c->password);
    k->database = istr_dup(c->database);
    k->tls = c->tls;
    k->ca_file = istr_dup(c->ca_file);
    k->server_public_key = istr_dup(c->server_public_key);
    k->allow_pubkey_retrieval = c->allow_public_key_retrieval;
    k->allow_cleartext = c->allow_cleartext_password;
    k->collation = istr_dup(c->collation);
    k->connect_timeout = c->connect_timeout;
    k->timeout = c->timeout;
    k->multi_statements = c->multi_statements;
    k->max_packet = c->max_allowed_packet;
    if (!k->host || !k->user || !k->password || !k->database || !k->ca_file ||
        !k->server_public_key || !k->collation) {
        cfg_free(k);
        return 0;
    }
    return 1;
}

static const struct { const char *name; uint8_t id; } k_collations[] = {
    { "utf8mb4_general_ci", 45 }, { "utf8mb4_bin", 46 },
    { "utf8mb4_unicode_ci", 224 }, { "utf8mb4_unicode_520_ci", 246 },
    { "utf8mb4_0900_ai_ci", 255 }, { "utf8_general_ci", 33 },
    { "utf8mb3_general_ci", 33 }, { "utf8_bin", 83 }, { "utf8_unicode_ci", 192 },
    { "latin1_swedish_ci", 8 }, { "latin1_bin", 47 }, { "ascii_general_ci", 11 },
    { "binary", 63 },
};

static int collation_id(const char *name) {
    for (size_t i = 0; i < sizeof(k_collations) / sizeof(k_collations[0]); i++)
        if (ascii_ieq(name, strlen(name), k_collations[i].name, strlen(k_collations[i].name)))
            return k_collations[i].id;
    return -1;
}

static int read_file_bytes(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    MyBuf b = { 0 };
    char chunk[4096];
    size_t k;
    while ((k = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (!buf_put(&b, chunk, k) || b.len > (1u << 20)) { buf_free(&b); fclose(f); return 0; }
    }
    fclose(f);
    if (!buf_u8(&b, 0)) { buf_free(&b); return 0; }
    *out = (char *)b.p;
    *out_len = b.len - 1;
    return 1;
}

static int write_auth_packet(MyConn *c, const void *data, size_t n, MyErr *e) {
    c->w.len = 0;
    size_t at = pkt_begin(c);
    if (!buf_put(&c->w, data, n)) return conn_fail(c, e, IRON_ERR_MYSQL_NO_MEMORY);
    return pkt_send(c, at, e);
}

static int plugin_is(const char *plugin, const char *name) {
    return strcmp(plugin, name) == 0;
}

/* The first response for a plugin. *pending_key is set when the response
 * asks the server for its public key. */
static int auth_response(MyConn *c, const MyCfg *k, const char *plugin,
                         const uint8_t *scramble, size_t slen, MyBuf *out, MyErr *e) {
    out->len = 0;
    if (plugin_is(plugin, "mysql_native_password")) {
        if (k->password_len == 0) return 0;
        uint8_t h[20];
        iron_mysql_scramble_native(scramble, slen, k->password, k->password_len, h);
        return buf_put(out, h, 20) ? 0 : IRON_ERR_MYSQL_NO_MEMORY;
    }
    if (plugin_is(plugin, "caching_sha2_password")) {
        if (k->password_len == 0) return 0;
        uint8_t h[32];
        iron_mysql_scramble_sha256(scramble, slen, k->password, k->password_len, h);
        return buf_put(out, h, 32) ? 0 : IRON_ERR_MYSQL_NO_MEMORY;
    }
    if (plugin_is(plugin, "sha256_password")) {
        if (k->password_len == 0) return buf_u8(out, 0) ? 0 : IRON_ERR_MYSQL_NO_MEMORY;
        if (c->tls) {
            return buf_put(out, k->password, k->password_len) && buf_u8(out, 0)
                ? 0 : IRON_ERR_MYSQL_NO_MEMORY;
        }
        if (c->pubkey) {
            if (!rsa_encrypt_password(c->pubkey, c->pubkey_len, scramble, slen,
                                      k->password, k->password_len, out)) {
                err_setf(e, IRON_ERR_MYSQL_AUTH, "could not encrypt the password with the server's RSA public key");
                return IRON_ERR_MYSQL_AUTH;
            }
            return 0;
        }
        if (k->allow_pubkey_retrieval) return buf_u8(out, 1) ? 0 : IRON_ERR_MYSQL_NO_MEMORY;
        err_set(e, IRON_ERR_MYSQL_INSECURE_AUTH);
        return IRON_ERR_MYSQL_INSECURE_AUTH;
    }
    if (plugin_is(plugin, "mysql_clear_password")) {
        if (!c->tls && !k->allow_cleartext) {
            err_setf(e, IRON_ERR_MYSQL_INSECURE_AUTH,
                     "mysql_clear_password needs TLS or allow_cleartext_password");
            return IRON_ERR_MYSQL_INSECURE_AUTH;
        }
        return buf_put(out, k->password, k->password_len) && buf_u8(out, 0)
            ? 0 : IRON_ERR_MYSQL_NO_MEMORY;
    }
    err_setf(e, IRON_ERR_MYSQL_AUTH, "unsupported authentication plugin '%s'", plugin);
    return IRON_ERR_MYSQL_AUTH;
}

static int send_rsa_password(MyConn *c, const MyCfg *k, const uint8_t *scramble,
                             size_t slen, MyErr *e) {
    MyBuf enc = { 0 };
    if (!rsa_encrypt_password(c->pubkey, c->pubkey_len, scramble, slen,
                              k->password, k->password_len, &enc)) {
        buf_free(&enc);
        err_setf(e, IRON_ERR_MYSQL_AUTH, "could not encrypt the password with the server's RSA public key");
        return IRON_ERR_MYSQL_AUTH;
    }
    int rc = write_auth_packet(c, enc.p, enc.len, e);
    buf_free(&enc);
    return rc;
}

static int set_pubkey(MyConn *c, const uint8_t *pem, size_t n) {
    char *k = dup_bytes(pem, n);
    if (!k) return 0;
    free(c->pubkey);
    c->pubkey = k;
    c->pubkey_len = n;
    return 1;
}

/* After the handshake response: follow auth switches and the extra rounds
 * of caching_sha2_password / sha256_password until OK or ERR. */
static int auth_loop(MyConn *c, const MyCfg *k, uint8_t *scramble, size_t *slen, MyErr *e) {
    MyBuf resp = { 0 };
    int rc = 0;
    for (int rounds = 0; rounds < 16 && !rc; rounds++) {
        const uint8_t *p;
        size_t n;
        rc = conn_read_packet(c, &p, &n, e);
        if (rc) break;
        if (n == 0) { rc = conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL); break; }
        if (p[0] == 0x00) {
            if (!parse_ok(c, p, n, NULL)) rc = conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
            buf_free(&resp);
            return rc;
        }
        if (p[0] == 0xff) {
            err_from_packet(e, p, n, c->caps);
            rc = (int)e->code ? (int)e->code : IRON_ERR_MYSQL_AUTH;
            break;
        }
        if (p[0] == 0xfe) {
            /* Auth switch request: plugin name, then the new scramble. */
            if (n == 1) {
                err_setf(e, IRON_ERR_MYSQL_AUTH, "the server asked for the removed mysql_old_password plugin");
                rc = IRON_ERR_MYSQL_AUTH;
                break;
            }
            Rd r = { p, n, 1, 0 };
            size_t plen;
            const uint8_t *pname = rd_cstr(&r, &plen);
            if (r.bad || plen >= sizeof(c->plugin)) { rc = conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL); break; }
            memcpy(c->plugin, pname, plen);
            c->plugin[plen] = '\0';
            size_t dlen = n - r.i;
            if (dlen > 0 && p[n - 1] == 0) dlen--;
            if (dlen > 64) dlen = 64;
            memcpy(scramble, p + r.i, dlen);
            *slen = dlen;
            rc = auth_response(c, k, c->plugin, scramble, *slen, &resp, e);
            if (!rc) rc = write_auth_packet(c, resp.p, resp.len, e);
            continue;
        }
        if (p[0] == 0x01) {
            /* AuthMoreData. */
            if (plugin_is(c->plugin, "caching_sha2_password") && n == 2 && p[1] == 3) {
                continue;   /* fast auth succeeded; OK follows */
            }
            if (plugin_is(c->plugin, "caching_sha2_password") && n == 2 && p[1] == 4) {
                /* Full authentication. */
                if (c->tls) {
                    resp.len = 0;
                    if (!buf_put(&resp, k->password, k->password_len) || !buf_u8(&resp, 0)) {
                        rc = IRON_ERR_MYSQL_NO_MEMORY;
                        err_set(e, rc);
                        break;
                    }
                    rc = write_auth_packet(c, resp.p, resp.len, e);
                } else if (c->pubkey) {
                    rc = send_rsa_password(c, k, scramble, *slen, e);
                } else if (k->allow_pubkey_retrieval) {
                    uint8_t req = 2;
                    rc = write_auth_packet(c, &req, 1, e);
                } else {
                    err_set(e, IRON_ERR_MYSQL_INSECURE_AUTH);
                    rc = IRON_ERR_MYSQL_INSECURE_AUTH;
                }
                continue;
            }
            if (n > 1 && (plugin_is(c->plugin, "caching_sha2_password") ||
                          plugin_is(c->plugin, "sha256_password"))) {
                /* The server's public key (PEM). */
                if (!set_pubkey(c, p + 1, n - 1)) { rc = IRON_ERR_MYSQL_NO_MEMORY; err_set(e, rc); break; }
                rc = send_rsa_password(c, k, scramble, *slen, e);
                continue;
            }
            rc = conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
            break;
        }
        rc = conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
    }
    if (!rc) { err_set(e, IRON_ERR_MYSQL_AUTH); rc = IRON_ERR_MYSQL_AUTH; }
    buf_free(&resp);
    return rc;
}

static int run_simple(MyConn *c, const char *sql, size_t len, MyErr *e);

static int conn_handshake(MyConn *c, const MyCfg *k, MyErr *e) {
    const uint8_t *p;
    size_t n;
    c->seq = 0;
    int rc = conn_read_packet(c, &p, &n, e);
    if (rc) return rc;
    if (n > 0 && p[0] == 0xff) {
        err_from_packet(e, p, n, 0);
        return (int)e->code;
    }
    Rd r = { p, n, 0, 0 };
    uint8_t proto = rd_u8(&r);
    if (proto != 10) {
        err_setf(e, IRON_ERR_MYSQL_PROTOCOL, "unsupported protocol version %u", proto);
        c->broken = 1;
        return IRON_ERR_MYSQL_PROTOCOL;
    }
    size_t vlen;
    const uint8_t *ver = rd_cstr(&r, &vlen);
    if (r.bad) return conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
    if (vlen >= sizeof(c->version)) vlen = sizeof(c->version) - 1;
    memcpy(c->version, ver, vlen);
    c->version[vlen] = '\0';
    c->mariadb = strstr(c->version, "MariaDB") != NULL;
    if (c->mariadb && strncmp(c->version, "5.5.5-", 6) == 0)
        memmove(c->version, c->version + 6, strlen(c->version + 6) + 1);
    c->thread_id = rd_u32(&r);
    uint8_t scramble[64];
    size_t slen = 0;
    if (rd_need(&r, 8)) { memcpy(scramble, r.p + r.i, 8); r.i += 8; slen = 8; }
    rd_skip(&r, 1);
    uint32_t caps = rd_u16(&r);
    uint8_t server_collation = 0;
    if (r.i < r.n) {
        server_collation = rd_u8(&r);
        c->status = rd_u16(&r);
        caps |= (uint32_t)rd_u16(&r) << 16;
        uint8_t auth_len = rd_u8(&r);
        rd_skip(&r, 10);
        if (caps & CLIENT_SECURE_CONNECTION) {
            size_t part2 = auth_len > 8 ? (size_t)auth_len - 8 : 13;
            if (part2 > r.n - r.i) part2 = r.n - r.i;
            if (part2 > 0 && part2 <= sizeof(scramble) - 8) {
                memcpy(scramble + 8, r.p + r.i, part2);
                slen = 8 + part2;
                if (scramble[slen - 1] == 0) slen--;
                r.i += part2;
            }
        }
        c->plugin[0] = '\0';
        if ((caps & CLIENT_PLUGIN_AUTH) && r.i < r.n) {
            size_t plen = r.n - r.i;
            const uint8_t *z = (const uint8_t *)memchr(r.p + r.i, 0, plen);
            if (z) plen = (size_t)(z - (r.p + r.i));
            if (plen < sizeof(c->plugin)) {
                memcpy(c->plugin, r.p + r.i, plen);
                c->plugin[plen] = '\0';
            }
        }
    }
    if (r.bad) return conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
    if (!(caps & CLIENT_PROTOCOL_41)) {
        err_setf(e, IRON_ERR_MYSQL_PROTOCOL, "server does not support protocol 4.1");
        c->broken = 1;
        return IRON_ERR_MYSQL_PROTOCOL;
    }
    if (!c->plugin[0]) snprintf(c->plugin, sizeof(c->plugin), "mysql_native_password");

    uint32_t want = CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_PROTOCOL_41 |
                    CLIENT_TRANSACTIONS | CLIENT_SECURE_CONNECTION |
                    CLIENT_MULTI_RESULTS | CLIENT_PS_MULTI_RESULTS |
                    CLIENT_PLUGIN_AUTH | CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA |
                    CLIENT_DEPRECATE_EOF;
    if (k->database[0]) want |= CLIENT_CONNECT_WITH_DB;
    if (k->multi_statements) want |= CLIENT_MULTI_STATEMENTS;
    int want_tls = k->tls != IRON_MYSQL_TLS_DISABLED;
    if (want_tls && (caps & CLIENT_SSL)) want |= CLIENT_SSL;
    else if (want_tls && k->tls != IRON_MYSQL_TLS_PREFERRED) {
        err_set(e, IRON_ERR_MYSQL_TLS_REQUIRED);
        return IRON_ERR_MYSQL_TLS_REQUIRED;
    }
    c->caps = want & caps;

    int cid = k->collation[0] ? collation_id(k->collation) : -1;
    c->collation = cid > 0 ? (uint8_t)cid : (server_collation == 255 ? 255 : 45);
    uint32_t max_packet = 16u << 20;

    if (c->caps & CLIENT_SSL) {
        c->w.len = 0;
        size_t at = pkt_begin(c);
        buf_u32(&c->w, c->caps);
        buf_u32(&c->w, max_packet);
        buf_u8(&c->w, c->collation);
        uint8_t zero[23] = { 0 };
        buf_put(&c->w, zero, 23);
        rc = pkt_send(c, at, e);
        if (rc) return rc;
        int mode = k->tls == IRON_MYSQL_TLS_VERIFY_IDENTITY ? IRON_TLS_VERIFY_FULL
                 : k->tls == IRON_MYSQL_TLS_VERIFY_CA ? IRON_TLS_VERIFY_CA
                 : IRON_TLS_VERIFY_NONE;
        Iron_TcpSocket sock = { c->fd };
        Iron_String host = str_c(k->host);
        Iron_String ca = str_c(k->ca_file);
        Iron_TlsStreamResult t = iron_tls_client_connect_mode(sock, host, ca, mode, c->deadline);
        iron_string_release(&host);
        iron_string_release(&ca);
        if (t.error.code != 0) {
            err_setf(e, t.error.code, "TLS: %s", client_message(t.error.code));
            c->broken = 1;
            return (int)t.error.code;
        }
        c->tls = t.stream;
    }

    if (k->server_public_key[0] && !c->pubkey) {
        char *pem = NULL;
        size_t pem_len = 0;
        if (!read_file_bytes(k->server_public_key, &pem, &pem_len)) {
            err_setf(e, IRON_ERR_MYSQL_INVALID_ARGUMENT,
                     "could not read server_public_key file '%s'", k->server_public_key);
            return IRON_ERR_MYSQL_INVALID_ARGUMENT;
        }
        c->pubkey = pem;
        c->pubkey_len = pem_len;
    }

    MyBuf auth = { 0 };
    rc = auth_response(c, k, c->plugin, scramble, slen, &auth, e);
    if (rc) { buf_free(&auth); return rc; }
    c->w.len = 0;
    size_t at = pkt_begin(c);
    buf_u32(&c->w, c->caps);
    buf_u32(&c->w, max_packet);
    buf_u8(&c->w, c->collation);
    uint8_t zero[23] = { 0 };
    buf_put(&c->w, zero, 23);
    buf_put(&c->w, k->user, strlen(k->user) + 1);
    if (c->caps & CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) {
        buf_lenstr(&c->w, auth.p, auth.len);
    } else {
        buf_u8(&c->w, (uint8_t)auth.len);
        buf_put(&c->w, auth.p, auth.len);
    }
    buf_free(&auth);
    if (c->caps & CLIENT_CONNECT_WITH_DB) buf_put(&c->w, k->database, strlen(k->database) + 1);
    if (c->caps & CLIENT_PLUGIN_AUTH) buf_put(&c->w, c->plugin, strlen(c->plugin) + 1);
    rc = pkt_send(c, at, e);
    if (rc) return rc;
    rc = auth_loop(c, k, scramble, &slen, e);
    memset(scramble, 0, sizeof(scramble));
    if (rc) return rc;
    c->open = 1;

    /* A collation without a one-byte id (or not in the table) is set with
     * SET NAMES <charset> COLLATE <collation>. */
    if (k->collation[0] && cid < 0) {
        char sql[200];
        const char *us = strchr(k->collation, '_');
        size_t cs_len = us ? (size_t)(us - k->collation) : strlen(k->collation);
        for (const char *q = k->collation; *q; q++) {
            if (!isalnum((unsigned char)*q) && *q != '_') {
                err_setf(e, IRON_ERR_MYSQL_INVALID_ARGUMENT, "invalid collation name");
                return IRON_ERR_MYSQL_INVALID_ARGUMENT;
            }
        }
        int len = snprintf(sql, sizeof(sql), "SET NAMES %.*s COLLATE %s",
                           (int)cs_len, k->collation, k->collation);
        if (len < 0 || (size_t)len >= sizeof(sql)) {
            err_set(e, IRON_ERR_MYSQL_INVALID_ARGUMENT);
            return IRON_ERR_MYSQL_INVALID_ARGUMENT;
        }
        rc = run_simple(c, sql, (size_t)len, e);
        if (rc) return rc;
    }
    return 0;
}

static void set_nodelay(int64_t fd) {
    int one = 1;
#ifdef _WIN32
    setsockopt((SOCKET)fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, sizeof(one));
#else
    setsockopt((int)fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt((int)fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#endif
}

/* Open and authenticate a connection. Always returns a MyConn (NULL only
 * when out of memory); on failure it is closed and carries the error. */
static MyConn *conn_open(const MyCfg *k) {
    MyConn *c = conn_new();
    if (!c) return NULL;
    c->timeout_ms = k->timeout > 0 ? k->timeout : 0;
    c->max_packet = k->max_packet > 0 ? (uint64_t)k->max_packet : 1073741824u;
#ifndef IRON_HAVE_OPENSSL
    err_set(&c->err, IRON_ERR_MYSQL_UNAVAILABLE);
    return c;
#endif
    if (!k->host[0] || k->port <= 0 || k->port > 65535) {
        err_setf(&c->err, IRON_ERR_MYSQL_INVALID_ARGUMENT, "invalid host or port");
        return c;
    }
    int64_t connect_ms = k->connect_timeout > 0 ? k->connect_timeout : 0;
    c->deadline = deadline_after(connect_ms);
    Iron_String host = str_c(k->host);
    Iron_Result_TcpSocket_Error d = Iron_net_tcp_dial(
        host, k->port, connect_ms > 0 ? connect_ms : 365LL * 24 * 3600 * 1000);
    iron_string_release(&host);
    if (d.v1.code != 0) {
        err_setf(&c->err, d.v1.code, "could not connect to %s:%lld: %s", k->host,
                 (long long)k->port, client_message(d.v1.code));
        return c;
    }
    c->fd = d.v0.fd;
    set_nodelay(c->fd);
    c->open = 0;
    int rc = conn_handshake(c, k, &c->err);
    if (rc) {
        if (!c->err.code) err_set(&c->err, rc);
        c->open = 1;          /* let shutdown close the socket */
        c->broken = 1;
        conn_shutdown(c, 0);
        return c;
    }
    err_clear(&c->err);
    c->idle_since_ms = Iron_monotonic_now_ms();
    return c;
}

/* ── Result sets ────────────────────────────────────────────────────────── */

static int meta_put(MyMeta *m, const uint8_t *str, size_t n, uint32_t *off, uint32_t *len) {
    if (m->strings.len > UINT32_MAX - n) return 0;
    *off = (uint32_t)m->strings.len;
    *len = (uint32_t)n;
    return buf_put(&m->strings, str, n);
}

static int parse_column(MyMeta *m, MyCol *col, const uint8_t *p, size_t n) {
    Rd r = { p, n, 0, 0 };
    size_t len;
    const uint8_t *str;
    (void)rd_lenstr(&r, &len);                       /* catalog */
    str = rd_lenstr(&r, &len);
    if (r.bad || !meta_put(m, str, len, &col->schema, &col->schema_len)) return 0;
    str = rd_lenstr(&r, &len);
    if (r.bad || !meta_put(m, str, len, &col->table, &col->table_len)) return 0;
    str = rd_lenstr(&r, &len);
    if (r.bad || !meta_put(m, str, len, &col->org_table, &col->org_table_len)) return 0;
    str = rd_lenstr(&r, &len);
    if (r.bad || !meta_put(m, str, len, &col->name, &col->name_len)) return 0;
    str = rd_lenstr(&r, &len);
    if (r.bad || !meta_put(m, str, len, &col->org_name, &col->org_name_len)) return 0;
    (void)rd_lenenc(&r, NULL);                       /* length of fixed fields */
    col->charset = rd_u16(&r);
    col->length = rd_u32(&r);
    col->type = rd_u8(&r);
    col->flags = rd_u16(&r);
    col->decimals = rd_u8(&r);
    return !r.bad;
}

/* Build column metadata from length-prefixed definition packets. */
static MyMeta *meta_build(const uint8_t *raw, size_t raw_len, uint32_t count) {
    MyMeta *m = (MyMeta *)calloc(1, sizeof(MyMeta));
    if (!m) return NULL;
    atomic_init(&m->refs, 1);
    m->ncols = count;
    m->cols = (MyCol *)calloc(count ? count : 1, sizeof(MyCol));
    if (!m->cols || !buf_put(&m->raw, raw, raw_len)) { meta_unref(m); return NULL; }
    size_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (raw_len - off < 4) { meta_unref(m); return NULL; }
        uint32_t n = rd32(raw + off);
        off += 4;
        if (n > raw_len - off || !parse_column(m, &m->cols[i], raw + off, n)) {
            meta_unref(m);
            return NULL;
        }
        off += n;
    }
    return m;
}

/* Read `count` column definitions (and the EOF after them when the server
 * still sends one) into s->md, or skip them when s is NULL. `cache` is a
 * prepared statement's last metadata: reused when the server sent the same
 * bytes again, replaced otherwise. */
static int read_columns(MyConn *c, MySet *s, uint32_t count, MyMeta **cache, MyErr *e) {
    MyBuf *raw = &c->colbuf;
    raw->len = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p;
        size_t n;
        int rc = conn_read_packet(c, &p, &n, e);
        if (rc) return rc;
        if (s && (!buf_u32(raw, (uint32_t)n) || !buf_put(raw, p, n)))
            return conn_fail(c, e, IRON_ERR_MYSQL_NO_MEMORY);
    }
    if (!(c->caps & CLIENT_DEPRECATE_EOF) && count > 0) {
        const uint8_t *p;
        size_t n;
        int rc = conn_read_packet(c, &p, &n, e);
        if (rc) return rc;
        if (!is_eof_packet(p, n)) return conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
    }
    if (!s) return 0;
    MyMeta *m = NULL;
    if (cache && *cache && (*cache)->ncols == count && (*cache)->raw.len == raw->len &&
        memcmp((*cache)->raw.p, raw->p, raw->len) == 0) {
        m = meta_ref(*cache);
    } else {
        m = meta_build(raw->p, raw->len, count);
        if (!m) return conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
        if (cache) {
            meta_unref(*cache);
            *cache = meta_ref(m);
        }
    }
    if (raw->cap > (64u << 10)) buf_free(raw);
    s->md = m;
    s->ncols = m->ncols;
    s->cols = m->cols;
    return 0;
}

static int set_grow_rows(MySet *s) {
    if (s->nrows < s->rows_cap) return 1;
    size_t per = (size_t)s->ncols * 2;
    if (!s->cells && per && per <= SET_INLINE_CELLS) {
        s->cells = s->cells_inline;
        s->rows_cap = SET_INLINE_CELLS / per;
        return 1;
    }
    uint64_t cap = s->rows_cap ? s->rows_cap * 2 : 8;
    if (per && cap > (SIZE_MAX / sizeof(uint32_t)) / per) return 0;
    size_t bytes = (size_t)cap * (per ? per : 1) * sizeof(uint32_t);
    uint32_t *cells;
    if (s->cells == s->cells_inline) {
        cells = (uint32_t *)malloc(bytes);
        if (cells) memcpy(cells, s->cells_inline, (size_t)s->nrows * per * sizeof(uint32_t));
    } else {
        cells = (uint32_t *)realloc(s->cells, bytes);
    }
    if (!cells) return 0;
    s->cells = cells;
    s->rows_cap = cap;
    return 1;
}

/* Append row bytes to the set's data, starting in the inline buffer. */
static int set_data_put(MySet *s, const uint8_t *p, size_t n) {
    if (!s->data.p && n <= SET_INLINE_DATA) {
        s->data.p = s->data_inline;
        s->data.cap = SET_INLINE_DATA;
    }
    if (s->data.p == s->data_inline && s->data.len + n > SET_INLINE_DATA) {
        size_t cap = SET_INLINE_DATA * 4;
        while (cap < s->data.len + n) cap *= 2;
        uint8_t *heap = (uint8_t *)malloc(cap);
        if (!heap) return 0;
        memcpy(heap, s->data_inline, s->data.len);
        s->data.p = heap;
        s->data.cap = cap;
    }
    return buf_put(&s->data, p, n);
}

/* Length of a binary-protocol value of this type at p (n bytes left), or
 * -1 when malformed. *skip is the length prefix before the value bytes. */
static int64_t binary_value_len(uint8_t type, const uint8_t *p, size_t n, size_t *skip) {
    *skip = 0;
    switch (type) {
        case T_NULL: return 0;
        case T_TINY: return 1;
        case T_SHORT: case T_YEAR: return 2;
        case T_LONG: case T_INT24: case T_FLOAT: return 4;
        case T_LONGLONG: case T_DOUBLE: return 8;
        case T_DATE: case T_DATETIME: case T_TIMESTAMP: case T_TIME:
        case T_NEWDATE: case T_DATETIME2: case T_TIMESTAMP2: case T_TIME2:
            if (n < 1) return -1;
            *skip = 1;
            return p[0];
        default: {
            Rd r = { p, n, 0, 0 };
            uint64_t len = rd_lenenc(&r, NULL);
            if (r.bad) return -1;
            *skip = r.i;
            return (int64_t)len;
        }
    }
}

/* Store one row packet: the payload is copied into the set's data buffer
 * and each value is indexed by offset and length. */
static int store_row(MySet *s, const uint8_t *p, size_t n, int binary) {
    if (!set_grow_rows(s)) return IRON_ERR_MYSQL_NO_MEMORY;
    if (s->data.len > (size_t)UINT32_MAX - n) return IRON_ERR_MYSQL_RESULT_TOO_LARGE;
    size_t base = s->data.len;
    if (!set_data_put(s, p, n)) return IRON_ERR_MYSQL_NO_MEMORY;
    uint32_t *cell = s->cells + (size_t)s->nrows * s->ncols * 2;
    if (!binary) {
        Rd r = { p, n, 0, 0 };
        for (uint32_t i = 0; i < s->ncols; i++) {
            if (r.i < n && p[r.i] == 0xfb) {
                r.i++;
                cell[2 * i] = 0;
                cell[2 * i + 1] = CELL_NULL;
                continue;
            }
            size_t len;
            const uint8_t *v = rd_lenstr(&r, &len);
            if (r.bad) return IRON_ERR_MYSQL_PROTOCOL;
            cell[2 * i] = (uint32_t)(base + (size_t)(v - p));
            cell[2 * i + 1] = (uint32_t)len;
        }
        if (r.i != n) return IRON_ERR_MYSQL_PROTOCOL;
    } else {
        size_t bitmap = (s->ncols + 7 + 2) / 8;
        if (n < 1 + bitmap || p[0] != 0x00) return IRON_ERR_MYSQL_PROTOCOL;
        const uint8_t *nulls = p + 1;
        size_t i = 1 + bitmap;
        for (uint32_t col = 0; col < s->ncols; col++) {
            size_t bit = col + 2;
            if (nulls[bit / 8] & (1u << (bit % 8))) {
                cell[2 * col] = 0;
                cell[2 * col + 1] = CELL_NULL;
                continue;
            }
            size_t skip;
            int64_t len = binary_value_len(s->cols[col].type, p + i, n - i, &skip);
            if (len < 0 || (uint64_t)len > n - i - skip) return IRON_ERR_MYSQL_PROTOCOL;
            cell[2 * col] = (uint32_t)(base + i + skip);
            cell[2 * col + 1] = (uint32_t)len;
            i += skip + (size_t)len;
        }
        if (i != n) return IRON_ERR_MYSQL_PROTOCOL;
    }
    s->nrows++;
    return 0;
}

/* Read every result of a command into `res` (rows are skipped when
 * res->discard). Stops after an ERR packet, which ends the command. */
static int read_results(MyConn *c, MyResult *res, int binary, int discard,
                        MyMeta **cache) {
    MyErr *e = &res->err;
    for (;;) {
        const uint8_t *p;
        size_t n;
        int rc = conn_read_packet(c, &p, &n, e);
        if (rc) return rc;
        if (n == 0) return conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
        if (p[0] == 0x00) {
            MySet *s = discard && res->nsets ? res_set(res, 0) : result_add_set(res);
            if (!s) return conn_fail(c, e, IRON_ERR_MYSQL_NO_MEMORY);
            if (!parse_ok(c, p, n, s)) return conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
        } else if (p[0] == 0xff) {
            err_from_packet(e, p, n, c->caps);
            return 0;
        } else if (p[0] == 0xfb) {
            /* LOCAL INFILE request: refuse by sending an empty packet, then
             * read the server's answer. */
            c->w.len = 0;
            size_t at = pkt_begin(c);
            rc = pkt_send(c, at, e);
            if (rc) return rc;
            rc = conn_read_packet(c, &p, &n, e);
            if (rc) return rc;
            if (n > 0 && p[0] == 0xff) err_from_packet(e, p, n, c->caps);
            else if (n > 0 && p[0] == 0x00) parse_ok(c, p, n, NULL);
            if (!e->code) err_set(e, IRON_ERR_MYSQL_LOCAL_INFILE);
            if (!(c->status & SERVER_MORE_RESULTS_EXISTS)) return 0;
            continue;
        } else {
            Rd r = { p, n, 0, 0 };
            uint64_t ncols = rd_lenenc(&r, NULL);
            if (r.bad || ncols == 0 || ncols > 4096) return conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
            MySet scratch;
            set_init(&scratch);
            MySet *s = discard ? &scratch : result_add_set(res);
            if (!s) return conn_fail(c, e, IRON_ERR_MYSQL_NO_MEMORY);
            s->binary = binary;
            rc = read_columns(c, discard ? NULL : s, (uint32_t)ncols, discard ? NULL : cache, e);
            cache = NULL;               /* only the first set is cached */
            if (rc) { if (discard) set_free(&scratch); return rc; }
            for (;;) {
                rc = conn_read_packet(c, &p, &n, e);
                if (rc) { if (discard) set_free(&scratch); return rc; }
                if (is_terminator(c, p, n)) {
                    if (!parse_terminator(c, p, n, s)) {
                        if (discard) set_free(&scratch);
                        return conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
                    }
                    break;
                }
                if (n > 0 && p[0] == 0xff) {
                    err_from_packet(e, p, n, c->caps);
                    if (discard) set_free(&scratch);
                    return 0;
                }
                if (discard) continue;
                rc = store_row(s, p, n, binary);
                if (rc) return conn_fail(c, e, rc);
            }
            if (discard) {
                uint16_t st = scratch.status;
                set_free(&scratch);
                c->status = st;
            }
        }
        if (!(c->status & SERVER_MORE_RESULTS_EXISTS)) return 0;
    }
}

/* ── Text protocol commands ─────────────────────────────────────────────── */

static int fetch_server_max_packet(MyConn *c, MyErr *e);

/* Refuse to send a packet the server would reject (and then disconnect).
 * The server's limit is fetched once, only for payloads over 1 MB. */
static int check_payload(MyConn *c, uint64_t payload, MyErr *e) {
    if (payload <= (1u << 20)) return 0;
    if (c->server_max_packet < 0) {
        int rc = fetch_server_max_packet(c, e);
        if (rc) return rc;
    }
    if (c->server_max_packet > 0 && payload > (uint64_t)c->server_max_packet) {
        err_setf(e, IRON_ERR_MYSQL_PACKET_TOO_LARGE,
                 "statement of %llu bytes exceeds the server's max_allowed_packet (%lld bytes)",
                 (unsigned long long)payload, (long long)c->server_max_packet);
        return IRON_ERR_MYSQL_PACKET_TOO_LARGE;
    }
    return 0;
}

static int send_query(MyConn *c, const char *sql, size_t len, MyErr *e) {
    int rc = check_payload(c, (uint64_t)len + 1, e);
    if (rc) return rc;
    size_t at;
    rc = cmd_begin(c, e, &at);
    if (rc) return rc;
    if (!buf_u8(&c->w, COM_QUERY) || !buf_put(&c->w, sql, len)) {
        err_set(e, IRON_ERR_MYSQL_NO_MEMORY);
        return IRON_ERR_MYSQL_NO_MEMORY;
    }
    return pkt_send(c, at, e);
}

static MyResult *do_query(MyConn *c, const char *sql, size_t len) {
    MyResult *res = result_new();
    if (!res) return NULL;
    if (send_query(c, sql, len, &res->err) == 0) read_results(c, res, 0, 0, NULL);
    if (res->nsets == 0) result_add_set(res);
    return res;
}

/* Run SQL and keep only the error. */
static int run_simple(MyConn *c, const char *sql, size_t len, MyErr *e) {
    MyResult res;
    memset(&res, 0, offsetof(MyResult, err));
    err_clear(&res.err);
    int rc = send_query(c, sql, len, &res.err);
    if (!rc) read_results(c, &res, 0, 1, NULL);
    result_clear(&res);
    *e = res.err;
    return (int)res.err.code;
}

static int fetch_server_max_packet(MyConn *c, MyErr *e) {
    static const char sql[] = "SELECT @@max_allowed_packet";
    MyResult res;
    memset(&res, 0, offsetof(MyResult, err));
    err_clear(&res.err);
    int rc = send_query(c, sql, sizeof(sql) - 1, &res.err);
    if (!rc) read_results(c, &res, 0, 0, NULL);
    c->server_max_packet = 0;
    if (!res.err.code && res.nsets && res.first.nrows == 1 && res.first.ncols == 1 &&
        res.first.cells[1] != CELL_NULL) {
        char tmp[32];
        uint32_t len = res.first.cells[1];
        if (len < sizeof(tmp)) {
            memcpy(tmp, res.first.data.p + res.first.cells[0], len);
            tmp[len] = '\0';
            c->server_max_packet = strtoll(tmp, NULL, 10);
        }
    }
    result_clear(&res);
    if (res.err.code) { *e = res.err; return (int)res.err.code; }
    return 0;
}

/* Commands answered by a plain OK or ERR. */
static int simple_command(MyConn *c, uint8_t cmd, const char *arg, size_t arg_len, MyErr *e) {
    size_t at;
    int rc = cmd_begin(c, e, &at);
    if (rc) return rc;
    if (!buf_u8(&c->w, cmd) || !buf_put(&c->w, arg, arg_len)) {
        err_set(e, IRON_ERR_MYSQL_NO_MEMORY);
        return IRON_ERR_MYSQL_NO_MEMORY;
    }
    rc = pkt_send(c, at, e);
    if (rc) return rc;
    const uint8_t *p;
    size_t n;
    rc = conn_read_packet(c, &p, &n, e);
    if (rc) return rc;
    if (n > 0 && p[0] == 0xff) {
        err_from_packet(e, p, n, c->caps);
        return (int)e->code;
    }
    if (n == 0 || p[0] != 0x00 || !parse_ok(c, p, n, NULL))
        return conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
    return 0;
}

/* ── Prepared statements ────────────────────────────────────────────────── */

static int prepare_stmt(MyConn *c, const char *sql, size_t len, MyPrep *out, MyErr *e) {
    memset(out, 0, sizeof(*out));
    int rc = check_payload(c, (uint64_t)len + 1, e);
    if (rc) return rc;
    size_t at;
    rc = cmd_begin(c, e, &at);
    if (rc) return rc;
    if (!buf_u8(&c->w, COM_STMT_PREPARE) || !buf_put(&c->w, sql, len)) {
        err_set(e, IRON_ERR_MYSQL_NO_MEMORY);
        return IRON_ERR_MYSQL_NO_MEMORY;
    }
    rc = pkt_send(c, at, e);
    if (rc) return rc;
    const uint8_t *p;
    size_t n;
    rc = conn_read_packet(c, &p, &n, e);
    if (rc) return rc;
    if (n > 0 && p[0] == 0xff) {
        err_from_packet(e, p, n, c->caps);
        return (int)e->code;
    }
    if (n < 12 || p[0] != 0x00) return conn_fail(c, e, IRON_ERR_MYSQL_PROTOCOL);
    out->id = rd32(p + 1);
    out->ncols = rd16(p + 5);
    out->nparams = rd16(p + 7);
    rc = read_columns(c, NULL, out->nparams, NULL, e);
    if (!rc) rc = read_columns(c, NULL, out->ncols, NULL, e);
    if (rc) return rc;
    if (out->nparams) {
        out->types = (uint8_t *)calloc(out->nparams, 2);
        if (!out->types) { err_set(e, IRON_ERR_MYSQL_NO_MEMORY); return IRON_ERR_MYSQL_NO_MEMORY; }
    }
    return 0;
}

static void queue_stmt_close(MyConn *c, uint32_t id) {
    iron_mutex_lock(&c->pend_lock);
    if (c->npend == c->pend_cap) {
        int cap = c->pend_cap ? c->pend_cap * 2 : 8;
        uint32_t *p = (uint32_t *)realloc(c->pend, (size_t)cap * sizeof(uint32_t));
        if (p) { c->pend = p; c->pend_cap = cap; }
    }
    if (c->npend < c->pend_cap) c->pend[c->npend++] = id;
    atomic_store_explicit(&c->pend_count, c->npend, memory_order_release);
    iron_mutex_unlock(&c->pend_lock);
}

static int dt_ok(const Iron_MysqlDateTime *d) {
    return d->year >= 0 && d->year <= 9999 && d->month >= 0 && d->month <= 12 &&
           d->day >= 0 && d->day <= 31 && d->hour >= 0 && d->hour <= 23 &&
           d->minute >= 0 && d->minute <= 59 && d->second >= 0 && d->second <= 59 &&
           d->microsecond >= 0 && d->microsecond <= 999999;
}

static int bind_value(MyBuf *w, const Iron_MysqlValue *v, uint8_t *type, uint8_t *flag) {
    *flag = 0;
    switch (v->tag) {
        case Iron_MysqlValue_TAG_Null:
            *type = T_NULL;
            return 1;
        case Iron_MysqlValue_TAG_Int:
            *type = T_LONGLONG;
            return buf_u64(w, (uint64_t)v->data.Int._0);
        case Iron_MysqlValue_TAG_UInt:
            *type = T_LONGLONG;
            *flag = 0x80;
            return buf_u64(w, v->data.UInt._0);
        case Iron_MysqlValue_TAG_Float: {
            *type = T_DOUBLE;
            uint64_t bits;
            memcpy(&bits, &v->data.Float._0, 8);
            return buf_u64(w, bits);
        }
        case Iron_MysqlValue_TAG_Decimal:
        case Iron_MysqlValue_TAG_Text:
        case Iron_MysqlValue_TAG_Bytes: {
            const Iron_String *s = v->tag == Iron_MysqlValue_TAG_Decimal ? &v->data.Decimal._0
                                 : v->tag == Iron_MysqlValue_TAG_Text ? &v->data.Text._0
                                 : &v->data.Bytes._0;
            *type = v->tag == Iron_MysqlValue_TAG_Decimal ? T_NEWDECIMAL
                  : v->tag == Iron_MysqlValue_TAG_Text ? T_VAR_STRING : T_BLOB;
            return buf_lenstr(w, iron_string_cstr(s), iron_string_byte_len(s));
        }
        case Iron_MysqlValue_TAG_Bool:
            *type = T_TINY;
            return buf_u8(w, v->data.Bool._0 ? 1 : 0);
        case Iron_MysqlValue_TAG_Date:
        case Iron_MysqlValue_TAG_DateTime: {
            const Iron_MysqlDateTime *d = v->tag == Iron_MysqlValue_TAG_Date
                ? &v->data.Date._0 : &v->data.DateTime._0;
            if (!dt_ok(d)) return -1;
            *type = v->tag == Iron_MysqlValue_TAG_Date ? T_DATE : T_DATETIME;
            int with_time = v->tag == Iron_MysqlValue_TAG_DateTime &&
                (d->hour || d->minute || d->second || d->microsecond);
            uint8_t len = d->microsecond ? 11 : with_time ? 7 : 4;
            if (v->tag == Iron_MysqlValue_TAG_Date) len = 4;
            if (!buf_u8(w, len) || !buf_u16(w, (uint16_t)d->year) ||
                !buf_u8(w, (uint8_t)d->month) || !buf_u8(w, (uint8_t)d->day)) return 0;
            if (len >= 7 && (!buf_u8(w, (uint8_t)d->hour) || !buf_u8(w, (uint8_t)d->minute) ||
                             !buf_u8(w, (uint8_t)d->second))) return 0;
            if (len == 11 && !buf_u32(w, (uint32_t)d->microsecond)) return 0;
            return 1;
        }
        case Iron_MysqlValue_TAG_Time: {
            const Iron_MysqlTime *t = &v->data.Time._0;
            if (t->hours < 0 || t->hours > 838 || t->minutes < 0 || t->minutes > 59 ||
                t->seconds < 0 || t->seconds > 59 || t->microsecond < 0 ||
                t->microsecond > 999999) return -1;
            *type = T_TIME;
            uint8_t len = t->microsecond ? 12 : 8;
            if (!buf_u8(w, len) || !buf_u8(w, t->negative ? 1 : 0) ||
                !buf_u32(w, (uint32_t)(t->hours / 24)) ||
                !buf_u8(w, (uint8_t)(t->hours % 24)) || !buf_u8(w, (uint8_t)t->minutes) ||
                !buf_u8(w, (uint8_t)t->seconds)) return 0;
            if (len == 12 && !buf_u32(w, (uint32_t)t->microsecond)) return 0;
            return 1;
        }
        default:
            return -1;
    }
}

static uint64_t params_size(const Iron_MysqlValueList *params) {
    uint64_t total = 0;
    for (int64_t i = 0; i < params->count; i++) {
        const Iron_MysqlValue *v = &params->items[i];
        if (v->tag == Iron_MysqlValue_TAG_Text) total += iron_string_byte_len(&v->data.Text._0) + 9;
        else if (v->tag == Iron_MysqlValue_TAG_Bytes) total += iron_string_byte_len(&v->data.Bytes._0) + 9;
        else if (v->tag == Iron_MysqlValue_TAG_Decimal) total += iron_string_byte_len(&v->data.Decimal._0) + 9;
        else total += 16;
    }
    return total;
}

static int execute_stmt(MyConn *c, MyPrep *st, const Iron_MysqlValueList *params,
                        MyResult *res) {
    MyErr *e = &res->err;
    if (params->count != st->nparams) {
        err_setf(e, IRON_ERR_MYSQL_PARAM_COUNT,
                 "statement has %u placeholders but %lld parameters were given",
                 (unsigned)st->nparams, (long long)params->count);
        return IRON_ERR_MYSQL_PARAM_COUNT;
    }
    int rc = check_payload(c, params_size(params) + 16 + (uint64_t)st->nparams * 3, e);
    if (rc) return rc;
    size_t at;
    rc = cmd_begin(c, e, &at);
    if (rc) return rc;
    MyBuf *w = &c->w;
    int ok = buf_u8(w, COM_STMT_EXECUTE) && buf_u32(w, st->id) && buf_u8(w, 0) && buf_u32(w, 1);
    if (ok && st->nparams) {
        size_t bitmap_len = ((size_t)st->nparams + 7) / 8;
        size_t bitmap_at = w->len;
        uint8_t zero = 0;
        for (size_t i = 0; ok && i < bitmap_len; i++) ok = buf_put(w, &zero, 1);
        size_t bound_at = w->len;
        ok = ok && buf_u8(w, 1);
        size_t types_at = w->len;
        for (uint16_t i = 0; ok && i < st->nparams; i++) ok = buf_u16(w, 0);
        for (uint16_t i = 0; ok && i < st->nparams; i++) {
            const Iron_MysqlValue *v = &params->items[i];
            uint8_t type, flag;
            int b = bind_value(w, v, &type, &flag);
            if (b < 0) {
                w->len = 0;
                err_setf(e, IRON_ERR_MYSQL_INVALID_ARGUMENT,
                         "parameter %u has an out of range date or time", (unsigned)i + 1);
                return IRON_ERR_MYSQL_INVALID_ARGUMENT;
            }
            ok = b;
            if (v->tag == Iron_MysqlValue_TAG_Null)
                w->p[bitmap_at + i / 8] |= (uint8_t)(1u << (i % 8));
            w->p[types_at + 2 * i] = type;
            w->p[types_at + 2 * i + 1] = flag;
        }
        /* Skip the types when they match the previous execution. */
        if (ok && st->types_sent && memcmp(st->types, w->p + types_at, 2u * st->nparams) == 0) {
            w->p[bound_at] = 0;
            memmove(w->p + types_at, w->p + types_at + 2u * st->nparams,
                    w->len - types_at - 2u * st->nparams);
            w->len -= 2u * st->nparams;
        } else if (ok) {
            memcpy(st->types, w->p + types_at, 2u * st->nparams);
            st->types_sent = 1;
        }
    }
    if (!ok) {
        w->len = 0;
        err_set(e, IRON_ERR_MYSQL_NO_MEMORY);
        return IRON_ERR_MYSQL_NO_MEMORY;
    }
    rc = pkt_send(c, at, e);
    if (rc) return rc;
    return read_results(c, res, 1, 0, &st->rmeta);
}

/* Statement cache: one LRU per connection, keyed by SQL text. */
static void cache_remove(MyConn *c, int i, int close_on_server) {
    if (close_on_server) queue_stmt_close(c, c->cache[i].prep.id);
    free(c->cache[i].sql);
    prep_free(&c->cache[i].prep);
    c->cache[i] = c->cache[--c->ncache];
    c->cache_mru = -1;
}

static void cache_clear(MyConn *c, int close_on_server) {
    while (c->ncache > 0) cache_remove(c, c->ncache - 1, close_on_server);
}

static MyPrep *cache_get(MyConn *c, const char *sql, size_t len, MyErr *e) {
    /* The statement used last is the likeliest: compare it before hashing. */
    if (c->cache_mru >= 0 && c->cache_mru < c->ncache) {
        MyCacheEnt *ent = &c->cache[c->cache_mru];
        if (ent->len == len && memcmp(ent->sql, sql, len) == 0) {
            ent->used = ++c->cache_clock;
            return &ent->prep;
        }
    }
    uint64_t h = hash_bytes(sql, len);
    for (int i = 0; i < c->ncache; i++) {
        MyCacheEnt *ent = &c->cache[i];
        if (ent->hash == h && ent->len == len && memcmp(ent->sql, sql, len) == 0) {
            ent->used = ++c->cache_clock;
            c->cache_mru = i;
            return &ent->prep;
        }
    }
    MyPrep prep;
    if (prepare_stmt(c, sql, len, &prep, e)) return NULL;
    if (!c->cache) {
        c->cache = (MyCacheEnt *)calloc(STMT_CACHE_MAX, sizeof(MyCacheEnt));
        if (!c->cache) {
            queue_stmt_close(c, prep.id);
            prep_free(&prep);
            err_set(e, IRON_ERR_MYSQL_NO_MEMORY);
            return NULL;
        }
    }
    if (c->ncache == STMT_CACHE_MAX) {
        int lru = 0;
        for (int i = 1; i < c->ncache; i++)
            if (c->cache[i].used < c->cache[lru].used) lru = i;
        cache_remove(c, lru, 1);
    }
    MyCacheEnt *ent = &c->cache[c->ncache];
    ent->sql = dup_bytes(sql, len);
    if (!ent->sql) {
        queue_stmt_close(c, prep.id);
        prep_free(&prep);
        err_set(e, IRON_ERR_MYSQL_NO_MEMORY);
        return NULL;
    }
    ent->len = len;
    ent->hash = h;
    ent->used = ++c->cache_clock;
    ent->prep = prep;
    c->cache_mru = c->ncache;
    c->ncache++;
    return &ent->prep;
}

static int cache_index(MyConn *c, MyPrep *p) {
    for (int i = 0; i < c->ncache; i++) if (&c->cache[i].prep == p) return i;
    return -1;
}

/* exec: prepare (cached) and execute. A statement the server forgot
 * (1243 unknown handler) or wants re-prepared (1615) is prepared again
 * once; SQL that cannot be prepared (1295) runs as a text query when it has
 * no parameters. */
static MyResult *conn_exec(MyConn *c, const char *sql, size_t len,
                           const Iron_MysqlValueList *params) {
    MyResult *res = result_new();
    if (!res) return NULL;
    for (int attempt = 0; attempt < 2; attempt++) {
        MyPrep *st = cache_get(c, sql, len, &res->err);
        if (!st) {
            if (res->err.server && res->err.code == 1295 && params->count == 0) {
                result_destroy(res);
                return do_query(c, sql, len);
            }
            break;
        }
        execute_stmt(c, st, params, res);
        if (res->err.server && (res->err.code == 1243 || res->err.code == 1615) &&
            attempt == 0 && !c->broken) {
            int i = cache_index(c, st);
            if (i >= 0) cache_remove(c, i, res->err.code == 1615);
            result_clear(res);
            err_clear(&res->err);
            continue;
        }
        break;
    }
    if (res->nsets == 0) result_add_set(res);
    return res;
}

/* ── Reading values ─────────────────────────────────────────────────────── */

typedef struct Cell {
    const uint8_t *p;
    uint32_t len;
    const MyCol *col;
    int binary;
} Cell;

static MySet *cur_set(MyResult *r) {
    return (r && r->cur < r->nsets) ? res_set(r, r->cur) : NULL;
}

static int get_cell(MyResult *r, int64_t row, int64_t col, Cell *out) {
    MySet *s = cur_set(r);
    if (!s || row < 0 || col < 0 || (uint64_t)row >= s->nrows || (uint64_t)col >= s->ncols)
        return IRON_ERR_MYSQL_RANGE;
    const uint32_t *cell = s->cells + ((size_t)row * s->ncols + (size_t)col) * 2;
    if (cell[1] == CELL_NULL) return IRON_ERR_MYSQL_NULL;
    out->p = s->data.p + cell[0];
    out->len = cell[1];
    out->col = &s->cols[col];
    out->binary = s->binary;
    return 0;
}

static int is_int_type(uint8_t t) {
    return t == T_TINY || t == T_SHORT || t == T_LONG || t == T_LONGLONG ||
           t == T_INT24 || t == T_YEAR;
}

static int is_float_type(uint8_t t) { return t == T_FLOAT || t == T_DOUBLE; }

/* Copy a cell into a NUL-terminated buffer for strtod and friends. */
static const char *cell_cstr(const Cell *c, char *tmp, size_t cap, char **heap) {
    *heap = NULL;
    if (c->len < cap) {
        memcpy(tmp, c->p, c->len);
        tmp[c->len] = '\0';
        return tmp;
    }
    *heap = dup_bytes(c->p, c->len);
    return *heap;
}

/* An exact integer from text: [+-]digits with an optional all-zero
 * fraction ("12", "-7", "15.000"). */
static int parse_int_text(const uint8_t *s, size_t n, int *neg, uint64_t *mag) {
    size_t i = 0;
    *neg = 0;
    *mag = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) { *neg = s[i] == '-'; i++; }
    size_t digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        uint64_t d = (uint64_t)(s[i] - '0');
        if (*mag > (UINT64_MAX - d) / 10) return 0;
        *mag = *mag * 10 + d;
        i++;
        digits++;
    }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] == '0') i++;
    }
    return digits > 0 && i == n;
}

static int decode_integer(const Cell *c, int *neg, uint64_t *mag) {
    *neg = 0;
    *mag = 0;
    uint8_t t = c->col->type;
    int uns = (c->col->flags & FLAG_UNSIGNED) != 0;
    if (c->binary && is_int_type(t)) {
        int64_t v;
        switch (t) {
            case T_TINY: v = uns ? (int64_t)c->p[0] : (int64_t)(int8_t)c->p[0]; break;
            case T_SHORT: v = uns ? (int64_t)rd16(c->p) : (int64_t)(int16_t)rd16(c->p); break;
            case T_YEAR: v = (int64_t)rd16(c->p); break;
            case T_LONG: case T_INT24:
                v = uns ? (int64_t)rd32(c->p) : (int64_t)(int32_t)rd32(c->p); break;
            default: {
                uint64_t u = rd64(c->p);
                if (uns) { *mag = u; return 0; }
                v = (int64_t)u;
            }
        }
        if (v < 0) { *neg = 1; *mag = (uint64_t)0 - (uint64_t)v; }
        else *mag = (uint64_t)v;
        return 0;
    }
    if (c->binary && is_float_type(t)) {
        double d;
        if (t == T_FLOAT) { float f; uint32_t b = rd32(c->p); memcpy(&f, &b, 4); d = f; }
        else { uint64_t b = rd64(c->p); memcpy(&d, &b, 8); }
        if (!(d == floor(d)) || d >= 18446744073709551616.0 || d <= -9223372036854775809.0)
            return IRON_ERR_MYSQL_CONVERSION;
        if (d < 0) { *neg = 1; *mag = (uint64_t)(-d); }
        else *mag = (uint64_t)d;
        return 0;
    }
    if (t == T_BIT) {
        if (c->len > 8) return IRON_ERR_MYSQL_CONVERSION;
        uint64_t u = 0;
        for (uint32_t i = 0; i < c->len; i++) u = (u << 8) | c->p[i];
        *mag = u;
        return 0;
    }
    if (c->binary && (t == T_DATE || t == T_DATETIME || t == T_TIMESTAMP || t == T_TIME))
        return IRON_ERR_MYSQL_CONVERSION;
    if (parse_int_text(c->p, c->len, neg, mag)) return 0;
    if (is_float_type(t)) {
        char tmp[64], *heap;
        const char *s = cell_cstr(c, tmp, sizeof(tmp), &heap);
        if (!s) return IRON_ERR_MYSQL_NO_MEMORY;
        char *end;
        double d = strtod(s, &end);
        int full = end && *end == '\0' && end != s;
        free(heap);
        if (!full || !(d == floor(d)) || d >= 18446744073709551616.0 ||
            d <= -9223372036854775809.0)
            return IRON_ERR_MYSQL_CONVERSION;
        if (d < 0) { *neg = 1; *mag = (uint64_t)(-d); }
        else *mag = (uint64_t)d;
        return 0;
    }
    return IRON_ERR_MYSQL_CONVERSION;
}

static int decode_double(const Cell *c, double *out) {
    uint8_t t = c->col->type;
    if (c->binary && t == T_FLOAT) {
        float f;
        uint32_t b = rd32(c->p);
        memcpy(&f, &b, 4);
        *out = f;
        return 0;
    }
    if (c->binary && t == T_DOUBLE) {
        uint64_t b = rd64(c->p);
        memcpy(out, &b, 8);
        return 0;
    }
    if ((c->binary && is_int_type(t)) || t == T_BIT) {
        int neg;
        uint64_t mag;
        int rc = decode_integer(c, &neg, &mag);
        if (rc) return rc;
        *out = neg ? -(double)mag : (double)mag;
        return 0;
    }
    if (c->binary && (t == T_DATE || t == T_DATETIME || t == T_TIMESTAMP || t == T_TIME))
        return IRON_ERR_MYSQL_CONVERSION;
    char tmp[64], *heap;
    const char *s = cell_cstr(c, tmp, sizeof(tmp), &heap);
    if (!s) return IRON_ERR_MYSQL_NO_MEMORY;
    char *end;
    errno = 0;
    double d = strtod(s, &end);
    int ok = end && end != s && *end == '\0' && c->len > 0 &&
             !isspace((unsigned char)s[0]);
    free(heap);
    if (!ok) return IRON_ERR_MYSQL_CONVERSION;
    *out = d;
    return 0;
}

typedef struct DT {
    int neg;
    int64_t year, month, day, hour, minute, second, micro;
} DT;

/* Binary DATE/DATETIME/TIMESTAMP (0, 4, 7 or 11 bytes). */
static int decode_bin_datetime(const Cell *c, DT *d) {
    memset(d, 0, sizeof(*d));
    if (c->len == 0) return 0;
    if (c->len != 4 && c->len != 7 && c->len != 11) return IRON_ERR_MYSQL_PROTOCOL;
    d->year = rd16(c->p);
    d->month = c->p[2];
    d->day = c->p[3];
    if (c->len >= 7) { d->hour = c->p[4]; d->minute = c->p[5]; d->second = c->p[6]; }
    if (c->len == 11) d->micro = rd32(c->p + 7);
    return 0;
}

/* Binary TIME (0, 8 or 12 bytes); days fold into hours. */
static int decode_bin_time(const Cell *c, DT *d) {
    memset(d, 0, sizeof(*d));
    if (c->len == 0) return 0;
    if (c->len != 8 && c->len != 12) return IRON_ERR_MYSQL_PROTOCOL;
    d->neg = c->p[0] != 0;
    d->hour = (int64_t)rd32(c->p + 1) * 24 + c->p[5];
    d->minute = c->p[6];
    d->second = c->p[7];
    if (c->len == 12) d->micro = rd32(c->p + 8);
    return 0;
}

static int take_digits(const uint8_t *s, size_t n, size_t *i, size_t min, size_t max, int64_t *out) {
    size_t k = 0;
    int64_t v = 0;
    while (*i < n && k < max && s[*i] >= '0' && s[*i] <= '9') {
        v = v * 10 + (s[*i] - '0');
        (*i)++;
        k++;
    }
    if (k < min) return 0;
    *out = v;
    return 1;
}

static int take_fraction(const uint8_t *s, size_t n, size_t *i, int64_t *micro) {
    *micro = 0;
    if (*i < n && s[*i] == '.') {
        (*i)++;
        int64_t scale = 100000;
        size_t k = 0;
        while (*i < n && s[*i] >= '0' && s[*i] <= '9') {
            if (k < 6) *micro += (s[*i] - '0') * scale;
            scale /= 10;
            (*i)++;
            k++;
        }
        if (k == 0) return 0;
    }
    return 1;
}

/* Text "YYYY-MM-DD[ HH:MM:SS[.ffffff]]". */
static int parse_datetime_text(const uint8_t *s, size_t n, DT *d) {
    memset(d, 0, sizeof(*d));
    size_t i = 0;
    if (!take_digits(s, n, &i, 4, 4, &d->year) || i >= n || s[i++] != '-' ||
        !take_digits(s, n, &i, 1, 2, &d->month) || i >= n || s[i++] != '-' ||
        !take_digits(s, n, &i, 1, 2, &d->day))
        return 0;
    if (i == n) return 1;
    if (s[i] != ' ' && s[i] != 'T') return 0;
    i++;
    if (!take_digits(s, n, &i, 1, 2, &d->hour) || i >= n || s[i++] != ':' ||
        !take_digits(s, n, &i, 1, 2, &d->minute) || i >= n || s[i++] != ':' ||
        !take_digits(s, n, &i, 1, 2, &d->second) || !take_fraction(s, n, &i, &d->micro))
        return 0;
    return i == n;
}

/* Text "[-]H+:MM:SS[.ffffff]". */
static int parse_time_text(const uint8_t *s, size_t n, DT *d) {
    memset(d, 0, sizeof(*d));
    size_t i = 0;
    if (i < n && s[i] == '-') { d->neg = 1; i++; }
    if (!take_digits(s, n, &i, 1, 4, &d->hour) || i >= n || s[i++] != ':' ||
        !take_digits(s, n, &i, 1, 2, &d->minute) || i >= n || s[i++] != ':' ||
        !take_digits(s, n, &i, 1, 2, &d->second) || !take_fraction(s, n, &i, &d->micro))
        return 0;
    return i == n;
}

static int decode_datetime(const Cell *c, DT *d) {
    uint8_t t = c->col->type;
    if (c->binary && (t == T_DATE || t == T_DATETIME || t == T_TIMESTAMP || t == T_NEWDATE))
        return decode_bin_datetime(c, d);
    if (c->binary && (is_int_type(t) || is_float_type(t) || t == T_TIME))
        return IRON_ERR_MYSQL_CONVERSION;
    return parse_datetime_text(c->p, c->len, d) ? 0 : IRON_ERR_MYSQL_CONVERSION;
}

static int decode_time(const Cell *c, DT *d) {
    uint8_t t = c->col->type;
    if (c->binary && t == T_TIME) return decode_bin_time(c, d);
    if (c->binary && (is_int_type(t) || is_float_type(t) || t == T_DATE ||
                      t == T_DATETIME || t == T_TIMESTAMP))
        return IRON_ERR_MYSQL_CONVERSION;
    return parse_time_text(c->p, c->len, d) ? 0 : IRON_ERR_MYSQL_CONVERSION;
}

static size_t fmt_fraction(char *out, size_t cap, int64_t micro, int digits) {
    if (digits <= 0) return 0;
    if (digits > 6) digits = 6;
    char tmp[8];
    snprintf(tmp, sizeof(tmp), "%06lld", (long long)micro);
    int n = snprintf(out, cap, ".%.*s", digits, tmp);
    return n > 0 ? (size_t)n : 0;
}

static size_t fmt_date(char *out, size_t cap, const DT *d) {
    int n = snprintf(out, cap, "%04lld-%02lld-%02lld", (long long)d->year,
                     (long long)d->month, (long long)d->day);
    return n > 0 ? (size_t)n : 0;
}

static size_t fmt_datetime(char *out, size_t cap, const DT *d, int digits) {
    int n = snprintf(out, cap, "%04lld-%02lld-%02lld %02lld:%02lld:%02lld",
                     (long long)d->year, (long long)d->month, (long long)d->day,
                     (long long)d->hour, (long long)d->minute, (long long)d->second);
    if (n <= 0) return 0;
    return (size_t)n + fmt_fraction(out + n, cap - (size_t)n, d->micro, digits);
}

static size_t fmt_time(char *out, size_t cap, const DT *d, int digits) {
    int n = snprintf(out, cap, "%s%02lld:%02lld:%02lld", d->neg ? "-" : "",
                     (long long)d->hour, (long long)d->minute, (long long)d->second);
    if (n <= 0) return 0;
    return (size_t)n + fmt_fraction(out + n, cap - (size_t)n, d->micro, digits);
}

/* MySQL writes exponents without '+' or leading zeros ("1e29", "1e-7"). */
static size_t tidy_exponent(char *out, size_t n) {
    char *e = memchr(out, 'e', n);
    if (!e) return n;
    char *src = e + 1, *dst = e + 1;
    if (*src == '+') src++;
    else if (*src == '-') *dst++ = *src++;
    while (*src == '0' && src[1]) src++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
    return (size_t)(dst - out);
}

/* Shortest decimal text that reads back as the same value. */
static size_t fmt_double(char *out, size_t cap, double d, int is_float) {
    if (isnan(d)) return (size_t)snprintf(out, cap, "nan");
    if (isinf(d)) return (size_t)snprintf(out, cap, d < 0 ? "-inf" : "inf");
    int lo = is_float ? 6 : 15, hi = is_float ? 9 : 17;
    for (int prec = lo; prec <= hi; prec++) {
        int n = snprintf(out, cap, "%.*g", prec, d);
        if (n <= 0 || (size_t)n >= cap) return 0;
        double back = strtod(out, NULL);
        if (is_float ? ((float)back == (float)d) : (back == d))
            return tidy_exponent(out, (size_t)n);
    }
    int n = snprintf(out, cap, "%.17g", d);
    return n > 0 && (size_t)n < cap ? tidy_exponent(out, (size_t)n) : 0;
}

/* The cell as text: raw bytes for the text protocol and for string,
 * decimal and binary values, formatted for binary numbers and dates. */
static Iron_String cell_text(const Cell *c) {
    uint8_t t = c->col->type;
    if (!c->binary || !(is_int_type(t) || is_float_type(t) || t == T_DATE ||
                        t == T_NEWDATE || t == T_DATETIME || t == T_TIMESTAMP || t == T_TIME))
        return str_of(c->p, c->len);
    char tmp[64];
    size_t n = 0;
    if (is_int_type(t)) {
        int neg;
        uint64_t mag;
        decode_integer(c, &neg, &mag);
        n = (size_t)snprintf(tmp, sizeof(tmp), "%s%llu", neg ? "-" : "", (unsigned long long)mag);
    } else if (is_float_type(t)) {
        double d;
        decode_double(c, &d);
        n = fmt_double(tmp, sizeof(tmp), d, t == T_FLOAT);
    } else if (t == T_TIME) {
        DT d;
        if (decode_bin_time(c, &d)) return str_of("", 0);
        n = fmt_time(tmp, sizeof(tmp), &d, c->col->decimals);
    } else {
        DT d;
        if (decode_bin_datetime(c, &d)) return str_of("", 0);
        n = (t == T_DATE || t == T_NEWDATE) ? fmt_date(tmp, sizeof(tmp), &d)
                                            : fmt_datetime(tmp, sizeof(tmp), &d, c->col->decimals);
    }
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    return str_of(tmp, n);
}

/* ── Escaping ───────────────────────────────────────────────────────────── */

size_t iron_mysql_escape_bytes(char *out, const char *in, size_t len,
                               bool no_backslash_escapes) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = in[i];
        if (no_backslash_escapes) {
            if (ch == '\'') out[o++] = '\'';
            out[o++] = ch;
            continue;
        }
        char esc = 0;
        switch (ch) {
            case '\0': esc = '0'; break;
            case '\n': esc = 'n'; break;
            case '\r': esc = 'r'; break;
            case '\\': esc = '\\'; break;
            case '\'': esc = '\''; break;
            case '"': esc = '"'; break;
            case '\032': esc = 'Z'; break;
        }
        if (esc) { out[o++] = '\\'; out[o++] = esc; }
        else out[o++] = ch;
    }
    out[o] = '\0';
    return o;
}

static Iron_String escape_string(Iron_String s, int no_backslash) {
    size_t len = iron_string_byte_len(&s);
    char stack[256];
    stack[0] = '\0';
    char *buf = len * 2 + 1 <= sizeof(stack) ? stack : (char *)malloc(len * 2 + 1);
    if (!buf) return str_of("", 0);
    size_t n = iron_mysql_escape_bytes(buf, iron_string_cstr(&s), len, no_backslash != 0);
    Iron_String out = str_of(buf, n);
    if (buf != stack) free(buf);
    return out;
}

/* ── URL configuration ──────────────────────────────────────────────────── */

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Percent-decode s[0..n) into a new Iron string; 0 on a bad escape. */
static int pct_decode(const char *s, size_t n, int plus_space, Iron_String *out) {
    char stack[256];
    stack[0] = '\0';
    char *buf = n + 1 <= sizeof(stack) ? stack : (char *)malloc(n + 1);
    if (!buf) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%') {
            int hi = i + 1 < n ? hexval((unsigned char)s[i + 1]) : -1;
            int lo = i + 2 < n ? hexval((unsigned char)s[i + 2]) : -1;
            if (hi < 0 || lo < 0) { if (buf != stack) free(buf); return 0; }
            buf[o++] = (char)(hi * 16 + lo);
            i += 2;
        } else if (plus_space && s[i] == '+') {
            buf[o++] = ' ';
        } else {
            buf[o++] = s[i];
        }
    }
    *out = str_of(buf, o);
    if (buf != stack) free(buf);
    return 1;
}

static int parse_bool_text(const char *v, size_t n, bool *out) {
    if (ascii_ieq(v, n, "true", 4) || ascii_ieq(v, n, "1", 1) || ascii_ieq(v, n, "yes", 3) ||
        ascii_ieq(v, n, "on", 2)) { *out = true; return 1; }
    if (ascii_ieq(v, n, "false", 5) || ascii_ieq(v, n, "0", 1) || ascii_ieq(v, n, "no", 2) ||
        ascii_ieq(v, n, "off", 3)) { *out = false; return 1; }
    return 0;
}

static int parse_int_value(const char *v, size_t n, int64_t *out) {
    int neg;
    uint64_t mag;
    if (!parse_int_text((const uint8_t *)v, n, &neg, &mag) || neg || mag > INT64_MAX ||
        memchr(v, '.', n))
        return 0;
    *out = (int64_t)mag;
    return 1;
}

static Iron_MysqlConfig default_config(void) {
    Iron_MysqlConfig c;
    memset(&c, 0, sizeof(c));
    c.host = str_c("127.0.0.1");
    c.port = 3306;
    c.tls = IRON_MYSQL_TLS_PREFERRED;
    c.connect_timeout = 10000;
    c.max_allowed_packet = 1073741824;
    return c;
}

static void config_release(Iron_MysqlConfig *c) {
    iron_string_release(&c->host);
    iron_string_release(&c->user);
    iron_string_release(&c->password);
    iron_string_release(&c->database);
    iron_string_release(&c->ca_file);
    iron_string_release(&c->server_public_key);
    iron_string_release(&c->collation);
}

static void replace_str(Iron_String *dst, Iron_String v) {
    iron_string_release(dst);
    *dst = v;
}

static Iron_MysqlConfigResult url_error(Iron_MysqlConfigResult *r, const char *fmt, const char *detail) {
    config_release(&r->v0);
    r->v0 = default_config();
    MyErr e;
    err_setf(&e, IRON_ERR_MYSQL_BAD_URL, fmt, detail);
    r->v1 = err_to_iron(&e);
    return *r;
}

Iron_MysqlConfigResult Iron_mysqlnative_parse_url(Iron_String url_value) {
    Iron_MysqlConfigResult r;
    memset(&r, 0, sizeof(r));
    r.v0 = default_config();
    const char *url = iron_string_cstr(&url_value);
    size_t len = iron_string_byte_len(&url_value);
    size_t i;
    if (len >= 8 && ascii_ieq(url, 8, "mysql://", 8)) i = 8;
    else if (len >= 10 && ascii_ieq(url, 10, "mariadb://", 10)) i = 10;
    else return url_error(&r, "URL must start with mysql:// or mariadb://%s", "");
    if (memchr(url, '\0', len)) return url_error(&r, "URL contains a NUL byte%s", "");
    size_t auth_end = i;
    while (auth_end < len && url[auth_end] != '/' && url[auth_end] != '?') auth_end++;
    /* user:password@ (the last @ ends the user info). */
    size_t at = SIZE_MAX;
    for (size_t k = i; k < auth_end; k++) if (url[k] == '@') at = k;
    if (at != SIZE_MAX) {
        size_t colon = i;
        while (colon < at && url[colon] != ':') colon++;
        Iron_String v;
        if (!pct_decode(url + i, colon - i, 0, &v)) return url_error(&r, "bad percent escape in user%s", "");
        replace_str(&r.v0.user, v);
        if (colon < at) {
            if (!pct_decode(url + colon + 1, at - colon - 1, 0, &v))
                return url_error(&r, "bad percent escape in password%s", "");
            replace_str(&r.v0.password, v);
        }
        i = at + 1;
    }
    /* host[:port], with [v6] brackets. */
    size_t host_start = i, host_end, port_at = SIZE_MAX;
    if (i < auth_end && url[i] == '[') {
        const char *close = (const char *)memchr(url + i, ']', auth_end - i);
        if (!close) return url_error(&r, "unclosed [ in host%s", "");
        host_start = i + 1;
        host_end = (size_t)(close - url);
        size_t after = host_end + 1;
        if (after < auth_end) {
            if (url[after] != ':') return url_error(&r, "unexpected text after ]%s", "");
            port_at = after + 1;
        }
    } else {
        host_end = host_start;
        while (host_end < auth_end && url[host_end] != ':') host_end++;
        if (host_end < auth_end) port_at = host_end + 1;
    }
    if (host_end > host_start) {
        Iron_String v;
        if (!pct_decode(url + host_start, host_end - host_start, 0, &v))
            return url_error(&r, "bad percent escape in host%s", "");
        replace_str(&r.v0.host, v);
    }
    if (port_at != SIZE_MAX) {
        int64_t port;
        if (!parse_int_value(url + port_at, auth_end - port_at, &port) || port < 1 || port > 65535)
            return url_error(&r, "invalid port%s", "");
        r.v0.port = port;
    }
    i = auth_end;
    if (i < len && url[i] == '/') {
        size_t db_start = ++i;
        while (i < len && url[i] != '?') i++;
        if (memchr(url + db_start, '/', i - db_start))
            return url_error(&r, "database name may not contain '/'%s", "");
        Iron_String v;
        if (!pct_decode(url + db_start, i - db_start, 0, &v))
            return url_error(&r, "bad percent escape in database%s", "");
        replace_str(&r.v0.database, v);
    }
    if (i < len && url[i] == '?') {
        i++;
        while (i < len) {
            size_t end = i;
            while (end < len && url[end] != '&') end++;
            size_t eq = i;
            while (eq < end && url[eq] != '=') eq++;
            const char *key = url + i;
            size_t key_len = eq - i;
            Iron_String vs;
            if (!pct_decode(url + (eq < end ? eq + 1 : end), eq < end ? end - eq - 1 : 0, 1, &vs))
                return url_error(&r, "bad percent escape in option%s", "");
            const char *v = iron_string_cstr(&vs);
            size_t vn = iron_string_byte_len(&vs);
            char keybuf[48];
            if (key_len >= sizeof(keybuf)) { iron_string_release(&vs); return url_error(&r, "unknown option%s", ""); }
            for (size_t k = 0; k < key_len; k++) keybuf[k] = key[k] == '-' ? '_' : (char)tolower((unsigned char)key[k]);
            keybuf[key_len] = '\0';
            int ok = 1;
            bool bv;
            int64_t iv;
            if (key_len == 0) {
                ok = 1;
            } else if (!strcmp(keybuf, "tls") || !strcmp(keybuf, "ssl_mode") ||
                       !strcmp(keybuf, "sslmode") || !strcmp(keybuf, "ssl")) {
                if (ascii_ieq(v, vn, "disabled", 8) || ascii_ieq(v, vn, "disable", 7) ||
                    ascii_ieq(v, vn, "false", 5) || ascii_ieq(v, vn, "0", 1))
                    r.v0.tls = IRON_MYSQL_TLS_DISABLED;
                else if (ascii_ieq(v, vn, "preferred", 9) || ascii_ieq(v, vn, "prefer", 6))
                    r.v0.tls = IRON_MYSQL_TLS_PREFERRED;
                else if (ascii_ieq(v, vn, "required", 8) || ascii_ieq(v, vn, "require", 7) ||
                         ascii_ieq(v, vn, "true", 4) || ascii_ieq(v, vn, "1", 1))
                    r.v0.tls = IRON_MYSQL_TLS_REQUIRED;
                else if (ascii_ieq(v, vn, "verify_ca", 9) || ascii_ieq(v, vn, "verify-ca", 9))
                    r.v0.tls = IRON_MYSQL_TLS_VERIFY_CA;
                else if (ascii_ieq(v, vn, "verify_identity", 15) || ascii_ieq(v, vn, "verify-identity", 15) ||
                         ascii_ieq(v, vn, "verify_full", 11) || ascii_ieq(v, vn, "verify-full", 11))
                    r.v0.tls = IRON_MYSQL_TLS_VERIFY_IDENTITY;
                else ok = 0;
            } else if (!strcmp(keybuf, "ca") || !strcmp(keybuf, "ca_file") || !strcmp(keybuf, "ssl_ca")) {
                replace_str(&r.v0.ca_file, str_of(v, vn));
            } else if (!strcmp(keybuf, "server_public_key") || !strcmp(keybuf, "server_pubkey")) {
                replace_str(&r.v0.server_public_key, str_of(v, vn));
            } else if (!strcmp(keybuf, "allow_public_key_retrieval")) {
                ok = parse_bool_text(v, vn, &bv);
                if (ok) r.v0.allow_public_key_retrieval = bv;
            } else if (!strcmp(keybuf, "allow_cleartext_password")) {
                ok = parse_bool_text(v, vn, &bv);
                if (ok) r.v0.allow_cleartext_password = bv;
            } else if (!strcmp(keybuf, "multi_statements")) {
                ok = parse_bool_text(v, vn, &bv);
                if (ok) r.v0.multi_statements = bv;
            } else if (!strcmp(keybuf, "collation")) {
                replace_str(&r.v0.collation, str_of(v, vn));
            } else if (!strcmp(keybuf, "connect_timeout")) {
                ok = parse_int_value(v, vn, &iv);
                if (ok) r.v0.connect_timeout = iv;
            } else if (!strcmp(keybuf, "timeout")) {
                ok = parse_int_value(v, vn, &iv);
                if (ok) r.v0.timeout = iv;
            } else if (!strcmp(keybuf, "max_allowed_packet")) {
                ok = parse_int_value(v, vn, &iv) && iv > 0;
                if (ok) r.v0.max_allowed_packet = iv;
            } else {
                iron_string_release(&vs);
                return url_error(&r, "unknown option '%s'", keybuf);
            }
            iron_string_release(&vs);
            if (!ok) return url_error(&r, "invalid value for option '%s'", keybuf);
            i = end < len ? end + 1 : end;
        }
    }
    return r;
}

/* ── Pool ───────────────────────────────────────────────────────────────── */

struct MyPool {
    atomic_int refs;               /* Iron handle + one per pooled connection */
    MyCfg cfg;
    int64_t min, max;
    int64_t idle_timeout, acquire_timeout, health_check, max_lifetime;
    int reset_on_release;
    iron_mutex_t lock;
    iron_cond_t cond;
    MyConn **idle;                 /* LIFO: the most recently used on top */
    int nidle;
    int total;                     /* idle + in use + being opened */
    int in_use;
    int closing;
    MyErr err;
};

static void pool_unref(MyPool *p) {
    if (!p || atomic_fetch_sub(&p->refs, 1) != 1) return;
    cfg_free(&p->cfg);
    free(p->idle);
    iron_mutex_destroy(&p->lock);
    iron_cond_destroy(&p->cond);
    free(p);
}

/* Close a connection the pool no longer counts. */
static void pool_discard(MyPool *p, MyConn *c) {
    conn_shutdown(c, !c->broken);
    c->pool = NULL;
    conn_unref(c);
    pool_unref(p);
}

/* A pooled connection that the server closed while idle has its socket
 * readable (EOF, or an unsolicited ERR such as 4031 wait_timeout). */
static int conn_alive(MyConn *c) {
    if (!c->open || c->broken || c->fd < 0) return 0;
#ifdef _WIN32
    WSAPOLLFD q;
    q.fd = (SOCKET)c->fd;
    q.events = POLLIN;
    q.revents = 0;
    int rc = WSAPoll(&q, 1, 0);
#else
    struct pollfd q;
    q.fd = (int)c->fd;
    q.events = POLLIN;
    q.revents = 0;
    int rc = poll(&q, 1, 0);
#endif
    return rc == 0;
}

static MyConn *pool_take(MyPool *p, MyErr *e, int *reused) {
    *reused = 0;
    Iron_Deadline dl = deadline_after(p->acquire_timeout);
    iron_mutex_lock(&p->lock);
    for (;;) {
        if (p->closing) {
            iron_mutex_unlock(&p->lock);
            err_set(e, IRON_ERR_MYSQL_POOL_CLOSED);
            return NULL;
        }
        while (p->nidle > 0) {
            MyConn *c = p->idle[--p->nidle];
            uint64_t now = Iron_monotonic_now_ms();
            int expired = (p->max_lifetime > 0 && now - c->created_ms >= (uint64_t)p->max_lifetime) ||
                          (p->idle_timeout > 0 && p->total > p->min &&
                           now - c->idle_since_ms >= (uint64_t)p->idle_timeout);
            p->in_use++;
            iron_mutex_unlock(&p->lock);
            /* One non-blocking poll per hand-out: a connection the server
             * closed while it sat idle (KILL, wait_timeout, a restart) is
             * readable, and is replaced before the caller sees an error. */
            int ok = !expired && conn_alive(c);
            if (ok && p->health_check > 0 && now - c->idle_since_ms >= (uint64_t)p->health_check) {
                MyErr pe;
                ok = simple_command(c, COM_PING, "", 0, &pe) == 0;
            }
            if (ok) {
                *reused = 1;
                return c;
            }
            iron_mutex_lock(&p->lock);
            p->in_use--;
            p->total--;
            iron_mutex_unlock(&p->lock);
            pool_discard(p, c);
            iron_mutex_lock(&p->lock);
            if (p->closing) continue;
        }
        if (p->total < p->max) {
            p->total++;
            p->in_use++;
            iron_mutex_unlock(&p->lock);
            MyConn *c = conn_open(&p->cfg);
            if (!c || c->err.code) {
                if (c) { *e = c->err; conn_unref(c); }
                else err_set(e, IRON_ERR_MYSQL_NO_MEMORY);
                iron_mutex_lock(&p->lock);
                p->total--;
                p->in_use--;
                iron_cond_broadcast(&p->cond);
                iron_mutex_unlock(&p->lock);
                return NULL;
            }
            atomic_fetch_add(&p->refs, 1);
            c->pool = p;
            return c;
        }
        int remaining = Iron_deadline_remaining_ms(dl);
        if (remaining <= 0) {
            iron_mutex_unlock(&p->lock);
            err_setf(e, IRON_ERR_MYSQL_POOL_TIMEOUT,
                     "timed out after %lld ms waiting for a pooled connection (max %lld)",
                     (long long)p->acquire_timeout, (long long)p->max);
            return NULL;
        }
        iron_cond_timedwait_ms(&p->cond, &p->lock, remaining);
    }
}

/* Give a connection back: roll back an open transaction, optionally reset
 * the session, and keep it unless it is broken, too old, or the pool is
 * closing. Also closes the oldest idle connection when it has idled past
 * idle_timeout and the pool is above min. */
static void pool_give_back(MyConn *c) {
    MyPool *p = c->pool;
    if (c->open && !c->broken && (c->status & SERVER_STATUS_IN_TRANS)) {
        MyErr e;
        if (run_simple(c, "ROLLBACK", 8, &e)) c->broken = 1;
    }
    if (c->open && !c->broken && p->reset_on_release) {
        MyErr e;
        if (simple_command(c, COM_RESET_CONNECTION, "", 0, &e)) c->broken = 1;
        else cache_clear(c, 0);
    }
    atomic_fetch_add(&c->gen, 1);
    uint64_t now = Iron_monotonic_now_ms();
    MyConn *stale = NULL;
    iron_mutex_lock(&p->lock);
    p->in_use--;
    int keep = !p->closing && c->open && !c->broken &&
               !(p->max_lifetime > 0 && now - c->created_ms >= (uint64_t)p->max_lifetime);
    if (keep) {
        c->idle_since_ms = now;
        p->idle[p->nidle++] = c;
    } else {
        p->total--;
    }
    if (p->nidle > 1 && p->total > p->min && p->idle_timeout > 0 &&
        now - p->idle[0]->idle_since_ms >= (uint64_t)p->idle_timeout) {
        stale = p->idle[0];
        memmove(p->idle, p->idle + 1, (size_t)(p->nidle - 1) * sizeof(MyConn *));
        p->nidle--;
        p->total--;
    }
    iron_cond_broadcast(&p->cond);
    iron_mutex_unlock(&p->lock);
    if (!keep) pool_discard(p, c);
    if (stale) pool_discard(p, stale);
}

static MyResult *pool_run(MyPool *p, const char *sql, size_t len,
                          const Iron_MysqlValueList *params) {
    for (int attempt = 0; attempt < 2; attempt++) {
        MyErr e;
        int reused;
        MyConn *c = pool_take(p, &e, &reused);
        if (!c) {
            MyResult *res = result_new();
            if (!res) return NULL;
            res->err = e;
            result_add_set(res);
            return res;
        }
        MyResult *res = params ? conn_exec(c, sql, len, params) : do_query(c, sql, len);
        /* A reused connection whose write failed never delivered the
         * statement: retry once on another connection. */
        int retry = res && reused && c->broken && c->send_failed && c->cmd_rx == 0 && attempt == 0;
        pool_give_back(c);
        if (!retry) return res;
        result_destroy(res);
    }
    return NULL;
}

static void pool_shutdown(MyPool *p) {
    iron_mutex_lock(&p->lock);
    p->closing = 1;
    int n = p->nidle;
    MyConn **conns = n ? (MyConn **)malloc((size_t)n * sizeof(MyConn *)) : NULL;
    if (conns) memcpy(conns, p->idle, (size_t)n * sizeof(MyConn *));
    else n = 0;
    p->total -= n;
    p->nidle -= n;
    iron_cond_broadcast(&p->cond);
    iron_mutex_unlock(&p->lock);
    for (int i = 0; i < n; i++) pool_discard(p, conns[i]);
    free(conns);
}

/* ── Iron entry points: connections ─────────────────────────────────────── */

#define AS_CONN(h) ((MyConn *)(intptr_t)(h))
#define AS_RES(h) ((MyResult *)(intptr_t)(h))
#define AS_STMT(h) ((MyStmt *)(intptr_t)(h))
#define AS_POOL(h) ((MyPool *)(intptr_t)(h))
#define HANDLE(p) ((int64_t)(intptr_t)(p))


static int64_t error_conn(int64_t code, const MyErr *e) {
    MyConn *c = conn_new();
    if (!c) return 0;
    if (e) c->err = *e;
    else err_set(&c->err, code);
    return HANDLE(c);
}

int64_t Iron_mysqlnative_connect(Iron_MysqlConfig config) {
    MyCfg k;
    if (!cfg_from_iron(&k, &config)) return error_conn(IRON_ERR_MYSQL_NO_MEMORY, NULL);
    MyConn *c = conn_open(&k);
    cfg_free(&k);
    return HANDLE(c);
}

Iron_MysqlError Iron_mysqlnative_conn_error(int64_t h) {
    MyConn *c = AS_CONN(h);
    if (!c) return iron_err_code(IRON_ERR_MYSQL_NO_MEMORY);
    return err_to_iron(&c->err);
}

void Iron_mysqlnative_conn_free(int64_t h) {
    MyConn *c = AS_CONN(h);
    if (!c) return;
    if (c->pool) { pool_give_back(c); return; }
    conn_shutdown(c, 1);
    conn_unref(c);
}

void Iron_mysqlnative_conn_close(int64_t h) {
    MyConn *c = AS_CONN(h);
    if (!c) return;
    if (c->pool) {
        /* Close it now; the pool forgets it when it is released. */
        conn_shutdown(c, !c->broken);
        c->broken = 1;
        return;
    }
    conn_shutdown(c, 1);
}

void Iron_mysqlnative_conn_set(int64_t h, int64_t what, int64_t value) {
    MyConn *c = AS_CONN(h);
    if (!c) return;
    if (what == 0) c->timeout_ms = value > 0 ? value : 0;
}

int64_t Iron_mysqlnative_conn_info(int64_t h, int64_t what) {
    MyConn *c = AS_CONN(h);
    if (!c) return 0;
    switch (what) {
        case 0: return c->open && !c->broken;
        case 1: return (c->status & SERVER_STATUS_IN_TRANS) != 0;
        case 2: return c->thread_id;
        case 3: return c->tls != NULL;
        default: return 0;
    }
}

Iron_String Iron_mysqlnative_server_version(int64_t h) {
    MyConn *c = AS_CONN(h);
    return str_c(c ? c->version : "");
}

Iron_String Iron_mysqlnative_auth_plugin(int64_t h) {
    MyConn *c = AS_CONN(h);
    return str_c(c ? c->plugin : "");
}

static int64_t closed_result(int64_t code) {
    MyResult *res = result_new();
    if (!res) return 0;
    err_set(&res->err, code);
    result_add_set(res);
    return HANDLE(res);
}

int64_t Iron_mysqlnative_query(int64_t h, Iron_String sql) {
    MyConn *c = AS_CONN(h);
    if (!c) return closed_result(IRON_ERR_MYSQL_CLOSED);
    MyResult *res = do_query(c, iron_string_cstr(&sql), iron_string_byte_len(&sql));
    if (res && res->err.code) c->err = res->err;
    return HANDLE(res);
}

int64_t Iron_mysqlnative_exec(int64_t h, Iron_String sql, Iron_MysqlValueList params) {
    MyConn *c = AS_CONN(h);
    if (!c) return closed_result(IRON_ERR_MYSQL_CLOSED);
    MyResult *res = conn_exec(c, iron_string_cstr(&sql), iron_string_byte_len(&sql), &params);
    if (res && res->err.code) c->err = res->err;
    return HANDLE(res);
}

Iron_MysqlError Iron_mysqlnative_run(int64_t h, Iron_String sql) {
    MyConn *c = AS_CONN(h);
    if (!c) return iron_err_code(IRON_ERR_MYSQL_CLOSED);
    MyErr e;
    err_clear(&e);
    run_simple(c, iron_string_cstr(&sql), iron_string_byte_len(&sql), &e);
    if (e.code) c->err = e;
    return err_to_iron(&e);
}

Iron_MysqlError Iron_mysqlnative_command(int64_t h, int64_t what, Iron_String arg) {
    MyConn *c = AS_CONN(h);
    if (!c) return iron_err_code(IRON_ERR_MYSQL_CLOSED);
    MyErr e;
    err_clear(&e);
    switch (what) {
        case 0: simple_command(c, COM_PING, "", 0, &e); break;
        case 1:
            if (simple_command(c, COM_RESET_CONNECTION, "", 0, &e) == 0) cache_clear(c, 0);
            break;
        case 2:
            simple_command(c, COM_INIT_DB, iron_string_cstr(&arg), iron_string_byte_len(&arg), &e);
            break;
        default: err_set(&e, IRON_ERR_MYSQL_INVALID_ARGUMENT);
    }
    if (e.code) c->err = e;
    return err_to_iron(&e);
}

Iron_String Iron_mysqlnative_escape(int64_t h, Iron_String s) {
    MyConn *c = AS_CONN(h);
    int nb = c && (c->status & SERVER_STATUS_NO_BACKSLASH_ESCAPES);
    return escape_string(s, nb);
}

Iron_String Iron_mysqlnative_quote_identifier(Iron_String name) {
    size_t len = iron_string_byte_len(&name);
    const char *in = iron_string_cstr(&name);
    MyBuf b = { 0 };
    int ok = buf_u8(&b, '`');
    for (size_t i = 0; ok && i < len; i++) {
        if (in[i] == '`') ok = buf_u8(&b, '`');
        if (ok) ok = buf_u8(&b, (uint8_t)in[i]);
    }
    ok = ok && buf_u8(&b, '`');
    Iron_String out = ok ? str_of(b.p, b.len) : str_of("", 0);
    buf_free(&b);
    return out;
}

/* ── Iron entry points: statements ──────────────────────────────────────── */

int64_t Iron_mysqlnative_prepare(int64_t h, Iron_String sql) {
    MyConn *c = AS_CONN(h);
    MyStmt *st = (MyStmt *)calloc(1, sizeof(MyStmt));
    if (!st) return 0;
    st->closed = 1;
    if (!c) { err_set(&st->err, IRON_ERR_MYSQL_CLOSED); return HANDLE(st); }
    if (prepare_stmt(c, iron_string_cstr(&sql), iron_string_byte_len(&sql), &st->prep, &st->err) == 0) {
        st->conn = c;
        st->gen = atomic_load(&c->gen);
        st->closed = 0;
        atomic_fetch_add(&c->refs, 1);
    } else {
        c->err = st->err;
    }
    return HANDLE(st);
}

Iron_MysqlError Iron_mysqlnative_stmt_error(int64_t h) {
    MyStmt *st = AS_STMT(h);
    if (!st) return iron_err_code(IRON_ERR_MYSQL_NO_MEMORY);
    return err_to_iron(&st->err);
}

static int stmt_usable(MyStmt *st, MyErr *e) {
    if (!st || st->closed || !st->conn || atomic_load(&st->conn->gen) != st->gen) {
        err_set(e, IRON_ERR_MYSQL_STMT_CLOSED);
        return 0;
    }
    return 1;
}

int64_t Iron_mysqlnative_stmt_exec(int64_t h, Iron_MysqlValueList params) {
    MyStmt *st = AS_STMT(h);
    MyResult *res = result_new();
    if (!res) return 0;
    if (stmt_usable(st, &res->err)) execute_stmt(st->conn, &st->prep, &params, res);
    if (res->nsets == 0) result_add_set(res);
    return HANDLE(res);
}

Iron_MysqlError Iron_mysqlnative_stmt_reset(int64_t h) {
    MyStmt *st = AS_STMT(h);
    MyErr e;
    err_clear(&e);
    if (stmt_usable(st, &e)) {
        uint8_t id[4] = { (uint8_t)st->prep.id, (uint8_t)(st->prep.id >> 8),
                          (uint8_t)(st->prep.id >> 16), (uint8_t)(st->prep.id >> 24) };
        simple_command(st->conn, COM_STMT_RESET, (const char *)id, 4, &e);
    }
    return err_to_iron(&e);
}

static void stmt_close(MyStmt *st) {
    if (!st || st->closed) return;
    st->closed = 1;
    if (st->conn) {
        if (atomic_load(&st->conn->gen) == st->gen && st->conn->open && !st->conn->broken)
            queue_stmt_close(st->conn, st->prep.id);
        conn_unref(st->conn);
        st->conn = NULL;
    }
}

void Iron_mysqlnative_stmt_close(int64_t h) { stmt_close(AS_STMT(h)); }

void Iron_mysqlnative_stmt_free(int64_t h) {
    MyStmt *st = AS_STMT(h);
    if (!st) return;
    stmt_close(st);
    prep_free(&st->prep);
    free(st);
}

int64_t Iron_mysqlnative_stmt_info(int64_t h, int64_t what) {
    MyStmt *st = AS_STMT(h);
    if (!st) return 0;
    return what == 0 ? st->prep.nparams : what == 1 ? st->prep.ncols : 0;
}

/* ── Iron entry points: results ─────────────────────────────────────────── */

Iron_MysqlError Iron_mysqlnative_res_error(int64_t h) {
    MyResult *r = AS_RES(h);
    if (!r) return iron_err_code(IRON_ERR_MYSQL_NO_MEMORY);
    return err_to_iron(&r->err);
}

void Iron_mysqlnative_res_free(int64_t h) { result_destroy(AS_RES(h)); }

int64_t Iron_mysqlnative_res_info(int64_t h, int64_t what) {
    MyResult *r = AS_RES(h);
    MySet *s = cur_set(r);
    switch (what) {
        case 0: return s ? (int64_t)s->nrows : 0;
        case 1: return s ? (int64_t)s->ncols : 0;
        case 2: return s ? (int64_t)s->affected : 0;
        case 3: return s ? (int64_t)s->insert_id : 0;
        case 4: return s ? (int64_t)s->warnings : 0;
        case 5: return r ? (int64_t)r->nsets : 0;
        case 6: return r ? (int64_t)r->cur : 0;
        default: return 0;
    }
}

Iron_String Iron_mysqlnative_res_info_text(int64_t h) {
    MySet *s = cur_set(AS_RES(h));
    return str_c(s && s->info ? s->info : "");
}

bool Iron_mysqlnative_res_next(int64_t h) {
    MyResult *r = AS_RES(h);
    if (!r || r->cur + 1 >= r->nsets) return false;
    r->cur++;
    return true;
}

static const char *type_name(uint8_t type, uint16_t flags, uint16_t charset) {
    int uns = (flags & FLAG_UNSIGNED) != 0;
    switch (type) {
        case T_DECIMAL: case T_NEWDECIMAL: return "DECIMAL";
        case T_TINY: return uns ? "TINYINT UNSIGNED" : "TINYINT";
        case T_SHORT: return uns ? "SMALLINT UNSIGNED" : "SMALLINT";
        case T_LONG: return uns ? "INT UNSIGNED" : "INT";
        case T_INT24: return uns ? "MEDIUMINT UNSIGNED" : "MEDIUMINT";
        case T_LONGLONG: return uns ? "BIGINT UNSIGNED" : "BIGINT";
        case T_FLOAT: return "FLOAT";
        case T_DOUBLE: return "DOUBLE";
        case T_NULL: return "NULL";
        case T_TIMESTAMP: case T_TIMESTAMP2: return "TIMESTAMP";
        case T_DATE: case T_NEWDATE: return "DATE";
        case T_TIME: case T_TIME2: return "TIME";
        case T_DATETIME: case T_DATETIME2: return "DATETIME";
        case T_YEAR: return "YEAR";
        case T_BIT: return "BIT";
        case T_JSON: return "JSON";
        case T_ENUM: return "ENUM";
        case T_SET: return "SET";
        case T_GEOMETRY: return "GEOMETRY";
        case T_VECTOR: return "VECTOR";
        case T_TINY_BLOB: case T_MEDIUM_BLOB: case T_LONG_BLOB: case T_BLOB:
            return charset == COLLATION_BINARY ? "BLOB" : "TEXT";
        case T_VARCHAR: case T_VAR_STRING:
            return charset == COLLATION_BINARY ? "VARBINARY" : "VARCHAR";
        case T_STRING:
            if (flags & 256) return "ENUM";
            if (flags & 2048) return "SET";
            return charset == COLLATION_BINARY ? "BINARY" : "CHAR";
        default: return "UNKNOWN";
    }
}

Iron_MysqlColumn Iron_mysqlnative_res_column(int64_t h, int64_t index) {
    Iron_MysqlColumn out;
    memset(&out, 0, sizeof(out));
    MySet *s = cur_set(AS_RES(h));
    if (!s || index < 0 || (uint64_t)index >= s->ncols) {
        out.type_name = str_c("");
        return out;
    }
    const MyCol *c = &s->cols[index];
    const uint8_t *m = set_strings(s);
    out.name = str_of(m + c->name, c->name_len);
    out.org_name = str_of(m + c->org_name, c->org_name_len);
    out.table = str_of(m + c->table, c->table_len);
    out.org_table = str_of(m + c->org_table, c->org_table_len);
    out.schema = str_of(m + c->schema, c->schema_len);
    out.type = c->type;
    out.type_name = str_c(type_name(c->type, c->flags, c->charset));
    out.flags = c->flags;
    out.decimals = c->decimals;
    out.length = c->length;
    out.charset = c->charset;
    return out;
}

int64_t Iron_mysqlnative_res_col(int64_t h, Iron_String name) {
    MySet *s = cur_set(AS_RES(h));
    if (!s) return -1;
    const char *n = iron_string_cstr(&name);
    size_t nl = iron_string_byte_len(&name);
    for (uint32_t i = 0; i < s->ncols; i++) {
        const MyCol *c = &s->cols[i];
        if (c->name_len == nl && memcmp(set_strings(s) + c->name, n, nl) == 0) return i;
    }
    for (uint32_t i = 0; i < s->ncols; i++) {
        const MyCol *c = &s->cols[i];
        if (ascii_ieq((const char *)set_strings(s) + c->name, c->name_len, n, nl)) return i;
    }
    return -1;
}

bool Iron_mysqlnative_res_is_null(int64_t h, int64_t row, int64_t col) {
    Cell c;
    return get_cell(AS_RES(h), row, col, &c) == IRON_ERR_MYSQL_NULL;
}

static Iron_MysqlError cell_error(int code, int64_t row, int64_t col) {
    MyErr e;
    if (code == IRON_ERR_MYSQL_RANGE)
        err_setf(&e, code, "no cell at row %lld, column %lld", (long long)row, (long long)col);
    else
        err_set(&e, code);
    return err_to_iron(&e);
}

Iron_MysqlIntResult Iron_mysqlnative_res_int(int64_t h, int64_t row, int64_t col) {
    Iron_MysqlIntResult out;
    memset(&out, 0, sizeof(out));
    Cell c;
    int neg = 0;
    uint64_t mag = 0;
    int rc = get_cell(AS_RES(h), row, col, &c);
    if (!rc) rc = decode_integer(&c, &neg, &mag);
    if (!rc && (neg ? mag > (uint64_t)INT64_MAX + 1 : mag > (uint64_t)INT64_MAX))
        rc = IRON_ERR_MYSQL_CONVERSION;
    if (rc) { out.v1 = cell_error(rc, row, col); return out; }
    out.v0 = neg ? (int64_t)(0 - mag) : (int64_t)mag;
    return out;
}

Iron_MysqlUIntResult Iron_mysqlnative_res_uint(int64_t h, int64_t row, int64_t col) {
    Iron_MysqlUIntResult out;
    memset(&out, 0, sizeof(out));
    Cell c;
    int neg = 0;
    uint64_t mag = 0;
    int rc = get_cell(AS_RES(h), row, col, &c);
    if (!rc) rc = decode_integer(&c, &neg, &mag);
    if (!rc && neg && mag != 0) rc = IRON_ERR_MYSQL_CONVERSION;
    if (rc) { out.v1 = cell_error(rc, row, col); return out; }
    out.v0 = mag;
    return out;
}

Iron_MysqlFloatResult Iron_mysqlnative_res_float(int64_t h, int64_t row, int64_t col) {
    Iron_MysqlFloatResult out;
    memset(&out, 0, sizeof(out));
    Cell c;
    int rc = get_cell(AS_RES(h), row, col, &c);
    if (!rc) rc = decode_double(&c, &out.v0);
    if (rc) { out.v0 = 0; out.v1 = cell_error(rc, row, col); }
    return out;
}

Iron_MysqlStringResult Iron_mysqlnative_res_text(int64_t h, int64_t row, int64_t col) {
    Iron_MysqlStringResult out;
    memset(&out, 0, sizeof(out));
    Cell c;
    int rc = get_cell(AS_RES(h), row, col, &c);
    if (rc) { out.v1 = cell_error(rc, row, col); return out; }
    out.v0 = cell_text(&c);
    return out;
}

Iron_MysqlBoolResult Iron_mysqlnative_res_bool(int64_t h, int64_t row, int64_t col) {
    Iron_MysqlBoolResult out;
    memset(&out, 0, sizeof(out));
    Cell c;
    int neg = 0;
    uint64_t mag = 0;
    int rc = get_cell(AS_RES(h), row, col, &c);
    if (!rc) {
        rc = decode_integer(&c, &neg, &mag);
        if (rc == IRON_ERR_MYSQL_CONVERSION) {
            if (ascii_ieq((const char *)c.p, c.len, "true", 4)) { rc = 0; mag = 1; }
            else if (ascii_ieq((const char *)c.p, c.len, "false", 5)) { rc = 0; mag = 0; }
        }
    }
    if (rc) { out.v1 = cell_error(rc, row, col); return out; }
    out.v0 = mag != 0;
    return out;
}

static Iron_MysqlDateTime dt_to_iron(const DT *d) {
    Iron_MysqlDateTime v;
    v.year = d->year; v.month = d->month; v.day = d->day;
    v.hour = d->hour; v.minute = d->minute; v.second = d->second;
    v.microsecond = d->micro;
    return v;
}

static Iron_MysqlTime time_to_iron(const DT *d) {
    Iron_MysqlTime v;
    memset(&v, 0, sizeof(v));
    v.negative = d->neg != 0;
    v.hours = d->hour; v.minutes = d->minute; v.seconds = d->second;
    v.microsecond = d->micro;
    return v;
}

Iron_MysqlDateTimeResult Iron_mysqlnative_res_datetime(int64_t h, int64_t row, int64_t col) {
    Iron_MysqlDateTimeResult out;
    memset(&out, 0, sizeof(out));
    Cell c;
    DT d;
    int rc = get_cell(AS_RES(h), row, col, &c);
    if (!rc) rc = decode_datetime(&c, &d);
    if (rc) { out.v1 = cell_error(rc, row, col); return out; }
    out.v0 = dt_to_iron(&d);
    return out;
}

Iron_MysqlTimeResult Iron_mysqlnative_res_time(int64_t h, int64_t row, int64_t col) {
    Iron_MysqlTimeResult out;
    memset(&out, 0, sizeof(out));
    Cell c;
    DT d;
    int rc = get_cell(AS_RES(h), row, col, &c);
    if (!rc) rc = decode_time(&c, &d);
    if (rc) { out.v1 = cell_error(rc, row, col); return out; }
    out.v0 = time_to_iron(&d);
    return out;
}

Iron_MysqlValue Iron_mysqlnative_res_value(int64_t h, int64_t row, int64_t col) {
    Iron_MysqlValue v;
    memset(&v, 0, sizeof(v));
    v.tag = Iron_MysqlValue_TAG_Null;
    Cell c;
    if (get_cell(AS_RES(h), row, col, &c)) return v;
    uint8_t t = c.col->type;
    int uns = (c.col->flags & FLAG_UNSIGNED) != 0;
    if (is_int_type(t) || t == T_BIT) {
        int neg;
        uint64_t mag;
        if (decode_integer(&c, &neg, &mag)) return v;
        if ((t == T_LONGLONG && uns) || t == T_BIT) {
            v.tag = Iron_MysqlValue_TAG_UInt;
            v.data.UInt._0 = mag;
        } else {
            v.tag = Iron_MysqlValue_TAG_Int;
            v.data.Int._0 = neg ? (int64_t)(0 - mag) : (int64_t)mag;
        }
    } else if (is_float_type(t)) {
        double d;
        if (decode_double(&c, &d)) return v;
        v.tag = Iron_MysqlValue_TAG_Float;
        v.data.Float._0 = d;
    } else if (t == T_DECIMAL || t == T_NEWDECIMAL) {
        v.tag = Iron_MysqlValue_TAG_Decimal;
        v.data.Decimal._0 = str_of(c.p, c.len);
    } else if (t == T_DATE || t == T_NEWDATE || t == T_DATETIME || t == T_TIMESTAMP) {
        DT d;
        if (decode_datetime(&c, &d)) return v;
        v.tag = (t == T_DATE || t == T_NEWDATE) ? Iron_MysqlValue_TAG_Date
                                                : Iron_MysqlValue_TAG_DateTime;
        if (v.tag == Iron_MysqlValue_TAG_Date) v.data.Date._0 = dt_to_iron(&d);
        else v.data.DateTime._0 = dt_to_iron(&d);
    } else if (t == T_TIME) {
        DT d;
        if (decode_time(&c, &d)) return v;
        v.tag = Iron_MysqlValue_TAG_Time;
        v.data.Time._0 = time_to_iron(&d);
    } else if (t == T_JSON || c.col->charset != COLLATION_BINARY) {
        v.tag = Iron_MysqlValue_TAG_Text;
        v.data.Text._0 = str_of(c.p, c.len);
    } else {
        v.tag = Iron_MysqlValue_TAG_Bytes;
        v.data.Bytes._0 = str_of(c.p, c.len);
    }
    return v;
}

static DT dt_from_iron(const Iron_MysqlDateTime *v) {
    DT d;
    memset(&d, 0, sizeof(d));
    d.year = v->year; d.month = v->month; d.day = v->day;
    d.hour = v->hour; d.minute = v->minute; d.second = v->second;
    d.micro = v->microsecond;
    return d;
}

Iron_String Iron_mysqlnative_format_datetime(Iron_MysqlDateTime v) {
    char tmp[96];
    DT d = dt_from_iron(&v);
    size_t n = fmt_datetime(tmp, sizeof(tmp), &d, d.micro ? 6 : 0);
    return str_of(tmp, n < sizeof(tmp) ? n : sizeof(tmp) - 1);
}

Iron_String Iron_mysqlnative_format_date(Iron_MysqlDateTime v) {
    char tmp[64];
    DT d = dt_from_iron(&v);
    size_t n = fmt_date(tmp, sizeof(tmp), &d);
    return str_of(tmp, n < sizeof(tmp) ? n : sizeof(tmp) - 1);
}

Iron_String Iron_mysqlnative_format_time(Iron_MysqlTime v) {
    char tmp[96];
    DT d;
    memset(&d, 0, sizeof(d));
    d.neg = v.negative;
    d.hour = v.hours; d.minute = v.minutes; d.second = v.seconds;
    d.micro = v.microsecond;
    size_t n = fmt_time(tmp, sizeof(tmp), &d, d.micro ? 6 : 0);
    return str_of(tmp, n < sizeof(tmp) ? n : sizeof(tmp) - 1);
}

/* ── Iron entry points: pools ───────────────────────────────────────────── */

int64_t Iron_mysqlnative_pool_open(Iron_MysqlConfig config, Iron_MysqlPoolOptions options) {
    MyPool *p = (MyPool *)calloc(1, sizeof(MyPool));
    if (!p) return 0;
    atomic_init(&p->refs, 1);
    iron_mutex_init(&p->lock);
    iron_cond_init(&p->cond);
    p->closing = 1;
    if (options.max <= 0 || options.max > 4096 || options.min < 0 || options.min > options.max) {
        err_setf(&p->err, IRON_ERR_MYSQL_INVALID_ARGUMENT,
                 "pool sizes must satisfy 0 <= min <= max, 1 <= max <= 4096");
        return HANDLE(p);
    }
    if (!cfg_from_iron(&p->cfg, &config)) {
        err_set(&p->err, IRON_ERR_MYSQL_NO_MEMORY);
        return HANDLE(p);
    }
    p->idle = (MyConn **)calloc((size_t)options.max, sizeof(MyConn *));
    if (!p->idle) {
        err_set(&p->err, IRON_ERR_MYSQL_NO_MEMORY);
        return HANDLE(p);
    }
    p->min = options.min;
    p->max = options.max;
    p->idle_timeout = options.idle_timeout;
    p->acquire_timeout = options.acquire_timeout;
    p->health_check = options.health_check;
    p->max_lifetime = options.max_lifetime;
    p->reset_on_release = options.reset_on_release;
    p->closing = 0;
    /* Open min connections now so a bad configuration fails here. */
    for (int64_t i = 0; i < p->min; i++) {
        MyConn *c = conn_open(&p->cfg);
        if (!c || c->err.code) {
            if (c) { p->err = c->err; conn_unref(c); }
            else err_set(&p->err, IRON_ERR_MYSQL_NO_MEMORY);
            pool_shutdown(p);
            return HANDLE(p);
        }
        atomic_fetch_add(&p->refs, 1);
        c->pool = p;
        iron_mutex_lock(&p->lock);
        p->idle[p->nidle++] = c;
        p->total++;
        iron_mutex_unlock(&p->lock);
    }
    return HANDLE(p);
}

Iron_MysqlError Iron_mysqlnative_pool_error(int64_t h) {
    MyPool *p = AS_POOL(h);
    if (!p) return iron_err_code(IRON_ERR_MYSQL_NO_MEMORY);
    return err_to_iron(&p->err);
}

int64_t Iron_mysqlnative_pool_acquire(int64_t h) {
    MyPool *p = AS_POOL(h);
    if (!p) return error_conn(IRON_ERR_MYSQL_POOL_CLOSED, NULL);
    MyErr e;
    int reused;
    MyConn *c = pool_take(p, &e, &reused);
    if (!c) return error_conn(0, &e);
    err_clear(&c->err);
    return HANDLE(c);
}

int64_t Iron_mysqlnative_pool_query(int64_t h, Iron_String sql) {
    MyPool *p = AS_POOL(h);
    if (!p) return closed_result(IRON_ERR_MYSQL_POOL_CLOSED);
    return HANDLE(pool_run(p, iron_string_cstr(&sql), iron_string_byte_len(&sql), NULL));
}

int64_t Iron_mysqlnative_pool_exec(int64_t h, Iron_String sql, Iron_MysqlValueList params) {
    MyPool *p = AS_POOL(h);
    if (!p) return closed_result(IRON_ERR_MYSQL_POOL_CLOSED);
    return HANDLE(pool_run(p, iron_string_cstr(&sql), iron_string_byte_len(&sql), &params));
}

void Iron_mysqlnative_pool_close(int64_t h) {
    MyPool *p = AS_POOL(h);
    if (p) pool_shutdown(p);
}

void Iron_mysqlnative_pool_free(int64_t h) {
    MyPool *p = AS_POOL(h);
    if (!p) return;
    pool_shutdown(p);
    pool_unref(p);
}

int64_t Iron_mysqlnative_pool_info(int64_t h, int64_t what) {
    MyPool *p = AS_POOL(h);
    if (!p) return 0;
    iron_mutex_lock(&p->lock);
    int64_t v = what == 0 ? p->total : what == 1 ? p->nidle : what == 2 ? p->in_use : 0;
    iron_mutex_unlock(&p->lock);
    return v;
}

/* ── In-process protocol transport ─────────────────────────────────────── */

static void test_transport_init(MyConn *c, const uint8_t *wire, size_t wire_len,
                                size_t read_size) {
    c->test_transport = 1;
    c->test_in = wire;
    c->test_in_len = wire_len;
    c->test_read_size = read_size ? read_size : wire_len;
    c->deadline = deadline_after(5000);
}

Iron_MysqlError iron_mysql_test_read_packet(const uint8_t *wire, size_t wire_len,
                                            size_t read_size, uint8_t first_sequence,
                                            uint64_t max_packet,
                                            Iron_MysqlTestBytes *payload) {
    if (payload) memset(payload, 0, sizeof(*payload));
    MyConn *c = conn_new();
    if (!c) return iron_err_code(IRON_ERR_MYSQL_NO_MEMORY);
    test_transport_init(c, wire, wire_len, read_size);
    c->seq = first_sequence;
    c->max_packet = max_packet;
    MyErr e;
    err_clear(&e);
    const uint8_t *p = NULL;
    size_t n = 0;
    int rc = conn_read_packet(c, &p, &n, &e);
    if (!rc && payload) {
        payload->data = (uint8_t *)malloc(n ? n : 1);
        if (!payload->data) err_set(&e, IRON_ERR_MYSQL_NO_MEMORY);
        else {
            if (n) memcpy(payload->data, p, n);
            payload->len = n;
        }
    }
    Iron_MysqlError out = err_to_iron(&e);
    conn_unref(c);
    return out;
}

Iron_MysqlError iron_mysql_test_decode_lenenc(const uint8_t *encoded, size_t encoded_len,
                                              uint64_t *value, bool *is_null,
                                              size_t *consumed) {
    Rd r = { encoded, encoded_len, 0, 0 };
    int null_value = 0;
    uint64_t v = rd_lenenc(&r, &null_value);
    if (value) *value = v;
    if (is_null) *is_null = null_value != 0;
    if (consumed) *consumed = r.i;
    if (!r.bad) return err_to_iron(NULL);
    return iron_err_code(IRON_ERR_MYSQL_PROTOCOL);
}

Iron_MysqlError iron_mysql_test_encode_lenenc(uint64_t value,
                                              Iron_MysqlTestBytes *encoded) {
    if (encoded) memset(encoded, 0, sizeof(*encoded));
    MyBuf b = { 0 };
    if (!buf_lenenc(&b, value)) return iron_err_code(IRON_ERR_MYSQL_NO_MEMORY);
    if (encoded) {
        encoded->data = b.p;
        encoded->len = b.len;
    } else {
        buf_free(&b);
    }
    return err_to_iron(NULL);
}

int64_t iron_mysql_test_decode_result(const uint8_t *wire, size_t wire_len,
                                      size_t read_size, uint32_t capabilities,
                                      uint8_t first_sequence, bool binary) {
    MyResult *res = result_new();
    MyConn *c = conn_new();
    if (!res || !c) {
        if (res) result_destroy(res);
        if (c) conn_unref(c);
        return HANDLE(closed_result(IRON_ERR_MYSQL_NO_MEMORY));
    }
    test_transport_init(c, wire, wire_len, read_size);
    c->open = 1;
    c->caps = capabilities;
    c->seq = first_sequence;
    (void)read_results(c, res, binary ? 1 : 0, 0, NULL);
    conn_unref(c);
    return HANDLE(res);
}

Iron_MysqlError iron_mysql_test_handshake(const uint8_t *wire, size_t wire_len,
                                          size_t read_size,
                                          Iron_MysqlConfig config,
                                          Iron_MysqlTestBytes *written) {
    if (written) memset(written, 0, sizeof(*written));
    MyCfg k;
    if (!cfg_from_iron(&k, &config)) return iron_err_code(IRON_ERR_MYSQL_NO_MEMORY);
    MyConn *c = conn_new();
    if (!c) {
        cfg_free(&k);
        return iron_err_code(IRON_ERR_MYSQL_NO_MEMORY);
    }
    test_transport_init(c, wire, wire_len, read_size);
    c->timeout_ms = k.timeout;
    c->max_packet = k.max_packet > 0 ? (uint64_t)k.max_packet : 1073741824u;
    MyErr e;
    err_clear(&e);
    (void)conn_handshake(c, &k, &e);
    if (written) {
        written->data = c->test_out.p;
        written->len = c->test_out.len;
        memset(&c->test_out, 0, sizeof(c->test_out));
    }
    Iron_MysqlError out = err_to_iron(&e);
    conn_unref(c);
    cfg_free(&k);
    return out;
}

Iron_MysqlError iron_mysql_test_encode_params(const Iron_MysqlValueList *params,
                                              Iron_MysqlTestBytes *encoded) {
    if (encoded) memset(encoded, 0, sizeof(*encoded));
    MyErr e;
    err_clear(&e);
    if (!params || params->count < 0 || params->count > UINT16_MAX ||
        (params->count && !params->items)) {
        err_set(&e, IRON_ERR_MYSQL_INVALID_ARGUMENT);
        return err_to_iron(&e);
    }
    MyBuf out = { 0 }, values = { 0 };
    size_t count = (size_t)params->count;
    size_t bitmap_len = (count + 7) / 8;
    if (!buf_reserve(&out, bitmap_len + 1 + count * 2)) goto oom;
    memset(out.p, 0, bitmap_len);
    out.len = bitmap_len;
    if (!buf_u8(&out, 1)) goto oom;             /* new-params-bound flag */
    size_t types_at = out.len;
    for (size_t i = 0; i < count; i++) if (!buf_u16(&out, 0)) goto oom;
    for (size_t i = 0; i < count; i++) {
        uint8_t type, flag;
        int ok = bind_value(&values, &params->items[i], &type, &flag);
        if (ok < 0) {
            err_setf(&e, IRON_ERR_MYSQL_INVALID_ARGUMENT,
                     "parameter %u has an out of range date or time", (unsigned)i + 1);
            goto done;
        }
        if (!ok) goto oom;
        if (params->items[i].tag == Iron_MysqlValue_TAG_Null)
            out.p[i / 8] |= (uint8_t)(1u << (i % 8));
        out.p[types_at + 2 * i] = type;
        out.p[types_at + 2 * i + 1] = flag;
    }
    if (!buf_put(&out, values.p, values.len)) goto oom;
    if (encoded) {
        encoded->data = out.p;
        encoded->len = out.len;
        memset(&out, 0, sizeof(out));
    }
    goto done;
oom:
    err_set(&e, IRON_ERR_MYSQL_NO_MEMORY);
done:
    buf_free(&out);
    buf_free(&values);
    return err_to_iron(&e);
}

void iron_mysql_test_bytes_free(Iron_MysqlTestBytes *bytes) {
    if (!bytes) return;
    free(bytes->data);
    bytes->data = NULL;
    bytes->len = 0;
}
