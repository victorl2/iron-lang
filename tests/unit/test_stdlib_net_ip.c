/* test_stdlib_net_ip.c — Phase 59 P03 IP address parse/format Unity tests.
 *
 * Covers NET-10 and NET-11 (IPv4 + IPv6 parse/format):
 *
 *   1. test_ipv4_parse_valid       parse "192.168.1.1" → octets {192,168,1,1}
 *   2. test_ipv4_parse_invalid     parse "999.0.0.0"   → IRON_ERR_NET_BAD_IP
 *   3. test_ipv4_format_roundtrip  parse "10.0.0.1" then format → "10.0.0.1"
 *   4. test_ipv6_parse_valid       parse "2001:db8::1" → correct octets
 *   5. test_ipv6_parse_with_zone   parse "fe80::1%eth0" → zone "eth0"
 *   6. test_ipv6_parse_url_zone    parse "fe80::1%25eth0" → zone "eth0"
 *   7. test_ipv6_parse_invalid     parse garbage → IRON_ERR_NET_BAD_IP
 *   8. test_ipv6_format_rfc5952    parse long form then format → canonical
 *   9. test_ipv6_format_with_zone  format an IPv6Addr with zone "eth0" → %eth0 suffix
 */

#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_errors.h"
#include "stdlib/iron_net.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

static Iron_String make_iron_string(const char *s) {
    return iron_string_from_cstr(s, strlen(s));
}

/* ── IPv4: parse valid ──────────────────────────────────────────────────── */
void test_ipv4_parse_valid(void) {
    Iron_String s = make_iron_string("192.168.1.1");
    Iron_Result_IPv4Addr_NetError r = Iron_Net_ipv4addr_parse_result(s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.v1.code, "parse 192.168.1.1 should succeed");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(192, r.v0.octets[0], "octet[0]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(168, r.v0.octets[1], "octet[1]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(  1, r.v0.octets[2], "octet[2]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(  1, r.v0.octets[3], "octet[3]");
}

/* ── IPv4: parse invalid returns IRON_ERR_NET_BAD_IP ───────────────────── */
void test_ipv4_parse_invalid(void) {
    Iron_String s = make_iron_string("999.0.0.0");
    Iron_Result_IPv4Addr_NetError r = Iron_Net_ipv4addr_parse_result(s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(IRON_ERR_NET_BAD_IP, r.v1.code,
        "invalid IPv4 should return IRON_ERR_NET_BAD_IP");
}

/* ── IPv4: parse then format roundtrips ─────────────────────────────────── */
void test_ipv4_format_roundtrip(void) {
    Iron_String s = make_iron_string("10.0.0.1");
    Iron_Result_IPv4Addr_NetError r = Iron_Net_ipv4addr_parse_result(s);
    TEST_ASSERT_EQUAL_INT(0, r.v1.code);

    Iron_String out = Iron_Net_ipv4addr_format(r.v0);
    Iron_String expect = make_iron_string("10.0.0.1");
    TEST_ASSERT_TRUE_MESSAGE(iron_string_equals(&out, &expect),
        "IPv4Addr.format should return '10.0.0.1'");
}

/* ── IPv6: parse compressed form ────────────────────────────────────────── */
void test_ipv6_parse_valid(void) {
    Iron_String s = make_iron_string("2001:db8::1");
    Iron_Result_IPv6Addr_NetError r = Iron_Net_ipv6addr_parse_result(s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.v1.code, "parse 2001:db8::1 should succeed");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x20, r.v0.octets[0],  "octet[0]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x01, r.v0.octets[1],  "octet[1]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x0d, r.v0.octets[2],  "octet[2]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xb8, r.v0.octets[3],  "octet[3]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x00, r.v0.octets[14], "octet[14]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x01, r.v0.octets[15], "octet[15]");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)iron_string_byte_len(&r.v0.zone),
        "zone should be empty when no %zone");
}

/* ── IPv6: parse with zone identifier ───────────────────────────────────── */
void test_ipv6_parse_with_zone(void) {
    Iron_String s = make_iron_string("fe80::1%eth0");
    Iron_Result_IPv6Addr_NetError r = Iron_Net_ipv6addr_parse_result(s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.v1.code, "parse fe80::1%eth0 should succeed");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xfe, r.v0.octets[0], "octet[0]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x80, r.v0.octets[1], "octet[1]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x01, r.v0.octets[15], "octet[15]");

    Iron_String expect = make_iron_string("eth0");
    TEST_ASSERT_TRUE_MESSAGE(iron_string_equals(&r.v0.zone, &expect),
        "zone should be 'eth0'");
}

/* ── IPv6: parse with URL-encoded %25 prefix ────────────────────────────── */
void test_ipv6_parse_url_zone(void) {
    Iron_String s = make_iron_string("fe80::1%25eth0");
    Iron_Result_IPv6Addr_NetError r = Iron_Net_ipv6addr_parse_result(s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.v1.code, "parse fe80::1%25eth0 should succeed");

    Iron_String expect = make_iron_string("eth0");
    TEST_ASSERT_TRUE_MESSAGE(iron_string_equals(&r.v0.zone, &expect),
        "zone should be 'eth0' with the '25' prefix stripped");
}

/* ── IPv6: parse invalid ────────────────────────────────────────────────── */
void test_ipv6_parse_invalid(void) {
    Iron_String s = make_iron_string("not:an:address:at:all:zzz");
    Iron_Result_IPv6Addr_NetError r = Iron_Net_ipv6addr_parse_result(s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(IRON_ERR_NET_BAD_IP, r.v1.code,
        "invalid IPv6 should return IRON_ERR_NET_BAD_IP");
}

/* ── IPv6: format emits RFC 5952 canonical form ─────────────────────────── */
void test_ipv6_format_rfc5952(void) {
    Iron_String s = make_iron_string("2001:0db8:0000:0000:0000:0000:0000:0001");
    Iron_Result_IPv6Addr_NetError r = Iron_Net_ipv6addr_parse_result(s);
    TEST_ASSERT_EQUAL_INT(0, r.v1.code);

    Iron_String out = Iron_Net_ipv6addr_format(r.v0);
    Iron_String expect = make_iron_string("2001:db8::1");
    TEST_ASSERT_TRUE_MESSAGE(iron_string_equals(&out, &expect),
        "IPv6Addr.format should return RFC 5952 canonical '2001:db8::1'");
}

/* ── IPv6: format with zone appends %zone ───────────────────────────────── */
void test_ipv6_format_with_zone(void) {
    Iron_String s = make_iron_string("fe80::1%eth0");
    Iron_Result_IPv6Addr_NetError r = Iron_Net_ipv6addr_parse_result(s);
    TEST_ASSERT_EQUAL_INT(0, r.v1.code);

    Iron_String out = Iron_Net_ipv6addr_format(r.v0);
    const char *cs = iron_string_cstr(&out);
    size_t len = iron_string_byte_len(&out);
    /* Last 5 bytes should be "%eth0" */
    TEST_ASSERT_MESSAGE(len >= 5, "format output too short");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE("%eth0", cs + len - 5, 5,
        "format output should end with '%eth0'");
}

/* ── Runner ─────────────────────────────────────────────────────────────── */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ipv4_parse_valid);
    RUN_TEST(test_ipv4_parse_invalid);
    RUN_TEST(test_ipv4_format_roundtrip);
    RUN_TEST(test_ipv6_parse_valid);
    RUN_TEST(test_ipv6_parse_with_zone);
    RUN_TEST(test_ipv6_parse_url_zone);
    RUN_TEST(test_ipv6_parse_invalid);
    RUN_TEST(test_ipv6_format_rfc5952);
    RUN_TEST(test_ipv6_format_with_zone);
    return UNITY_END();
}
