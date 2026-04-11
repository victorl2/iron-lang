/* test_stdlib_net_cross_platform.c — Phase 59 P06 cross-platform Unity battery.
 *
 * Exercises the full C-level networking surface (TCP loopback + UDP loopback +
 * IPv4/IPv6 parse/format + DNS localhost) in a single binary so ubuntu-latest,
 * macos-latest, AND windows-latest all run the same assertions.
 *
 * The more granular TCP/UDP/IP/DNS suites (test_stdlib_net_tcp / _udp / _ip /
 * _dns) cover edge cases and failure modes — this suite is the
 * "does-everything-work-together-on-Windows" smoke.
 *
 *   1. test_tcp_localhost_exchange     listen + spawn thread + dial + read 5B
 *   2. test_udp_localhost_exchange     bind srv/cli + sendto_v4 + recvfrom
 *   3. test_ipv4_ipv6_parse_format     parse valid v4/v6, reject garbage
 *   4. test_dns_localhost              lookup_host("localhost", 3000) >= 1 addr
 *
 * Notes:
 *   - iron_runtime_init/shutdown per test (idempotent since 59-01c).
 *   - Windows uses CreateThread; POSIX uses pthread_create. Same signature
 *     is hidden behind IRON_XTHREAD / xthread_create / xthread_join.
 *   - The TCP test is intentionally IPv4-only on 127.0.0.1; the authoritative
 *     v4-to-v6 dual-stack coverage lives in test_stdlib_net_tcp.c ::
 *     test_tcp_dualstack_v4_to_v6.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_errors.h"
#include "stdlib/iron_net.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef HANDLE IRON_XTHREAD;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <pthread.h>
  typedef pthread_t IRON_XTHREAD;
#endif

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

static Iron_String make_iron_string(const char *s) {
    return iron_string_from_cstr(s, strlen(s));
}

/* ── Tiny cross-platform thread wrapper ───────────────────────────────── */
typedef void *(*xthread_fn)(void *);

#ifdef _WIN32
typedef struct {
    xthread_fn fn;
    void      *arg;
} xthread_ctx_t;

static DWORD WINAPI xthread_trampoline(LPVOID p) {
    xthread_ctx_t *ctx = (xthread_ctx_t *)p;
    xthread_fn fn = ctx->fn;
    void      *arg = ctx->arg;
    free(ctx);
    (void)fn(arg);
    return 0;
}

static int xthread_create(IRON_XTHREAD *th, xthread_fn fn, void *arg) {
    xthread_ctx_t *ctx = (xthread_ctx_t *)malloc(sizeof(*ctx));
    if (!ctx) return -1;
    ctx->fn  = fn;
    ctx->arg = arg;
    *th = CreateThread(NULL, 0, xthread_trampoline, ctx, 0, NULL);
    if (*th == NULL) { free(ctx); return -1; }
    return 0;
}

static int xthread_join(IRON_XTHREAD th) {
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    return 0;
}
#else
static int xthread_create(IRON_XTHREAD *th, xthread_fn fn, void *arg) {
    return pthread_create(th, NULL, fn, arg);
}
static int xthread_join(IRON_XTHREAD th) {
    return pthread_join(th, NULL);
}
#endif

/* ──────────────────────────────────────────────────────────────────────
 * Test 1: TCP localhost exchange
 * Spawn a worker thread that accepts once and writes "hello"; main thread
 * dials the loopback port, reads 5 bytes, asserts contents match.
 * ────────────────────────────────────────────────────────────────────── */

typedef struct {
    Iron_TcpListener listener;
    int              accept_rc;
    int              write_rc;
} tcp_server_ctx_t;

static void *tcp_server_fn(void *arg) {
    tcp_server_ctx_t *ctx = (tcp_server_ctx_t *)arg;
    Iron_Result_TcpSocket_Error ar =
        Iron_TcpListener_accept_result(ctx->listener, 5000);
    ctx->accept_rc = (int)ar.v1.code;
    if (ar.v1.code != 0) return NULL;

    const uint8_t msg[5] = {'h', 'e', 'l', 'l', 'o'};
    Iron_Result_Int_Error wr =
        Iron_TcpSocket_write_result(ar.v0, msg, 5, 2000);
    ctx->write_rc = (int)wr.v1.code;

    Iron_TcpSocket_close(ar.v0);
    return NULL;
}

void test_tcp_localhost_exchange(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_TcpListener_Error lr = Iron_Net_tcp_listen_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lr.v1.code,
        "tcp_listen on 127.0.0.1:0 should succeed");

    /* Retrieve the ephemeral port via getsockname. */
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
#ifdef _WIN32
    int slen = (int)sizeof(ss);
#else
    socklen_t slen = sizeof(ss);
#endif
    TEST_ASSERT_EQUAL_INT_MESSAGE(0,
        getsockname((int)lr.v0.fd, (struct sockaddr *)&ss, &slen),
        "getsockname on listener failed");

    int port = 0;
    if (ss.ss_family == AF_INET) {
        port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    }
    TEST_ASSERT_MESSAGE(port > 0, "ephemeral port should be > 0");

    /* Spawn the server thread. */
    tcp_server_ctx_t ctx = {0};
    ctx.listener  = lr.v0;
    ctx.accept_rc = -1;
    ctx.write_rc  = -1;

    IRON_XTHREAD srv;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, xthread_create(&srv, tcp_server_fn, &ctx),
        "xthread_create for server failed");

    /* Dial the listener. */
    Iron_Result_TcpSocket_Error dr = Iron_Net_tcp_dial_result(host, port, 5000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dr.v1.code,
        "tcp_dial to loopback should succeed");

    /* Read up to 16 bytes; expect exactly 5 ("hello"). */
    uint8_t buf[16] = {0};
    Iron_Result_Int_Error rr =
        Iron_TcpSocket_read_result(dr.v0, buf, 16, 2000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rr.v1.code, "read should succeed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, (int)rr.v0, "should read 5 bytes");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE("hello", buf, 5, "payload mismatch");

    Iron_TcpSocket_close(dr.v0);
    xthread_join(srv);
    Iron_TcpListener_close(lr.v0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ctx.accept_rc,
        "server accept should have succeeded");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ctx.write_rc,
        "server write should have succeeded");
}

/* ──────────────────────────────────────────────────────────────────────
 * Test 2: UDP localhost exchange
 * Bind two sockets on 127.0.0.1:0, send 5 bytes from cli → srv, recv on
 * srv and verify payload + sender address family.
 * ────────────────────────────────────────────────────────────────────── */
void test_udp_localhost_exchange(void) {
    Iron_String host = make_iron_string("127.0.0.1");

    Iron_Result_UdpSocket_NetError srv = Iron_Net_udp_bind_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, srv.v1.code, "server bind failed");

    Iron_Result_UdpSocket_NetError cli = Iron_Net_udp_bind_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cli.v1.code, "client bind failed");

    /* getsockname on srv for the ephemeral port. */
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
#ifdef _WIN32
    int slen = (int)sizeof(saddr);
#else
    socklen_t slen = sizeof(saddr);
#endif
    TEST_ASSERT_EQUAL_INT(0, getsockname((int)srv.v0.fd,
                                         (struct sockaddr *)&saddr, &slen));
    uint16_t srv_port = ntohs(saddr.sin_port);
    TEST_ASSERT_MESSAGE(srv_port > 0, "srv ephemeral port > 0");

    /* cli sends "hello" (5 bytes) to srv via the flat v4 helper. */
    Iron_String buf = iron_string_from_cstr("hello", 5);
    Iron_IPv4Addr loopback_v4 = { 127, 0, 0, 1 };
    Iron_Result_Int_NetError sr = Iron_Net_udp_sendto_v4_result(
        cli.v0, buf, loopback_v4, (int64_t)srv_port, 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sr.v1.code, "sendto should succeed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, (int)sr.v0, "should report 5 bytes sent");

    /* srv recvfrom. */
    uint8_t rbuf[32] = {0};
    Iron_UdpRecvResult rr =
        Iron_UdpSocket_recvfrom_result(srv.v0, rbuf, sizeof(rbuf), 2000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rr.err.code, "recvfrom should succeed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, (int)rr.nbytes, "should receive 5 bytes");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE("hello", rbuf, 5, "payload mismatch");
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, (int)rr.addr_family,
        "sender family should be IPv4");

    Iron_UdpSocket_close(cli.v0);
    Iron_UdpSocket_close(srv.v0);
}

/* ──────────────────────────────────────────────────────────────────────
 * Test 3: IPv4 and IPv6 parse/format on all platforms
 * ────────────────────────────────────────────────────────────────────── */
void test_ipv4_ipv6_parse_format(void) {
    /* IPv4 valid */
    Iron_String s1 = make_iron_string("10.0.0.1");
    Iron_Result_IPv4Addr_NetError r1 = Iron_Net_ipv4addr_parse_result(s1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r1.v1.code, "parse 10.0.0.1 should succeed");
    TEST_ASSERT_EQUAL_INT64_MESSAGE(10, r1.v0.a, "octet a");
    TEST_ASSERT_EQUAL_INT64_MESSAGE( 0, r1.v0.b, "octet b");
    TEST_ASSERT_EQUAL_INT64_MESSAGE( 0, r1.v0.c, "octet c");
    TEST_ASSERT_EQUAL_INT64_MESSAGE( 1, r1.v0.d, "octet d");

    /* IPv4 format roundtrip */
    Iron_String fmt_out = Iron_Net_ipv4addr_format(r1.v0);
    Iron_String fmt_exp = make_iron_string("10.0.0.1");
    TEST_ASSERT_TRUE_MESSAGE(iron_string_equals(&fmt_out, &fmt_exp),
        "format(10.0.0.1) should roundtrip");

    /* IPv6 valid */
    Iron_String s2 = make_iron_string("2001:db8::1");
    Iron_Result_IPv6Addr_NetError r2 = Iron_Net_ipv6addr_parse_result(s2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r2.v1.code,
        "parse 2001:db8::1 should succeed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(16, (int)iron_string_byte_len(&r2.v0.bytes),
        "IPv6 bytes should be 16 octets");
    const uint8_t *b = (const uint8_t *)iron_string_cstr(&r2.v0.bytes);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x20, b[0],  "ipv6 octet[0]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x01, b[1],  "ipv6 octet[1]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x0d, b[2],  "ipv6 octet[2]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xb8, b[3],  "ipv6 octet[3]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x01, b[15], "ipv6 octet[15]");

    /* IPv4 invalid — "bogus" should return IRON_ERR_NET_BAD_IP */
    Iron_String s3 = make_iron_string("bogus");
    Iron_Result_IPv4Addr_NetError r3 = Iron_Net_ipv4addr_parse_result(s3);
    TEST_ASSERT_EQUAL_INT_MESSAGE(IRON_ERR_NET_BAD_IP, r3.v1.code,
        "invalid IPv4 should return IRON_ERR_NET_BAD_IP");
}

/* ──────────────────────────────────────────────────────────────────────
 * Test 4: localhost DNS resolution
 * On every supported OS, "localhost" must resolve to at least one Address
 * within a 3-second budget.
 * ────────────────────────────────────────────────────────────────────── */
void test_dns_localhost(void) {
    Iron_String name = make_iron_string("localhost");
    Iron_Result_AddressList_NetError r = Iron_Net_lookup_host_result(name, 3000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.v1.code,
        "lookup_host(localhost) should succeed");
    TEST_ASSERT_MESSAGE(r.v0.count >= 1,
        "lookup_host(localhost) should return >= 1 Address");

    /* First entry should be V4 or V6. */
    int tag = (int)r.v0.items[0].tag;
    TEST_ASSERT_MESSAGE(
        tag == Iron_Address_TAG_V4 || tag == Iron_Address_TAG_V6,
        "first address should be V4 or V6");

    if (r.v0.items) free(r.v0.items);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tcp_localhost_exchange);
    RUN_TEST(test_udp_localhost_exchange);
    RUN_TEST(test_ipv4_ipv6_parse_format);
    RUN_TEST(test_dns_localhost);
    return UNITY_END();
}
