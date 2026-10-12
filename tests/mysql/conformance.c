/* MySQL wire protocol conformance tests over the in-process transport. */
#include "unity.h"
#include "runtime/iron_errors.h"
#include "stdlib/iron_mysql.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CLIENT_PROTOCOL_41   0x00000200u
#define CLIENT_DEPRECATE_EOF 0x01000000u
#define MAX_PACKET_CHUNK     0xFFFFFFu

typedef struct Buf {
    uint8_t *p;
    size_t n;
    size_t cap;
} Buf;

void setUp(void) { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

static void err_free(Iron_MysqlError *e) {
    iron_string_release(&e->sqlstate);
    iron_string_release(&e->message);
}

static void reserve(Buf *b, size_t add) {
    if (b->n + add <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + add) cap *= 2;
    b->p = (uint8_t *)realloc(b->p, cap);
    TEST_ASSERT_NOT_NULL(b->p);
    b->cap = cap;
}

static void bytes(Buf *b, const void *p, size_t n) {
    reserve(b, n);
    if (n) memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void u8(Buf *b, uint8_t v) { bytes(b, &v, 1); }

static void lenenc(Buf *b, uint64_t v) {
    Iron_MysqlTestBytes e;
    Iron_MysqlError err = iron_mysql_test_encode_lenenc(v, &e);
    TEST_ASSERT_EQUAL_INT64(0, err.code);
    bytes(b, e.data, e.len);
    iron_mysql_test_bytes_free(&e);
}

static void lenstr(Buf *b, const void *p, size_t n) {
    lenenc(b, n);
    bytes(b, p, n);
}

static void packet(Buf *wire, uint8_t sequence, const void *payload, size_t n) {
    uint8_t h[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), sequence };
    bytes(wire, h, sizeof(h));
    bytes(wire, payload, n);
}

static void column(Buf *p, const char *name, uint8_t type, uint16_t flags) {
    lenstr(p, "def", 3);
    lenstr(p, "db", 2);
    lenstr(p, "t", 1);
    lenstr(p, "t", 1);
    lenstr(p, name, strlen(name));
    lenstr(p, name, strlen(name));
    u8(p, 0x0c);
    const uint8_t fixed[12] = {
        45, 0, 0xff, 0xff, 0, 0, type,
        (uint8_t)flags, (uint8_t)(flags >> 8), 0, 0, 0
    };
    bytes(p, fixed, sizeof(fixed));
}

static Buf one_text_result(void) {
    Buf wire = { 0 }, p = { 0 };
    u8(&p, 1);
    packet(&wire, 1, p.p, p.n);
    p.n = 0;
    column(&p, "value", 253, 0);
    packet(&wire, 2, p.p, p.n);
    p.n = 0;
    lenstr(&p, "hello", 5);
    packet(&wire, 3, p.p, p.n);
    const uint8_t end[] = { 0xfe, 0, 0, 2, 0, 0, 0 };
    packet(&wire, 4, end, sizeof(end));
    free(p.p);
    return wire;
}

static void assert_result_ok(int64_t result) {
    Iron_MysqlError e = Iron_mysqlnative_res_error(result);
    if (e.code) TEST_FAIL_MESSAGE(iron_string_cstr(&e.message));
    err_free(&e);
    TEST_ASSERT_EQUAL_INT64(1, Iron_mysqlnative_res_info(result, 0));
    TEST_ASSERT_EQUAL_INT64(1, Iron_mysqlnative_res_info(result, 1));
    Iron_MysqlStringResult s = Iron_mysqlnative_res_text(result, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, s.v1.code);
    TEST_ASSERT_EQUAL_size_t(5, iron_string_byte_len(&s.v0));
    TEST_ASSERT_EQUAL_MEMORY("hello", iron_string_cstr(&s.v0), 5);
    iron_string_release(&s.v0);
    err_free(&s.v1);
}

static void test_packet_header_and_every_read_split(void) {
    Buf wire = one_text_result();
    for (size_t split = 1; split <= wire.n; split++) {
        int64_t r = iron_mysql_test_decode_result(
            wire.p, wire.n, split, CLIENT_PROTOCOL_41 | CLIENT_DEPRECATE_EOF, 1, false);
        assert_result_ok(r);
        Iron_mysqlnative_res_free(r);
    }
    free(wire.p);
}

static void test_packet_continuation_and_empty_trailer(void) {
    size_t total = MAX_PACKET_CHUNK + 3;
    Buf wire = { 0 };
    uint8_t *payload = (uint8_t *)malloc(total);
    TEST_ASSERT_NOT_NULL(payload);
    for (size_t i = 0; i < total; i++) payload[i] = (uint8_t)(i * 131u + 7u);
    packet(&wire, 9, payload, MAX_PACKET_CHUNK);
    packet(&wire, 10, payload + MAX_PACKET_CHUNK, 3);
    Iron_MysqlTestBytes got;
    Iron_MysqlError e = iron_mysql_test_read_packet(
        wire.p, wire.n, 8191, 9, total, &got);
    TEST_ASSERT_EQUAL_INT64(0, e.code);
    TEST_ASSERT_EQUAL_size_t(total, got.len);
    TEST_ASSERT_EQUAL_MEMORY(payload, got.data, total);
    err_free(&e);
    iron_mysql_test_bytes_free(&got);
    free(wire.p);

    wire = (Buf){ 0 };
    packet(&wire, 250, payload, MAX_PACKET_CHUNK);
    packet(&wire, 251, NULL, 0);
    e = iron_mysql_test_read_packet(wire.p, wire.n, 4093, 250, MAX_PACKET_CHUNK, &got);
    TEST_ASSERT_EQUAL_INT64(0, e.code);
    TEST_ASSERT_EQUAL_size_t(MAX_PACKET_CHUNK, got.len);
    TEST_ASSERT_EQUAL_MEMORY(payload, got.data, MAX_PACKET_CHUNK);
    err_free(&e);
    iron_mysql_test_bytes_free(&got);
    free(payload);
    free(wire.p);
}

static void test_packet_rejects_bad_sequence_truncation_and_limit(void) {
    const uint8_t good[] = { 3, 0, 0, 4, 'a', 'b', 'c' };
    for (size_t n = 0; n < sizeof(good); n++) {
        Iron_MysqlTestBytes got;
        Iron_MysqlError e = iron_mysql_test_read_packet(good, n, 1, 4, 10, &got);
        TEST_ASSERT_NOT_EQUAL(0, e.code);
        err_free(&e);
    }
    Iron_MysqlTestBytes got;
    Iron_MysqlError e = iron_mysql_test_read_packet(good, sizeof(good), 7, 3, 10, &got);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PROTOCOL, e.code);
    err_free(&e);
    e = iron_mysql_test_read_packet(good, sizeof(good), 7, 4, 2, &got);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PACKET_TOO_LARGE, e.code);
    err_free(&e);
}

static void test_length_encoded_integer_size_classes(void) {
    static const uint64_t values[] = {
        0, 250, 251, 65535, 65536, 0xffffff, 0x1000000, UINT64_MAX
    };
    static const size_t sizes[] = { 1, 1, 3, 3, 4, 4, 9, 9 };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        Iron_MysqlTestBytes encoded;
        Iron_MysqlError e = iron_mysql_test_encode_lenenc(values[i], &encoded);
        TEST_ASSERT_EQUAL_INT64(0, e.code);
        TEST_ASSERT_EQUAL_size_t(sizes[i], encoded.len);
        uint64_t value = 0;
        bool is_null = true;
        size_t used = 0;
        e = iron_mysql_test_decode_lenenc(encoded.data, encoded.len, &value, &is_null, &used);
        TEST_ASSERT_EQUAL_INT64(0, e.code);
        TEST_ASSERT_EQUAL_UINT64(values[i], value);
        TEST_ASSERT_FALSE(is_null);
        TEST_ASSERT_EQUAL_size_t(encoded.len, used);
        iron_mysql_test_bytes_free(&encoded);
    }
    const uint8_t null_value[] = { 0xfb };
    uint64_t value;
    bool is_null;
    size_t used;
    Iron_MysqlError e = iron_mysql_test_decode_lenenc(
        null_value, sizeof(null_value), &value, &is_null, &used);
    TEST_ASSERT_EQUAL_INT64(0, e.code);
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EQUAL_size_t(1, used);
    const uint8_t invalid[][8] = {
        { 0xfc }, { 0xfd, 1, 2 }, { 0xfe, 1, 2, 3, 4, 5, 6, 7 }, { 0xff }
    };
    const size_t lengths[] = { 1, 3, 8, 1 };
    for (size_t i = 0; i < 4; i++) {
        e = iron_mysql_test_decode_lenenc(invalid[i], lengths[i], &value, &is_null, &used);
        TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PROTOCOL, e.code);
        err_free(&e);
    }
}

static void test_truncated_result_is_always_an_error(void) {
    Buf wire = one_text_result();
    for (size_t n = 0; n < wire.n; n++) {
        int64_t r = iron_mysql_test_decode_result(
            wire.p, n, 3, CLIENT_PROTOCOL_41 | CLIENT_DEPRECATE_EOF, 1, false);
        Iron_MysqlError e = Iron_mysqlnative_res_error(r);
        TEST_ASSERT_NOT_EQUAL(0, e.code);
        err_free(&e);
        Iron_mysqlnative_res_free(r);
    }
    free(wire.p);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_packet_header_and_every_read_split);
    RUN_TEST(test_packet_continuation_and_empty_trailer);
    RUN_TEST(test_packet_rejects_bad_sequence_truncation_and_limit);
    RUN_TEST(test_length_encoded_integer_size_classes);
    RUN_TEST(test_truncated_result_is_always_an_error);
    return UNITY_END();
}
