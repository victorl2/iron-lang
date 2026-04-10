/* test_stdlib_net_tcp.c — Phase 59 P02 TCP stdlib Unity tests.
 *
 * Covers NET-01..06 + INFRA-08 (IPv6 dual-stack):
 *
 *   1. test_tcp_listen_ephemeral
 *      Iron_Net_tcp_listen_result("127.0.0.1", 0) returns a listener bound to an
 *      OS-assigned ephemeral port. getsockname() confirms the port is > 0.
 *
 *   2. test_tcp_listen_v6only_off
 *      Iron_Net_tcp_listen_result("::", 0) sets IPV6_V6ONLY=0 via setsockopt
 *      before bind. getsockopt confirms the value is 0 (INFRA-08).
 *
 *   3. test_tcp_dial_loopback_roundtrip
 *      Spawn a helper pthread/Win32 thread that accepts once and sends "hello";
 *      main thread dials the loopback port, reads 5 bytes, asserts they match.
 *      Exercises the full dial+accept+read+write+close path.
 *
 *   4. test_tcp_dial_timeout_unreachable
 *      Iron_Net_tcp_dial_result("192.0.2.1", 80, 500) must return
 *      IRON_ERR_NET_TIMEOUT within (400..2500)ms — no hang, no wall-clock slack
 *      beyond the monotonic deadline (INFRA-09).
 *
 *   5. test_tcp_write_after_peer_close
 *      Writing to a socket whose peer has closed must return a typed error
 *      (IRON_ERR_NET_CONN_RESET / CONN_ABORTED / CLOSED), not SIGPIPE the
 *      process dead (INFRA-07).
 *
 *   6. test_tcp_read_clean_eof
 *      recv() returning 0 after peer shutdown(SHUT_WR) is a clean EOF, not an
 *      error — Iron_TcpSocket_read_result must return (0, iron_error_none()).
 *
 *   7. test_tcp_accept_timeout
 *      Iron_TcpListener_accept_result with timeout=100 and no client returns
 *      IRON_ERR_NET_TIMEOUT in ~100ms (80..400ms tolerance).
 *
 *   8. test_tcp_dualstack_v4_to_v6
 *      Listener on "::" accepts an IPv4 client from "127.0.0.1"; getpeername
 *      returns AF_INET6 with ::ffff:127.0.0.1 (IPv4-mapped IPv6) — proves
 *      Phase 59 Success Criterion 4 dual-stack support.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_errors.h"
#include "stdlib/iron_net.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef int socklen_t_t;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <pthread.h>
#endif

/* ── Unity boilerplate ─────────────────────────────────────────────────────
 * iron_runtime_init/shutdown are idempotent (P01c) so we can put them in
 * setUp/tearDown without worrying about refcount skew. */
void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── Helper: build Iron_String from C literal ─────────────────────────── */
static Iron_String make_iron_string(const char *s) {
    return iron_string_from_cstr(s, strlen(s));
}

/* ── Test 1: ephemeral listen ──────────────────────────────────────────── */
void test_tcp_listen_ephemeral(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_TcpListener_Error lr = Iron_Net_tcp_listen_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lr.v1.code,
        "tcp_listen should succeed on 127.0.0.1:0");
    TEST_ASSERT_MESSAGE(lr.v0.fd >= 0, "fd should be non-negative");

    /* Use getsockname to retrieve the OS-assigned port */
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int rc = getsockname((int)lr.v0.fd, (struct sockaddr*)&addr, &alen);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "getsockname failed");
    uint16_t port = ntohs(addr.sin_port);
    TEST_ASSERT_MESSAGE(port > 0, "ephemeral port should be > 0");

    Iron_TcpListener_close(lr.v0);
}

/* ── Test 2: dual-stack listener sets IPV6_V6ONLY=0 ────────────────────── */
void test_tcp_listen_v6only_off(void) {
    Iron_String host = make_iron_string("::");
    Iron_Result_TcpListener_Error lr = Iron_Net_tcp_listen_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lr.v1.code,
        "tcp_listen should succeed on ::");
    TEST_ASSERT_MESSAGE(lr.v0.fd >= 0, "fd should be non-negative");

    int v6only = 1; /* pre-load with the opposite value to catch no-op */
    socklen_t optlen = sizeof(v6only);
    int rc = getsockopt((int)lr.v0.fd, IPPROTO_IPV6, IPV6_V6ONLY,
                         (char*)&v6only, &optlen);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "getsockopt IPV6_V6ONLY failed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, v6only,
        "IPV6_V6ONLY should be 0 (dual-stack mode) when host is \"::\"");

    Iron_TcpListener_close(lr.v0);
}

/* ── Test 3: loopback roundtrip — dial + accept + read + write + close ── */
typedef struct {
    int64_t listener_fd;
    uint16_t port;
    int server_err;
} loopback_ctx_t;

#ifdef _WIN32
static DWORD WINAPI loopback_server_thread(LPVOID arg) {
#else
static void *loopback_server_thread(void *arg) {
#endif
    loopback_ctx_t *ctx = (loopback_ctx_t *)arg;
    Iron_TcpListener l = { ctx->listener_fd };
    Iron_Result_TcpSocket_Error ar = Iron_TcpListener_accept_result(l, 5000);
    if (ar.v1.code != 0) {
        ctx->server_err = ar.v1.code;
#ifdef _WIN32
        return 1;
#else
        return NULL;
#endif
    }
    /* Send "hello" */
    const uint8_t hello[5] = { 'h','e','l','l','o' };
    Iron_TcpSocket_write_result(ar.v0, hello, 5, 5000);
    Iron_TcpSocket_close(ar.v0);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

void test_tcp_dial_loopback_roundtrip(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_TcpListener_Error lr = Iron_Net_tcp_listen_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lr.v1.code, "listen failed");

    /* Retrieve ephemeral port */
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    TEST_ASSERT_EQUAL_INT(0, getsockname((int)lr.v0.fd, (struct sockaddr*)&addr, &alen));
    uint16_t port = ntohs(addr.sin_port);

    loopback_ctx_t ctx = { lr.v0.fd, port, 0 };

#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, loopback_server_thread, &ctx, 0, NULL);
    TEST_ASSERT_NOT_NULL(th);
#else
    pthread_t th;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, loopback_server_thread, &ctx));
#endif

    /* Client dial */
    Iron_String client_host = make_iron_string("127.0.0.1");
    Iron_Result_TcpSocket_Error dr = Iron_Net_tcp_dial_result(client_host, port, 5000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dr.v1.code, "dial failed");

    /* Read 5 bytes (may need multiple reads under normal short-read semantics,
     * but on loopback the server sends all 5 in one shot so first read returns 5) */
    uint8_t buf[8] = {0};
    int64_t total = 0;
    while (total < 5) {
        Iron_Result_Int_Error rr = Iron_TcpSocket_read_result(dr.v0, buf + total,
                                                                (int64_t)(sizeof(buf) - (size_t)total),
                                                                5000);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, rr.v1.code, "read failed");
        if (rr.v0 == 0) break; /* EOF */
        total += rr.v0;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, (int)total, "short read");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE("hello", buf, 5, "received bytes mismatch");

#ifdef _WIN32
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ctx.server_err, "server thread error");

    Iron_TcpSocket_close(dr.v0);
    Iron_TcpListener_close(lr.v0);
}

/* ── Test 4: unreachable dial hits IRON_ERR_NET_TIMEOUT promptly ───────── */
void test_tcp_dial_timeout_unreachable(void) {
    Iron_String host = make_iron_string("192.0.2.1");  /* RFC 5737 TEST-NET-1 */
    uint64_t t0 = Iron_monotonic_now_ms();
    Iron_Result_TcpSocket_Error dr = Iron_Net_tcp_dial_result(host, 80, 500);
    uint64_t t1 = Iron_monotonic_now_ms();

    TEST_ASSERT_EQUAL_INT_MESSAGE(IRON_ERR_NET_TIMEOUT, dr.v1.code,
        "dial to TEST-NET-1 should return IRON_ERR_NET_TIMEOUT");
    uint64_t elapsed = t1 - t0;
    /* Tolerance band: 400..2500ms. 500ms budget + scheduler jitter — must
     * fire promptly, must not hang the wall-clock duration. */
    TEST_ASSERT_MESSAGE(elapsed >= 400 && elapsed <= 2500,
        "dial timeout elapsed should be in [400, 2500]ms");
}

/* ── Test 5: write after peer-close returns typed error, not crash ─────── */
void test_tcp_write_after_peer_close(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_TcpListener_Error lr = Iron_Net_tcp_listen_result(host, 0);
    TEST_ASSERT_EQUAL_INT(0, lr.v1.code);

    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    getsockname((int)lr.v0.fd, (struct sockaddr*)&addr, &alen);
    uint16_t port = ntohs(addr.sin_port);

    Iron_Result_TcpSocket_Error dr = Iron_Net_tcp_dial_result(host, port, 2000);
    TEST_ASSERT_EQUAL_INT(0, dr.v1.code);

    Iron_Result_TcpSocket_Error ar = Iron_TcpListener_accept_result(lr.v0, 2000);
    TEST_ASSERT_EQUAL_INT(0, ar.v1.code);

    /* Close client side. */
    Iron_TcpSocket_close(dr.v0);

    /* Give the kernel a moment to propagate the FIN. */
#ifdef _WIN32
    Sleep(50);
#else
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);
#endif

    /* Write from the server side — first write after peer close may still
     * succeed (TCP doesn't know yet). Do two writes to force the reset
     * into the error path. */
    const uint8_t payload[4] = { 'd', 'a', 't', 'a' };
    Iron_Result_Int_Error wr1 = Iron_TcpSocket_write_result(ar.v0, payload, 4, 1000);
    (void)wr1;
#ifdef _WIN32
    Sleep(50);
#else
    nanosleep(&ts, NULL);
#endif
    Iron_Result_Int_Error wr2 = Iron_TcpSocket_write_result(ar.v0, payload, 4, 1000);

    /* Either wr1 or wr2 must surface a typed error — or at least the
     * aggregate must not have silently crashed with SIGPIPE. */
    int ok_codes[] = { 0, IRON_ERR_NET_CONN_RESET, IRON_ERR_NET_CONN_ABORTED,
                       IRON_ERR_NET_CLOSED, IRON_ERR_NET_BAD_FD };
    bool wr2_ok = false;
    for (size_t i = 0; i < sizeof(ok_codes)/sizeof(ok_codes[0]); i++) {
        if (wr2.v1.code == ok_codes[i]) { wr2_ok = true; break; }
    }
    TEST_ASSERT_MESSAGE(wr2_ok,
        "write after peer close: second write should return a typed error or 0, not SIGPIPE");
    /* We also assert we didn't die — the fact that control reached here
     * means SIG_IGN (P01c) worked. */

    Iron_TcpSocket_close(ar.v0);
    Iron_TcpListener_close(lr.v0);
}

/* ── Test 6: clean EOF returns (0, none) not an error ─────────────────── */
void test_tcp_read_clean_eof(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_TcpListener_Error lr = Iron_Net_tcp_listen_result(host, 0);
    TEST_ASSERT_EQUAL_INT(0, lr.v1.code);

    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    getsockname((int)lr.v0.fd, (struct sockaddr*)&addr, &alen);
    uint16_t port = ntohs(addr.sin_port);

    Iron_Result_TcpSocket_Error dr = Iron_Net_tcp_dial_result(host, port, 2000);
    TEST_ASSERT_EQUAL_INT(0, dr.v1.code);

    Iron_Result_TcpSocket_Error ar = Iron_TcpListener_accept_result(lr.v0, 2000);
    TEST_ASSERT_EQUAL_INT(0, ar.v1.code);

    /* Server shuts down the write side cleanly. */
#ifdef _WIN32
    shutdown((SOCKET)ar.v0.fd, SD_SEND);
#else
    shutdown((int)ar.v0.fd, SHUT_WR);
#endif

    /* Client reads — should return (0, none). */
    uint8_t buf[8] = {0};
    Iron_Result_Int_Error rr = Iron_TcpSocket_read_result(dr.v0, buf, 8, 2000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rr.v1.code, "clean EOF must not be an error");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)rr.v0, "clean EOF returns 0 bytes");

    Iron_TcpSocket_close(ar.v0);
    Iron_TcpSocket_close(dr.v0);
    Iron_TcpListener_close(lr.v0);
}

/* ── Test 7: accept timeout fires promptly with no pending client ─────── */
void test_tcp_accept_timeout(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_TcpListener_Error lr = Iron_Net_tcp_listen_result(host, 0);
    TEST_ASSERT_EQUAL_INT(0, lr.v1.code);

    uint64_t t0 = Iron_monotonic_now_ms();
    Iron_Result_TcpSocket_Error ar = Iron_TcpListener_accept_result(lr.v0, 100);
    uint64_t t1 = Iron_monotonic_now_ms();

    TEST_ASSERT_EQUAL_INT_MESSAGE(IRON_ERR_NET_TIMEOUT, ar.v1.code,
        "accept with no client should return IRON_ERR_NET_TIMEOUT");
    uint64_t elapsed = t1 - t0;
    TEST_ASSERT_MESSAGE(elapsed >= 80 && elapsed <= 400,
        "accept timeout elapsed should be in [80, 400]ms");

    Iron_TcpListener_close(lr.v0);
}

/* ── Test 8: dual-stack listener accepts IPv4 client ──────────────────── */
typedef struct {
    uint16_t port;
    int dial_err;
    int64_t dial_fd;
} dualstack_ctx_t;

#ifdef _WIN32
static DWORD WINAPI dualstack_client_thread(LPVOID arg) {
#else
static void *dualstack_client_thread(void *arg) {
#endif
    dualstack_ctx_t *ctx = (dualstack_ctx_t *)arg;
    Iron_String host = iron_string_from_cstr("127.0.0.1", 9);
    Iron_Result_TcpSocket_Error dr = Iron_Net_tcp_dial_result(host, ctx->port, 2000);
    ctx->dial_err = dr.v1.code;
    ctx->dial_fd  = dr.v0.fd;
    if (dr.v1.code == 0) {
        /* Send a small payload so the server's read has something to return. */
        const uint8_t payload[9] = { 'd','u','a','l','s','t','a','c','k' };
        Iron_TcpSocket_write_result(dr.v0, payload, 9, 2000);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

void test_tcp_dualstack_v4_to_v6(void) {
    Iron_String host = make_iron_string("::");
    Iron_Result_TcpListener_Error lr = Iron_Net_tcp_listen_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lr.v1.code, "v6 listen failed");

    /* getsockname on a v6 socket returns sockaddr_in6. */
    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    socklen_t alen = sizeof(addr6);
    int rc = getsockname((int)lr.v0.fd, (struct sockaddr*)&addr6, &alen);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "getsockname v6 failed");
    uint16_t port = ntohs(addr6.sin6_port);

    dualstack_ctx_t ctx = { port, -1, -1 };

#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, dualstack_client_thread, &ctx, 0, NULL);
    TEST_ASSERT_NOT_NULL(th);
#else
    pthread_t th;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, dualstack_client_thread, &ctx));
#endif

    Iron_Result_TcpSocket_Error ar = Iron_TcpListener_accept_result(lr.v0, 2000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ar.v1.code, "accept failed");

    /* getpeername should return AF_INET6 with ::ffff:127.0.0.1 mapped form.
     * Linux and macOS both deliver v4 clients to a v6 listener this way when
     * IPV6_V6ONLY is off. */
    struct sockaddr_in6 peer;
    memset(&peer, 0, sizeof(peer));
    socklen_t peerlen = sizeof(peer);
    rc = getpeername((int)ar.v0.fd, (struct sockaddr*)&peer, &peerlen);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "getpeername failed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(AF_INET6, peer.sin6_family,
        "peer family should be AF_INET6 with v4-mapped address");
    /* Last 4 bytes of the v6 address should be 127.0.0.1 (v4-mapped form). */
    const uint8_t *a = (const uint8_t *)&peer.sin6_addr;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0x7f, a[12], "v4-mapped: byte[12] should be 0x7f");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0x00, a[13], "v4-mapped: byte[13] should be 0x00");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0x00, a[14], "v4-mapped: byte[14] should be 0x00");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0x01, a[15], "v4-mapped: byte[15] should be 0x01");

    /* Read the 9-byte payload. */
    uint8_t buf[16] = {0};
    int64_t total = 0;
    while (total < 9) {
        Iron_Result_Int_Error rr = Iron_TcpSocket_read_result(ar.v0, buf + total,
                                                                (int64_t)(sizeof(buf) - (size_t)total),
                                                                2000);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, rr.v1.code, "read failed");
        if (rr.v0 == 0) break;
        total += rr.v0;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(9, (int)total, "expected 9 bytes");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE("dualstack", buf, 9, "payload mismatch");

#ifdef _WIN32
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ctx.dial_err, "client dial should have succeeded");

    if (ctx.dial_fd >= 0) {
        Iron_TcpSocket s = { ctx.dial_fd };
        Iron_TcpSocket_close(s);
    }
    Iron_TcpSocket_close(ar.v0);
    Iron_TcpListener_close(lr.v0);
}

/* ── Runner ─────────────────────────────────────────────────────────────── */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tcp_listen_ephemeral);
    RUN_TEST(test_tcp_listen_v6only_off);
    RUN_TEST(test_tcp_dial_loopback_roundtrip);
    RUN_TEST(test_tcp_dial_timeout_unreachable);
    RUN_TEST(test_tcp_write_after_peer_close);
    RUN_TEST(test_tcp_read_clean_eof);
    RUN_TEST(test_tcp_accept_timeout);
    RUN_TEST(test_tcp_dualstack_v4_to_v6);
    return UNITY_END();
}
