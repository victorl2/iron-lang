/* test_stdlib_mysql.c -- the MySQL client without a MySQL server.
 *
 * The pure parts (scrambles, escaping, URL parsing, value formatting) are
 * checked directly. The protocol is checked against a scripted fake
 * server on 127.0.0.1 that speaks just enough of the server side for each
 * scenario: handshakes with each auth plugin, auth switches, result sets,
 * multi-packet rows, and the malformed or truncated replies a client must
 * survive with an error value. Live-server coverage is tests/mysql/run.sh.
 */
#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_errors.h"
#include "stdlib/iron_mysql.h"
#include "stdlib/iron_net.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef HANDLE SRV_THREAD;
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <pthread.h>
  #include <sys/socket.h>
  typedef pthread_t SRV_THREAD;
#endif

void setUp(void) { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

static Iron_String S(const char *s) { return iron_string_from_cstr(s, strlen(s)); }

static int str_eq(Iron_String s, const char *want) {
    size_t n = iron_string_byte_len(&s);
    return n == strlen(want) && memcmp(iron_string_cstr(&s), want, n) == 0;
}

static void hex(const uint8_t *p, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]);
}

/* ── Pure helpers ───────────────────────────────────────────────────────── */

static void test_scrambles_match_reference_vectors(void) {
    uint8_t scramble[20];
    for (int i = 0; i < 20; i++) scramble[i] = (uint8_t)(i + 1);
    uint8_t out[32];
    char text[65];
    iron_mysql_scramble_native(scramble, 20, "secret", 6, out);
    hex(out, 20, text);
    TEST_ASSERT_EQUAL_STRING("b32bb3a583e1340c0a1108d58b1be49781ad8c2f", text);
    iron_mysql_scramble_sha256(scramble, 20, "secret", 6, out);
    hex(out, 32, text);
    TEST_ASSERT_EQUAL_STRING("746ebe205d56a0707acb3e796e834e0dd7b1d61743b26bd5202c7a623230c7c9", text);
}

static void test_escape_both_modes(void) {
    char out[64];
    const char in[] = { 'a', '\'', 'b', '"', '\\', '\0', '\n', '\r', '\032', 'z' };
    size_t n = iron_mysql_escape_bytes(out, in, sizeof(in), false);
    TEST_ASSERT_EQUAL_size_t(17, n);
    TEST_ASSERT_EQUAL_MEMORY("a\\'b\\\"\\\\\\0\\n\\r\\Zz", out, n);
    n = iron_mysql_escape_bytes(out, "it's \\", 6, true);
    TEST_ASSERT_EQUAL_STRING("it''s \\", out);
    Iron_String ident = S("a`b");
    Iron_String q = Iron_mysqlnative_quote_identifier(ident);
    iron_string_release(&ident);
    TEST_ASSERT_TRUE(str_eq(q, "`a``b`"));
    iron_string_release(&q);
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

static void error_release(Iron_MysqlError *e) {
    iron_string_release(&e->sqlstate);
    iron_string_release(&e->message);
}

static Iron_MysqlConfigResult parse(const char *url) {
    Iron_String u = S(url);
    Iron_MysqlConfigResult r = Iron_mysqlnative_parse_url(u);
    iron_string_release(&u);
    return r;
}

static void test_parse_url(void) {
    Iron_MysqlConfigResult r = parse(
        ("mysql://app%40x:p%3Ass%20w@db.example.com:3307/shop%2Dv2?tls=verify_identity&ca=/etc/ca.pem"
          "&connect-timeout=2500&timeout=900&multi_statements=true&collation=utf8mb4_bin"
          "&allow_public_key_retrieval=1&max_allowed_packet=1048576&server_public_key=/k.pem"));
    TEST_ASSERT_EQUAL_INT64(0, r.v1.code);
    TEST_ASSERT_TRUE(str_eq(r.v0.user, "app@x"));
    TEST_ASSERT_TRUE(str_eq(r.v0.password, "p:ss w"));
    TEST_ASSERT_TRUE(str_eq(r.v0.host, "db.example.com"));
    TEST_ASSERT_EQUAL_INT64(3307, r.v0.port);
    TEST_ASSERT_TRUE(str_eq(r.v0.database, "shop-v2"));
    TEST_ASSERT_EQUAL_INT(IRON_MYSQL_TLS_VERIFY_IDENTITY, r.v0.tls);
    TEST_ASSERT_TRUE(str_eq(r.v0.ca_file, "/etc/ca.pem"));
    TEST_ASSERT_EQUAL_INT64(2500, r.v0.connect_timeout);
    TEST_ASSERT_EQUAL_INT64(900, r.v0.timeout);
    TEST_ASSERT_TRUE(r.v0.multi_statements);
    TEST_ASSERT_TRUE(r.v0.allow_public_key_retrieval);
    TEST_ASSERT_TRUE(str_eq(r.v0.collation, "utf8mb4_bin"));
    TEST_ASSERT_EQUAL_INT64(1048576, r.v0.max_allowed_packet);
    TEST_ASSERT_TRUE(str_eq(r.v0.server_public_key, "/k.pem"));
    config_release(&r.v0);
    error_release(&r.v1);

    r = parse("mariadb://[::1]/x");
    TEST_ASSERT_EQUAL_INT64(0, r.v1.code);
    TEST_ASSERT_TRUE(str_eq(r.v0.host, "::1"));
    TEST_ASSERT_EQUAL_INT64(3306, r.v0.port);
    TEST_ASSERT_EQUAL_INT(IRON_MYSQL_TLS_PREFERRED, r.v0.tls);
    TEST_ASSERT_EQUAL_INT64(10000, r.v0.connect_timeout);
    config_release(&r.v0);
    error_release(&r.v1);

    const char *bad[] = {
        "postgres://u@h/d", "mysql://u@h:0/d", "mysql://u@h:99999/d", "mysql://u@h/d?bogus=1",
        "mysql://u@h/d?tls=sometimes", "mysql://u@[::1/d", "mysql://u%zz@h/d", "mysql://u@h/a/b",
        "mysql://u@h/d?timeout=-1", "mysql://u@h:12x/d",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        r = parse(bad[i]);
        if (r.v1.code != IRON_ERR_MYSQL_BAD_URL) {
            char msg[128];
            snprintf(msg, sizeof(msg), "accepted bad URL %s", bad[i]);
            TEST_FAIL_MESSAGE(msg);
        }
        config_release(&r.v0);
        error_release(&r.v1);
    }
}

static void test_format_values(void) {
    Iron_MysqlDateTime d = { 2024, 2, 9, 7, 5, 3, 0 };
    Iron_String s = Iron_mysqlnative_format_datetime(d);
    TEST_ASSERT_TRUE(str_eq(s, "2024-02-09 07:05:03"));
    iron_string_release(&s);
    d.microsecond = 120;
    s = Iron_mysqlnative_format_datetime(d);
    TEST_ASSERT_TRUE(str_eq(s, "2024-02-09 07:05:03.000120"));
    iron_string_release(&s);
    s = Iron_mysqlnative_format_date(d);
    TEST_ASSERT_TRUE(str_eq(s, "2024-02-09"));
    iron_string_release(&s);
    Iron_MysqlTime t = { true, 838, 59, 58, 0 };
    s = Iron_mysqlnative_format_time(t);
    TEST_ASSERT_TRUE(str_eq(s, "-838:59:58"));
    iron_string_release(&s);
}

static void test_zero_handles_are_closed_objects(void) {
    Iron_String one = S("SELECT 1");
    Iron_MysqlError e = Iron_mysqlnative_run(0, one);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_CLOSED, e.code);
    error_release(&e);
    int64_t r = Iron_mysqlnative_query(0, one);
    iron_string_release(&one);
    e = Iron_mysqlnative_res_error(r);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_CLOSED, e.code);
    error_release(&e);
    TEST_ASSERT_EQUAL_INT64(0, Iron_mysqlnative_res_info(r, 0));
    Iron_MysqlIntResult v = Iron_mysqlnative_res_int(r, 0, 0);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_RANGE, v.v1.code);
    error_release(&v.v1);
    Iron_mysqlnative_res_free(r);
    Iron_mysqlnative_conn_free(0);
    Iron_mysqlnative_stmt_free(0);
    Iron_mysqlnative_pool_free(0);
    Iron_mysqlnative_res_free(0);
}

/* ── Fake server ────────────────────────────────────────────────────────── */

enum {
    SC_NATIVE_QUERY, SC_BAD_SEQUENCE, SC_TRUNCATED_COLUMN, SC_HANDSHAKE_ERR,
    SC_CLOSE_MID_RESULT, SC_SWITCH_UNKNOWN, SC_SWITCH_TO_NATIVE, SC_CACHING_FAST,
    SC_CACHING_FULL_NO_TLS, SC_LARGE_ROW, SC_WRONG_PASSWORD, SC_GARBAGE_HANDSHAKE,
    SC_STMT_BINARY
};

typedef struct Fake {
    Iron_TcpListener listener;
    int scenario;
    int ok;                      /* the server saw what it expected */
    char note[128];
} Fake;

static int srv_send(Iron_TcpSocket s, uint8_t seq, const uint8_t *p, size_t n) {
    uint8_t h[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), seq };
    Iron_Result_Int_Error w = Iron_net_tcp_send_bytes(s, h, 4, 5000);
    if (w.v1.code || w.v0 != 4) return 0;
    size_t off = 0;
    while (off < n) {
        w = Iron_net_tcp_send_bytes(s, p + off, (int64_t)(n - off), 5000);
        if (w.v1.code || w.v0 <= 0) return 0;
        off += (size_t)w.v0;
    }
    return 1;
}

static int srv_recv_n(Iron_TcpSocket s, uint8_t *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        Iron_Result_Int_Error r = Iron_net_tcp_recv_bytes(s, p + off, (int64_t)(n - off), 5000);
        if (r.v1.code || r.v0 <= 0) return 0;
        off += (size_t)r.v0;
    }
    return 1;
}

/* Read one packet into buf (capacity cap); returns the length or -1. */
static long srv_recv(Iron_TcpSocket s, uint8_t *seq, uint8_t *buf, size_t cap) {
    uint8_t h[4];
    if (!srv_recv_n(s, h, 4)) return -1;
    size_t n = (size_t)h[0] | ((size_t)h[1] << 8) | ((size_t)h[2] << 16);
    *seq = h[3];
    if (n > cap) return -1;
    if (!srv_recv_n(s, buf, n)) return -1;
    return (long)n;
}

static size_t put_lenstr(uint8_t *p, const char *s) {
    size_t n = strlen(s);
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, n);
    return n + 1;
}

static size_t handshake(uint8_t *p, const char *plugin) {
    size_t i = 0;
    p[i++] = 10;
    memcpy(p + i, "8.4.0-fake", 11); i += 11;
    p[i++] = 42; p[i++] = 0; p[i++] = 0; p[i++] = 0;
    for (int k = 1; k <= 8; k++) p[i++] = (uint8_t)k;
    p[i++] = 0;
    uint32_t caps = 0x00000001 | 0x00000004 | 0x00000008 | 0x00000200 | 0x00002000 |
                    0x00008000 | 0x00010000 | 0x00020000 | 0x00040000 | 0x00080000 |
                    0x00200000 | 0x01000000;
    p[i++] = (uint8_t)caps; p[i++] = (uint8_t)(caps >> 8);
    p[i++] = 255;
    p[i++] = 2; p[i++] = 0;
    p[i++] = (uint8_t)(caps >> 16); p[i++] = (uint8_t)(caps >> 24);
    p[i++] = 21;
    memset(p + i, 0, 10); i += 10;
    for (int k = 9; k <= 20; k++) p[i++] = (uint8_t)k;
    p[i++] = 0;
    size_t pl = strlen(plugin) + 1;
    memcpy(p + i, plugin, pl); i += pl;
    return i;
}

static const uint8_t OK_PKT[] = { 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };
static const uint8_t EOF_OK[] = { 0xfe, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };

/* The auth response field of a HandshakeResponse41. */
static int auth_field(const uint8_t *p, long n, const uint8_t **auth, size_t *auth_len) {
    long i = 32;
    while (i < n && p[i]) i++;          /* user */
    i++;
    if (i >= n) return 0;
    *auth_len = p[i++];
    *auth = p + i;
    return i + (long)*auth_len <= n;
}

static int column_def(uint8_t *p, const char *name, uint8_t type) {
    size_t i = 0;
    i += put_lenstr(p + i, "def");
    i += put_lenstr(p + i, "db");
    i += put_lenstr(p + i, "t");
    i += put_lenstr(p + i, "t");
    i += put_lenstr(p + i, name);
    i += put_lenstr(p + i, name);
    p[i++] = 0x0c;
    p[i++] = 45; p[i++] = 0;
    p[i++] = 0xff; p[i++] = 0; p[i++] = 0; p[i++] = 0;
    p[i++] = type;
    p[i++] = 0; p[i++] = 0;
    p[i++] = 0;
    p[i++] = 0; p[i++] = 0;
    return (int)i;
}

static void serve_scenario(Fake *f, Iron_TcpSocket c) {
    static uint8_t buf[1 << 16];
    uint8_t seq;
    long n;
    uint8_t scramble[20];
    for (int k = 0; k < 20; k++) scramble[k] = (uint8_t)(k + 1);

    if (f->scenario == SC_HANDSHAKE_ERR) {
        const char msg[] = "\xff\x10\x04#08004Too many connections";
        srv_send(c, 0, (const uint8_t *)msg, sizeof(msg) - 1);
        f->ok = 1;
        return;
    }
    if (f->scenario == SC_GARBAGE_HANDSHAKE) {
        uint8_t junk[] = { 10, 'x', 'y' };     /* version string never ends */
        srv_send(c, 0, junk, sizeof(junk));
        f->ok = 1;
        return;
    }
    const char *plugin = (f->scenario == SC_SWITCH_TO_NATIVE || f->scenario == SC_CACHING_FAST ||
                          f->scenario == SC_CACHING_FULL_NO_TLS)
        ? "caching_sha2_password" : "mysql_native_password";
    size_t hn = handshake(buf, plugin);
    if (!srv_send(c, 0, buf, hn)) return;
    n = srv_recv(c, &seq, buf, sizeof(buf));
    if (n < 32 || seq != 1) { snprintf(f->note, sizeof(f->note), "bad handshake response"); return; }
    if (memcmp(buf + 32, "tester", 7) != 0) { snprintf(f->note, sizeof(f->note), "user"); return; }
    const uint8_t *auth;
    size_t alen;
    if (!auth_field(buf, n, &auth, &alen)) { snprintf(f->note, sizeof(f->note), "auth field"); return; }
    uint8_t want[32];
    uint8_t next = 2;

    switch (f->scenario) {
        case SC_SWITCH_UNKNOWN: {
            const char sw[] = "\xfe" "auth_gssapi_client\0" "abc";
            srv_send(c, 2, (const uint8_t *)sw, sizeof(sw) - 1);
            f->ok = 1;
            return;
        }
        case SC_SWITCH_TO_NATIVE: {
            iron_mysql_scramble_sha256(scramble, 20, "secret", 6, want);
            if (alen != 32 || memcmp(auth, want, 32) != 0) { snprintf(f->note, sizeof(f->note), "sha2 response"); return; }
            uint8_t sw[64];
            size_t i = 0;
            sw[i++] = 0xfe;
            memcpy(sw + i, "mysql_native_password", 22); i += 22;
            uint8_t fresh[20];
            for (int k = 0; k < 20; k++) fresh[k] = (uint8_t)(100 + k);
            memcpy(sw + i, fresh, 20); i += 20;
            sw[i++] = 0;
            srv_send(c, 2, sw, i);
            n = srv_recv(c, &seq, buf, sizeof(buf));
            iron_mysql_scramble_native(fresh, 20, "secret", 6, want);
            if (n != 20 || seq != 3 || memcmp(buf, want, 20) != 0) { snprintf(f->note, sizeof(f->note), "native after switch"); return; }
            next = 4;
            break;
        }
        case SC_CACHING_FAST: {
            iron_mysql_scramble_sha256(scramble, 20, "secret", 6, want);
            if (alen != 32 || memcmp(auth, want, 32) != 0) { snprintf(f->note, sizeof(f->note), "sha2 response"); return; }
            const uint8_t fast[] = { 0x01, 0x03 };
            srv_send(c, 2, fast, 2);
            next = 3;
            break;
        }
        case SC_CACHING_FULL_NO_TLS: {
            const uint8_t full[] = { 0x01, 0x04 };
            srv_send(c, 2, full, 2);
            f->ok = 1;               /* the client must give up here */
            return;
        }
        case SC_WRONG_PASSWORD: {
            const char msg[] = "\xff\x15\x04#28000Access denied for user 'tester'";
            srv_send(c, 2, (const uint8_t *)msg, sizeof(msg) - 1);
            f->ok = 1;
            return;
        }
        default:
            iron_mysql_scramble_native(scramble, 20, "secret", 6, want);
            if (alen != 20 || memcmp(auth, want, 20) != 0) { snprintf(f->note, sizeof(f->note), "native response"); return; }
    }
    srv_send(c, next, OK_PKT, sizeof(OK_PKT));

    /* The command. */
    n = srv_recv(c, &seq, buf, sizeof(buf));
    if (n < 1 || seq != 0) { snprintf(f->note, sizeof(f->note), "no command"); return; }
    if (f->scenario == SC_STMT_BINARY) {
        if (buf[0] != 0x16) { snprintf(f->note, sizeof(f->note), "expected prepare"); return; }
        /* stmt 7, 1 column, 1 parameter */
        uint8_t okp[12] = { 0x00, 7, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0 };
        srv_send(c, 1, okp, 12);
        uint8_t col[64];
        int cl = column_def(col, "?", 8);
        srv_send(c, 2, col, (size_t)cl);
        cl = column_def(col, "v", 8);
        srv_send(c, 3, col, (size_t)cl);
        n = srv_recv(c, &seq, buf, sizeof(buf));
        /* COM_STMT_EXECUTE id=7 flags=0 iter=1 bitmap=0 bound=1 type=LONGLONG(8),0 value=41 */
        const uint8_t want_exec[] = { 0x17, 7, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 8, 0,
                                      41, 0, 0, 0, 0, 0, 0, 0 };
        if (n != (long)sizeof(want_exec) || memcmp(buf, want_exec, sizeof(want_exec)) != 0) {
            snprintf(f->note, sizeof(f->note), "execute packet (%ld bytes)", n);
            return;
        }
        uint8_t one = 1;
        srv_send(c, 1, &one, 1);
        cl = column_def(col, "v", 8);
        srv_send(c, 2, col, (size_t)cl);
        uint8_t row[11] = { 0x00, 0x00, 42, 0, 0, 0, 0, 0, 0, 0 };
        srv_send(c, 3, row, 10);
        srv_send(c, 4, EOF_OK, sizeof(EOF_OK));
        f->ok = 1;
        n = srv_recv(c, &seq, buf, sizeof(buf));   /* COM_QUIT */
        return;
    }
    if (buf[0] != 0x03) { snprintf(f->note, sizeof(f->note), "expected COM_QUERY"); return; }
    uint8_t s = 1;
    if (f->scenario == SC_BAD_SEQUENCE) s = 5;
    uint8_t two = 2;
    srv_send(c, s++, &two, 1);
    if (f->scenario == SC_CLOSE_MID_RESULT) { f->ok = 1; return; }
    uint8_t col[64];
    int cl = column_def(col, "id", 8);
    if (f->scenario == SC_TRUNCATED_COLUMN) cl = 9;
    srv_send(c, s++, col, (size_t)cl);
    if (f->scenario == SC_TRUNCATED_COLUMN || f->scenario == SC_BAD_SEQUENCE) { f->ok = 1; return; }
    cl = column_def(col, "name", 253);
    srv_send(c, s++, col, (size_t)cl);
    if (f->scenario == SC_LARGE_ROW) {
        /* One row: "1" and a 20 MB value, split into 0xFFFFFF chunks. */
        size_t big = 20u * 1024 * 1024;
        size_t total = 2 + 9 + big;
        uint8_t *row = (uint8_t *)malloc(total);
        if (!row) return;
        row[0] = 1; row[1] = '1';
        row[2] = 0xfe;                        /* 8-byte length: over 16 MB */
        for (int k = 0; k < 8; k++) row[3 + k] = (uint8_t)((uint64_t)big >> (8 * k));
        for (size_t k = 0; k < big; k++) row[11 + k] = (uint8_t)('a' + k % 26);
        size_t off = 0;
        for (;;) {
            size_t chunk = total - off < 0xFFFFFF ? total - off : 0xFFFFFF;
            srv_send(c, s++, row + off, chunk);
            off += chunk;
            if (chunk < 0xFFFFFF) break;
        }
        free(row);
    } else {
        uint8_t row[32];
        size_t i = 0;
        i += put_lenstr(row + i, "1");
        i += put_lenstr(row + i, "alice");
        srv_send(c, s++, row, i);
        i = 0;
        i += put_lenstr(row + i, "2");
        row[i++] = 0xfb;                      /* NULL */
        srv_send(c, s++, row, i);
    }
    srv_send(c, s++, EOF_OK, sizeof(EOF_OK));
    f->ok = 1;
    n = srv_recv(c, &seq, buf, sizeof(buf));  /* COM_QUIT */
}

static void *fake_main(void *arg) {
    Fake *f = (Fake *)arg;
    Iron_Result_TcpSocket_Error a = Iron_tcplistener_accept(f->listener, 10000);
    if (a.v1.code == 0) {
        serve_scenario(f, a.v0);
        Iron_tcpsocket_close(a.v0);
    }
    return NULL;
}

#ifdef _WIN32
static DWORD WINAPI fake_trampoline(LPVOID p) { fake_main(p); return 0; }
#endif

static int start_fake(Fake *f, int scenario, SRV_THREAD *t, int64_t *port) {
    memset(f, 0, sizeof(*f));
    f->scenario = scenario;
    Iron_Result_TcpListener_Error l = Iron_net_tcp_listen(S("127.0.0.1"), 0);
    if (l.v1.code) return 0;
    f->listener = l.v0;
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
#ifdef _WIN32
    getsockname((SOCKET)l.v0.fd, (struct sockaddr *)&ss, &len);
#else
    getsockname((int)l.v0.fd, (struct sockaddr *)&ss, &len);
#endif
    *port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
#ifdef _WIN32
    *t = CreateThread(NULL, 0, fake_trampoline, f, 0, NULL);
    return *t != NULL;
#else
    return pthread_create(t, NULL, fake_main, f) == 0;
#endif
}

static void join_fake(Fake *f, SRV_THREAD t) {
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
    Iron_tcplistener_close(f->listener);
}

static Iron_MysqlConfig fake_config(int64_t port) {
    Iron_MysqlConfig c;
    memset(&c, 0, sizeof(c));
    c.host = S("127.0.0.1");
    c.port = port;
    c.user = S("tester");
    c.password = S("secret");
    c.database = S("db");
    c.tls = IRON_MYSQL_TLS_DISABLED;
    c.connect_timeout = 5000;
    c.timeout = 5000;
    c.max_allowed_packet = 64 * 1024 * 1024;
    c.ca_file = S("");
    c.server_public_key = S("");
    c.collation = S("");
    return c;
}

/* Connect to a fake server running `scenario`; run `sql` when connected.
 * Returns the connect error code (0 on success) and the query's result. */
static int64_t run_fake(int scenario, const char *sql, int64_t *res_out, int *server_ok,
                        Iron_MysqlError *conn_err_out) {
    Fake f;
    SRV_THREAD t;
    int64_t port;
    TEST_ASSERT_TRUE(start_fake(&f, scenario, &t, &port));
    Iron_MysqlConfig cfg = fake_config(port);
    int64_t conn = Iron_mysqlnative_connect(cfg);
    config_release(&cfg);
    Iron_MysqlError e = Iron_mysqlnative_conn_error(conn);
    int64_t code = e.code;
    if (conn_err_out) *conn_err_out = e; else error_release(&e);
    *res_out = 0;
    if (code == 0 && sql) {
        Iron_String q = S(sql);
        *res_out = Iron_mysqlnative_query(conn, q);
        iron_string_release(&q);
    }
    Iron_mysqlnative_conn_free(conn);
    join_fake(&f, t);
    if (!f.ok && f.note[0]) TEST_FAIL_MESSAGE(f.note);
    *server_ok = f.ok;
    return code;
}

static void test_fake_native_auth_and_text_result(void) {
    int64_t res;
    int ok;
    TEST_ASSERT_EQUAL_INT64(0, run_fake(SC_NATIVE_QUERY, "SELECT id, name FROM t", &res, &ok, NULL));
    TEST_ASSERT_TRUE(ok);
    Iron_MysqlError e = Iron_mysqlnative_res_error(res);
    TEST_ASSERT_EQUAL_INT64(0, e.code);
    error_release(&e);
    TEST_ASSERT_EQUAL_INT64(2, Iron_mysqlnative_res_info(res, 0));
    TEST_ASSERT_EQUAL_INT64(2, Iron_mysqlnative_res_info(res, 1));
    Iron_MysqlIntResult id = Iron_mysqlnative_res_int(res, 1, 0);
    TEST_ASSERT_EQUAL_INT64(2, id.v0);
    TEST_ASSERT_EQUAL_INT64(0, id.v1.code);
    Iron_MysqlStringResult name = Iron_mysqlnative_res_text(res, 0, 1);
    TEST_ASSERT_TRUE(str_eq(name.v0, "alice"));
    iron_string_release(&name.v0);
    TEST_ASSERT_TRUE(Iron_mysqlnative_res_is_null(res, 1, 1));
    Iron_MysqlStringResult null_name = Iron_mysqlnative_res_text(res, 1, 1);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_NULL, null_name.v1.code);
    error_release(&null_name.v1);
    Iron_String nm = S("NAME");
    TEST_ASSERT_EQUAL_INT64(1, Iron_mysqlnative_res_col(res, nm));
    iron_string_release(&nm);
    Iron_MysqlColumn col = Iron_mysqlnative_res_column(res, 0);
    TEST_ASSERT_TRUE(str_eq(col.name, "id"));
    TEST_ASSERT_TRUE(str_eq(col.type_name, "BIGINT"));
    iron_string_release(&col.name); iron_string_release(&col.org_name);
    iron_string_release(&col.table); iron_string_release(&col.org_table);
    iron_string_release(&col.schema); iron_string_release(&col.type_name);
    Iron_mysqlnative_res_free(res);
}

static void expect_query_error(int scenario, int64_t want_code) {
    int64_t res;
    int ok;
    TEST_ASSERT_EQUAL_INT64(0, run_fake(scenario, "SELECT 1", &res, &ok, NULL));
    TEST_ASSERT_TRUE(ok);
    Iron_MysqlError e = Iron_mysqlnative_res_error(res);
    TEST_ASSERT_EQUAL_INT64(want_code, e.code);
    TEST_ASSERT_FALSE(e.server);
    error_release(&e);
    Iron_mysqlnative_res_free(res);
}

static void test_fake_bad_sequence_is_protocol_error(void) {
    expect_query_error(SC_BAD_SEQUENCE, IRON_ERR_MYSQL_PROTOCOL);
}

static void test_fake_truncated_column_is_protocol_error(void) {
    expect_query_error(SC_TRUNCATED_COLUMN, IRON_ERR_MYSQL_PROTOCOL);
}

static void test_fake_close_mid_result_is_network_error(void) {
    expect_query_error(SC_CLOSE_MID_RESULT, IRON_ERR_NET_CLOSED);
}

static void expect_connect_error(int scenario, int64_t want_code, bool server) {
    int64_t res;
    int ok;
    Iron_MysqlError e;
    TEST_ASSERT_EQUAL_INT64(want_code, run_fake(scenario, NULL, &res, &ok, &e));
    TEST_ASSERT_EQUAL(server, e.server);
    error_release(&e);
}

static void test_fake_handshake_error_packet(void) {
    expect_connect_error(SC_HANDSHAKE_ERR, 1040, true);
}

static void test_fake_garbage_handshake(void) {
    expect_connect_error(SC_GARBAGE_HANDSHAKE, IRON_ERR_MYSQL_PROTOCOL, false);
}

static void test_fake_switch_to_unknown_plugin(void) {
    expect_connect_error(SC_SWITCH_UNKNOWN, IRON_ERR_MYSQL_AUTH, false);
}

static void test_fake_wrong_password(void) {
    expect_connect_error(SC_WRONG_PASSWORD, 1045, true);
}

static void test_fake_caching_full_auth_needs_tls_or_key(void) {
    expect_connect_error(SC_CACHING_FULL_NO_TLS, IRON_ERR_MYSQL_INSECURE_AUTH, false);
}

static void test_fake_auth_switch_to_native(void) {
    int64_t res;
    int ok;
    TEST_ASSERT_EQUAL_INT64(0, run_fake(SC_SWITCH_TO_NATIVE, "SELECT 1", &res, &ok, NULL));
    TEST_ASSERT_TRUE(ok);
    Iron_mysqlnative_res_free(res);
}

static void test_fake_caching_sha2_fast_path(void) {
    int64_t res;
    int ok;
    TEST_ASSERT_EQUAL_INT64(0, run_fake(SC_CACHING_FAST, "SELECT 1", &res, &ok, NULL));
    TEST_ASSERT_TRUE(ok);
    Iron_mysqlnative_res_free(res);
}

static void test_fake_row_split_over_16mb(void) {
    int64_t res;
    int ok;
    TEST_ASSERT_EQUAL_INT64(0, run_fake(SC_LARGE_ROW, "SELECT big", &res, &ok, NULL));
    TEST_ASSERT_TRUE(ok);
    Iron_MysqlError e = Iron_mysqlnative_res_error(res);
    if (e.code) TEST_FAIL_MESSAGE(iron_string_cstr(&e.message));
    error_release(&e);
    Iron_MysqlStringResult v = Iron_mysqlnative_res_text(res, 0, 1);
    size_t n = iron_string_byte_len(&v.v0);
    TEST_ASSERT_EQUAL_size_t(20u * 1024 * 1024, n);
    const char *p = iron_string_cstr(&v.v0);
    TEST_ASSERT_EQUAL_CHAR('a', p[0]);
    TEST_ASSERT_EQUAL_CHAR((char)('a' + (n - 1) % 26), p[n - 1]);
    iron_string_release(&v.v0);
    Iron_mysqlnative_res_free(res);
}

static void test_fake_prepared_statement_binary_protocol(void) {
    Fake f;
    SRV_THREAD t;
    int64_t port;
    TEST_ASSERT_TRUE(start_fake(&f, SC_STMT_BINARY, &t, &port));
    Iron_MysqlConfig cfg = fake_config(port);
    int64_t conn = Iron_mysqlnative_connect(cfg);
    config_release(&cfg);
    Iron_MysqlError e = Iron_mysqlnative_conn_error(conn);
    TEST_ASSERT_EQUAL_INT64(0, e.code);
    error_release(&e);
    Iron_MysqlValue param;
    memset(&param, 0, sizeof(param));
    param.tag = Iron_MysqlValue_TAG_Int;
    param.data.Int._0 = 41;
    Iron_MysqlValueList params = { &param, 1, 1 };
    Iron_String sql = S("SELECT ? + 1");
    int64_t res = Iron_mysqlnative_exec(conn, sql, params);
    iron_string_release(&sql);
    e = Iron_mysqlnative_res_error(res);
    TEST_ASSERT_EQUAL_INT64(0, e.code);
    error_release(&e);
    Iron_MysqlIntResult v = Iron_mysqlnative_res_int(res, 0, 0);
    TEST_ASSERT_EQUAL_INT64(42, v.v0);
    Iron_MysqlStringResult txt = Iron_mysqlnative_res_text(res, 0, 0);
    TEST_ASSERT_TRUE(str_eq(txt.v0, "42"));
    iron_string_release(&txt.v0);
    Iron_mysqlnative_res_free(res);
    Iron_mysqlnative_conn_free(conn);
    join_fake(&f, t);
    if (!f.ok) TEST_FAIL_MESSAGE(f.note[0] ? f.note : "server did not finish");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_scrambles_match_reference_vectors);
    RUN_TEST(test_escape_both_modes);
    RUN_TEST(test_parse_url);
    RUN_TEST(test_format_values);
    RUN_TEST(test_zero_handles_are_closed_objects);
    RUN_TEST(test_fake_native_auth_and_text_result);
    RUN_TEST(test_fake_bad_sequence_is_protocol_error);
    RUN_TEST(test_fake_truncated_column_is_protocol_error);
    RUN_TEST(test_fake_close_mid_result_is_network_error);
    RUN_TEST(test_fake_handshake_error_packet);
    RUN_TEST(test_fake_garbage_handshake);
    RUN_TEST(test_fake_switch_to_unknown_plugin);
    RUN_TEST(test_fake_wrong_password);
    RUN_TEST(test_fake_caching_full_auth_needs_tls_or_key);
    RUN_TEST(test_fake_auth_switch_to_native);
    RUN_TEST(test_fake_caching_sha2_fast_path);
    RUN_TEST(test_fake_row_split_over_16mb);
    RUN_TEST(test_fake_prepared_statement_binary_protocol);
    return UNITY_END();
}
