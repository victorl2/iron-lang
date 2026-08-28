/* test_stdlib_net_udp.c — Phase 59 P03 UDP stdlib Unity tests.
 *
 * Covers NET-07, NET-08, NET-09 (UDP bind / sendto / recvfrom + timeout):
 *
 *   1. test_udp_bind_ephemeral           bind 127.0.0.1:0 → fd>=0, port>0
 *   2. test_udp_sendto_recvfrom_roundtrip sendto+recvfrom on localhost
 *   3. test_udp_recvfrom_timeout          recvfrom(100) on empty socket → TIMEOUT
 *   4. test_udp_sendto_closed_socket      sendto after close → error
 *   5. test_udp_bind_v6_dual_stack        bind "::":0 → IPV6_V6ONLY==0
 *   6. test_udp_iron_packet_roundtrip      Iron-facing binary payload + sender
 *   7. test_udp_iron_packet_truncation     bounded prefix + explicit truncation
 *   8. test_udp_iron_zero_length_packet    empty success differs from timeout
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
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
#endif

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

static Iron_String make_iron_string(const char *s) {
    return iron_string_from_cstr(s, strlen(s));
}

/* ── Test 1: bind ephemeral port ──────────────────────────────────────── */
void test_udp_bind_ephemeral(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_UdpSocket_NetError r = Iron_Net_udp_bind_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.v1.code, "udp_bind should succeed");
    TEST_ASSERT_MESSAGE(r.v0.fd >= 0, "udp fd should be non-negative");

    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int rc = getsockname((int)r.v0.fd, (struct sockaddr*)&addr, &alen);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "getsockname failed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(AF_INET, addr.sin_family, "family should be AF_INET");
    uint16_t port = ntohs(addr.sin_port);
    TEST_ASSERT_MESSAGE(port > 0, "ephemeral port should be > 0");

    Iron_UdpSocket_close(r.v0);
}

/* ── Test 2: sendto + recvfrom roundtrip on loopback ──────────────────── */
void test_udp_sendto_recvfrom_roundtrip(void) {
    Iron_String host = make_iron_string("127.0.0.1");

    /* Server socket — bind to ephemeral, retrieve port */
    Iron_Result_UdpSocket_NetError rs = Iron_Net_udp_bind_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rs.v1.code, "server bind failed");
    struct sockaddr_in saddr;
    socklen_t salen = sizeof(saddr);
    TEST_ASSERT_EQUAL_INT(0, getsockname((int)rs.v0.fd,
                                          (struct sockaddr*)&saddr, &salen));
    uint16_t srv_port = ntohs(saddr.sin_port);

    /* Client socket — also bind to ephemeral so recvfrom on the server side
     * has a legitimate sender port to report back. */
    Iron_Result_UdpSocket_NetError rc = Iron_Net_udp_bind_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc.v1.code, "client bind failed");

    /* Client sends 12 bytes to the server's IPv4 address. */
    const char *msg_bytes = "hello, world";  /* 12 bytes */
    Iron_String buf = iron_string_from_cstr(msg_bytes, 12);

    Iron_IPv4Addr srv_v4 = { 127, 0, 0, 1 };
    Iron_Result_Int_NetError wr = Iron_Net_udp_sendto_v4_result(
        rc.v0, buf, srv_v4, (int64_t)srv_port, 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, wr.v1.code, "sendto should succeed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(12, (int)wr.v0, "sendto should report 12 bytes");

    /* Server recvfrom. */
    uint8_t rbuf[64] = {0};
    Iron_UdpRecvResult rr = Iron_UdpSocket_recvfrom_result(rs.v0, rbuf, sizeof(rbuf), 2000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rr.err.code, "recvfrom should succeed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(12, (int)rr.nbytes, "recvfrom should return 12 bytes");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(msg_bytes, rbuf, 12, "payload mismatch");
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, (int)rr.addr_family, "sender should be IPv4");
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, (int)iron_string_byte_len(&rr.addr_bytes),
        "IPv4 addr_bytes should be 4 octets");

    Iron_UdpSocket_close(rc.v0);
    Iron_UdpSocket_close(rs.v0);
}

/* ── Test 3: recvfrom with timeout on an empty socket ─────────────────── */
void test_udp_recvfrom_timeout(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_UdpSocket_NetError r = Iron_Net_udp_bind_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.v1.code, "bind failed");

    uint8_t buf[64];
    uint64_t t0 = Iron_monotonic_now_ms();
    Iron_UdpRecvResult rr = Iron_UdpSocket_recvfrom_result(r.v0, buf, sizeof(buf), 100);
    uint64_t t1 = Iron_monotonic_now_ms();

    TEST_ASSERT_EQUAL_INT_MESSAGE(IRON_ERR_NET_TIMEOUT, rr.err.code,
        "empty socket recvfrom(100) should fire TIMEOUT");
    uint64_t elapsed = t1 - t0;
    TEST_ASSERT_MESSAGE(elapsed >= 50 && elapsed <= 500,
        "recvfrom timeout elapsed should be in [50, 500]ms");

    Iron_UdpSocket_close(r.v0);
}

/* ── Test 4: sendto on a closed socket returns an error ───────────────── */
void test_udp_sendto_closed_socket(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_UdpSocket_NetError r = Iron_Net_udp_bind_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.v1.code, "bind failed");

    Iron_UdpSocket_close(r.v0);

    /* Synthesize a closed-fd sendto. On POSIX the fd was closed; on Windows
     * the SOCKET handle was closed. Either way, sendto should fail. */
    Iron_IPv4Addr dst = { 127, 0, 0, 1 };
    Iron_String buf = iron_string_from_cstr("x", 1);
    Iron_Result_Int_NetError wr = Iron_Net_udp_sendto_v4_result(
        r.v0, buf, dst, 9999, 100);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, wr.v1.code,
        "sendto on closed socket should return a typed error");
}

/* ── Test 5: dual-stack UDP listener sets IPV6_V6ONLY=0 ───────────────── */
void test_udp_bind_v6_dual_stack(void) {
    Iron_String host = make_iron_string("::");
    Iron_Result_UdpSocket_NetError r = Iron_Net_udp_bind_result(host, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.v1.code, "v6 udp_bind should succeed");

    int v6only = 1;
    socklen_t optlen = sizeof(v6only);
    int rc = getsockopt((int)r.v0.fd, IPPROTO_IPV6, IPV6_V6ONLY,
                         (char*)&v6only, &optlen);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "getsockopt failed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, v6only,
        "IPV6_V6ONLY should be 0 for dual-stack mode");

    Iron_UdpSocket_close(r.v0);
}

static uint16_t udp_bound_port(Iron_UdpSocket socket) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname((int)socket.fd, (struct sockaddr *)&addr, &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

/* ── Test 6: Iron-facing packet preserves arbitrary payload bytes ────── */
void test_udp_iron_packet_roundtrip(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_UdpSocket_NetError receiver = Iron_Net_udp_bind_result(host, 0);
    Iron_Result_UdpSocket_NetError sender = Iron_Net_udp_bind_result(host, 0);
    TEST_ASSERT_EQUAL_INT(0, receiver.v1.code);
    TEST_ASSERT_EQUAL_INT(0, sender.v1.code);

    const uint8_t payload[] = { 'a', 0, 'b', 0xff };
    Iron_String body = iron_string_from_cstr((const char *)payload, sizeof(payload));
    Iron_IPv4Addr loopback = { 127, 0, 0, 1 };
    Iron_Result_Int_NetError sent = Iron_Net_udp_sendto_v4_result(
        sender.v0, body, loopback, udp_bound_port(receiver.v0), 1000);
    TEST_ASSERT_EQUAL_INT(0, sent.v1.code);
    TEST_ASSERT_EQUAL_INT((int)sizeof(payload), (int)sent.v0);

    Iron_UdpPacket packet = Iron_udpsocket_recvfrom(receiver.v0, 64, 1000);
    TEST_ASSERT_EQUAL_INT(0, packet.error.code);
    TEST_ASSERT_EQUAL_INT(0, packet.truncated);
    TEST_ASSERT_EQUAL_INT((int)sizeof(payload),
                          (int)iron_string_byte_len(&packet.data));
    TEST_ASSERT_EQUAL_MEMORY(payload, iron_string_cstr(&packet.data), sizeof(payload));
    TEST_ASSERT_EQUAL_INT(9, (int)iron_string_byte_len(&packet.address));
    TEST_ASSERT_EQUAL_MEMORY("127.0.0.1", iron_string_cstr(&packet.address), 9);
    TEST_ASSERT_MESSAGE(packet.port > 0, "sender port should be reported");

    Iron_UdpSocket_close(sender.v0);
    Iron_UdpSocket_close(receiver.v0);
}

/* ── Test 7: oversized datagram returns the captured prefix ──────────── */
void test_udp_iron_packet_truncation(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_UdpSocket_NetError receiver = Iron_Net_udp_bind_result(host, 0);
    Iron_Result_UdpSocket_NetError sender = Iron_Net_udp_bind_result(host, 0);
    TEST_ASSERT_EQUAL_INT(0, receiver.v1.code);
    TEST_ASSERT_EQUAL_INT(0, sender.v1.code);

    Iron_IPv4Addr loopback = { 127, 0, 0, 1 };
    Iron_Result_Int_NetError sent = Iron_Net_udp_sendto_v4_result(
        sender.v0, make_iron_string("abcdefgh"), loopback,
        udp_bound_port(receiver.v0), 1000);
    TEST_ASSERT_EQUAL_INT(0, sent.v1.code);

    Iron_UdpPacket packet = Iron_udpsocket_recvfrom(receiver.v0, 4, 1000);
    TEST_ASSERT_EQUAL_INT(IRON_ERR_NET_MSG_TOO_LARGE, packet.error.code);
    TEST_ASSERT_EQUAL_INT(1, packet.truncated);
    TEST_ASSERT_EQUAL_INT(4, (int)iron_string_byte_len(&packet.data));
    TEST_ASSERT_EQUAL_MEMORY("abcd", iron_string_cstr(&packet.data), 4);
    TEST_ASSERT_EQUAL_MEMORY("127.0.0.1", iron_string_cstr(&packet.address), 9);

    Iron_UdpSocket_close(sender.v0);
    Iron_UdpSocket_close(receiver.v0);
}

/* ── Test 8: a zero-byte UDP packet is a successful receive ──────────── */
void test_udp_iron_zero_length_packet(void) {
    Iron_String host = make_iron_string("127.0.0.1");
    Iron_Result_UdpSocket_NetError receiver = Iron_Net_udp_bind_result(host, 0);
    Iron_Result_UdpSocket_NetError sender = Iron_Net_udp_bind_result(host, 0);
    TEST_ASSERT_EQUAL_INT(0, receiver.v1.code);
    TEST_ASSERT_EQUAL_INT(0, sender.v1.code);

    Iron_IPv4Addr loopback = { 127, 0, 0, 1 };
    Iron_Result_Int_NetError sent = Iron_Net_udp_sendto_v4_result(
        sender.v0, make_iron_string(""), loopback,
        udp_bound_port(receiver.v0), 1000);
    TEST_ASSERT_EQUAL_INT(0, sent.v1.code);

    Iron_UdpPacket packet = Iron_udpsocket_recvfrom(receiver.v0, 4, 1000);
    TEST_ASSERT_EQUAL_INT(0, packet.error.code);
    TEST_ASSERT_EQUAL_INT(0, (int)iron_string_byte_len(&packet.data));
    TEST_ASSERT_EQUAL_INT(9, (int)iron_string_byte_len(&packet.address));

    Iron_UdpSocket_close(sender.v0);
    Iron_UdpSocket_close(receiver.v0);
}

/* ── Runner ─────────────────────────────────────────────────────────────── */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_udp_bind_ephemeral);
    RUN_TEST(test_udp_sendto_recvfrom_roundtrip);
    RUN_TEST(test_udp_recvfrom_timeout);
    RUN_TEST(test_udp_sendto_closed_socket);
    RUN_TEST(test_udp_bind_v6_dual_stack);
    RUN_TEST(test_udp_iron_packet_roundtrip);
    RUN_TEST(test_udp_iron_packet_truncation);
    RUN_TEST(test_udp_iron_zero_length_packet);
    return UNITY_END();
}
