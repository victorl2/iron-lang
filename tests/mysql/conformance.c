/* MySQL wire protocol conformance tests over the in-process transport. */
#include "unity.h"
#include "runtime/iron_errors.h"
#include "stdlib/iron_mysql.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CLIENT_PROTOCOL_41   0x00000200u
#define CLIENT_DEPRECATE_EOF 0x01000000u
#define MAX_PACKET_CHUNK     0xFFFFFFu
#define FLAG_UNSIGNED        32

enum {
    T_DECIMAL = 0, T_TINY = 1, T_SHORT = 2, T_LONG = 3, T_FLOAT = 4,
    T_DOUBLE = 5, T_TIMESTAMP = 7, T_LONGLONG = 8, T_INT24 = 9,
    T_DATE = 10, T_TIME = 11, T_DATETIME = 12, T_YEAR = 13,
    T_VARCHAR = 15, T_BIT = 16, T_JSON = 245, T_NEWDECIMAL = 246,
    T_ENUM = 247, T_SET = 248, T_TINY_BLOB = 249, T_MEDIUM_BLOB = 250,
    T_LONG_BLOB = 251, T_BLOB = 252, T_VAR_STRING = 253, T_STRING = 254
};

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

static Iron_String S(const char *s) { return iron_string_from_cstr(s, strlen(s)); }

static Iron_MysqlConfig test_config(void) {
    Iron_MysqlConfig c;
    memset(&c, 0, sizeof(c));
    c.host = S("memory"); c.port = 3306; c.user = S("tester"); c.password = S("secret");
    c.database = S("db"); c.tls = IRON_MYSQL_TLS_DISABLED;
    c.ca_file = S(""); c.server_public_key = S(""); c.collation = S("");
    c.timeout = 5000; c.connect_timeout = 5000; c.max_allowed_packet = 64 * 1024 * 1024;
    return c;
}

static void config_free(Iron_MysqlConfig *c) {
    iron_string_release(&c->host); iron_string_release(&c->user);
    iron_string_release(&c->password); iron_string_release(&c->database);
    iron_string_release(&c->ca_file); iron_string_release(&c->server_public_key);
    iron_string_release(&c->collation);
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

static void u16(Buf *b, uint16_t v) {
    uint8_t p[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    bytes(b, p, sizeof(p));
}

static void u32(Buf *b, uint32_t v) {
    uint8_t p[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 24) };
    bytes(b, p, sizeof(p));
}

static void u64(Buf *b, uint64_t v) {
    u32(b, (uint32_t)v);
    u32(b, (uint32_t)(v >> 32));
}

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

static Buf handshake_wire(const char *plugin) {
    const uint32_t caps = 0x00000001u | 0x00000004u | 0x00000008u | CLIENT_PROTOCOL_41 |
        0x00002000u | 0x00008000u | 0x00020000u | 0x00040000u | 0x00080000u |
        0x00200000u | CLIENT_DEPRECATE_EOF;
    Buf wire = { 0 }, p = { 0 };
    u8(&p, 10); bytes(&p, "8.0.36-conformance", strlen("8.0.36-conformance")); u8(&p, 0);
    u32(&p, 42);
    for (uint8_t i = 1; i <= 8; i++) u8(&p, i);
    u8(&p, 0); u16(&p, (uint16_t)caps); u8(&p, 45); u16(&p, 2);
    u16(&p, (uint16_t)(caps >> 16)); u8(&p, 21);
    for (int i = 0; i < 10; i++) u8(&p, 0);
    for (uint8_t i = 9; i <= 20; i++) u8(&p, i);
    u8(&p, 0); bytes(&p, plugin, strlen(plugin) + 1);
    packet(&wire, 0, p.p, p.n);
    free(p.p);
    return wire;
}

static void ok_packet(Buf *wire, uint8_t sequence) {
    const uint8_t ok[] = { 0x00, 0, 0, 2, 0, 0, 0 };
    packet(wire, sequence, ok, sizeof(ok));
}

static void column_ex(Buf *p, const char *name, uint8_t type, uint16_t flags,
                      uint8_t decimals, uint16_t charset, uint32_t length) {
    lenstr(p, "def", 3);
    lenstr(p, "db", 2);
    lenstr(p, "t", 1);
    lenstr(p, "t", 1);
    lenstr(p, name, strlen(name));
    lenstr(p, name, strlen(name));
    u8(p, 0x0c);
    u16(p, charset);
    u32(p, length);
    u8(p, type);
    u16(p, flags);
    u8(p, decimals);
    u16(p, 0);
}

static void column(Buf *p, const char *name, uint8_t type, uint16_t flags) {
    column_ex(p, name, type, flags, 0, 45, 0xffff);
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

static int64_t result_error(Buf *wire, uint32_t caps) {
    int64_t r = iron_mysql_test_decode_result(wire->p, wire->n, 2, caps, 1, false);
    Iron_MysqlError e = Iron_mysqlnative_res_error(r);
    int64_t code = e.code;
    err_free(&e);
    Iron_mysqlnative_res_free(r);
    return code;
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

static void test_column_definition_rejects_inconsistent_fixed_fields(void) {
    Buf wire = { 0 }, p = { 0 };
    u8(&p, 1);
    packet(&wire, 1, p.p, p.n);
    p.n = 0;
    column(&p, "v", 253, 0);
    p.p[p.n - 13] = 1;
    packet(&wire, 2, p.p, p.n);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PROTOCOL,
                            result_error(&wire, CLIENT_PROTOCOL_41 | CLIENT_DEPRECATE_EOF));
    free(wire.p);

    wire = (Buf){ 0 };
    p.n = 0;
    u8(&p, 1);
    packet(&wire, 1, p.p, p.n);
    p.n = 0;
    column(&p, "v", 253, 0);
    u8(&p, 0xaa);
    packet(&wire, 2, p.p, p.n);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PROTOCOL,
                            result_error(&wire, CLIENT_PROTOCOL_41 | CLIENT_DEPRECATE_EOF));
    free(p.p);
    free(wire.p);
}

static Buf legacy_result_with_eof(const uint8_t *eof, size_t eof_len) {
    Buf wire = { 0 }, p = { 0 };
    u8(&p, 1);
    packet(&wire, 1, p.p, p.n);
    p.n = 0;
    column(&p, "v", 253, 0);
    packet(&wire, 2, p.p, p.n);
    packet(&wire, 3, eof, eof_len);
    p.n = 0;
    lenstr(&p, "x", 1);
    packet(&wire, 4, p.p, p.n);
    packet(&wire, 5, eof, eof_len);
    free(p.p);
    return wire;
}

static void test_legacy_eof_requires_warnings_and_status(void) {
    const uint8_t eof[] = { 0xfe, 0, 0, 2, 0 };
    Buf wire = legacy_result_with_eof(eof, sizeof(eof));
    int64_t r = iron_mysql_test_decode_result(
        wire.p, wire.n, 1, CLIENT_PROTOCOL_41, 1, false);
    Iron_MysqlError e = Iron_mysqlnative_res_error(r);
    TEST_ASSERT_EQUAL_INT64(0, e.code);
    err_free(&e);
    Iron_mysqlnative_res_free(r);
    free(wire.p);

    for (size_t n = 1; n < sizeof(eof); n++) {
        wire = legacy_result_with_eof(eof, n);
        TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PROTOCOL,
                                result_error(&wire, CLIENT_PROTOCOL_41));
        free(wire.p);
    }
}

static void test_err_packet_requires_error_code_and_sqlstate(void) {
    const uint8_t short_err[][3] = { { 0xff }, { 0xff, 1 }, { 0xff, 1, 2 } };
    const size_t lengths[] = { 1, 2, 3 };
    for (size_t i = 0; i < 3; i++) {
        Buf wire = { 0 };
        packet(&wire, 1, short_err[i], lengths[i]);
        TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PROTOCOL,
                                result_error(&wire, CLIENT_PROTOCOL_41));
        free(wire.p);
    }
    const uint8_t err[] = { 0xff, 0x15, 0x04, '#', '4', '2', 'S', '0', '2', 'b', 'a', 'd' };
    Buf wire = { 0 };
    packet(&wire, 1, err, sizeof(err));
    TEST_ASSERT_EQUAL_INT64(1045, result_error(&wire, CLIENT_PROTOCOL_41));
    free(wire.p);
}

typedef struct ColCase {
    const char *name;
    uint8_t type;
    uint16_t flags;
    uint8_t decimals;
} ColCase;

static const ColCase wide_cols[] = {
    { "i8", T_TINY, 0, 0 }, { "u8", T_TINY, FLAG_UNSIGNED, 0 },
    { "i16", T_SHORT, 0, 0 }, { "u16", T_SHORT, FLAG_UNSIGNED, 0 },
    { "i24", T_INT24, 0, 0 }, { "u24", T_INT24, FLAG_UNSIGNED, 0 },
    { "i32", T_LONG, 0, 0 }, { "u32", T_LONG, FLAG_UNSIGNED, 0 },
    { "i64", T_LONGLONG, 0, 0 }, { "u64", T_LONGLONG, FLAG_UNSIGNED, 0 },
    { "dec", T_NEWDECIMAL, 0, 6 }, { "flt", T_FLOAT, 0, 0 },
    { "dbl", T_DOUBLE, 0, 0 }, { "date", T_DATE, 0, 0 },
    { "time", T_TIME, 0, 6 }, { "dt", T_DATETIME, 0, 6 },
    { "ts", T_TIMESTAMP, 0, 3 }, { "year", T_YEAR, 0, 0 },
    { "bit", T_BIT, FLAG_UNSIGNED, 0 }, { "json", T_JSON, 0, 0 },
    { "enum", T_ENUM, 0, 0 }, { "set", T_SET, 0, 0 },
    { "tinyblob", T_TINY_BLOB, 0, 0 }, { "blob", T_BLOB, 0, 0 },
    { "mediumblob", T_MEDIUM_BLOB, 0, 0 }, { "longblob", T_LONG_BLOB, 0, 0 },
    { "varchar", T_VARCHAR, 0, 0 }, { "varstr", T_VAR_STRING, 0, 0 },
    { "text", T_STRING, 0, 0 }, { "null_a", T_VAR_STRING, 0, 0 },
    { "null_b", T_LONG, 0, 0 }, { "null_c", T_BLOB, 0, 0 }
};

static void wide_columns(Buf *wire, uint8_t *sequence) {
    Buf p = { 0 };
    lenenc(&p, sizeof(wide_cols) / sizeof(wide_cols[0]));
    packet(wire, (*sequence)++, p.p, p.n);
    for (size_t i = 0; i < sizeof(wide_cols) / sizeof(wide_cols[0]); i++) {
        p.n = 0;
        uint16_t charset = wide_cols[i].type >= T_TINY_BLOB &&
                           wide_cols[i].type <= T_BLOB ? 63 : 45;
        column_ex(&p, wide_cols[i].name, wide_cols[i].type, wide_cols[i].flags,
                  wide_cols[i].decimals, charset, 0xffffffffu);
        packet(wire, (*sequence)++, p.p, p.n);
    }
    free(p.p);
}

static Buf wide_binary_result(void) {
    Buf wire = { 0 }, row = { 0 };
    uint8_t sequence = 1;
    wide_columns(&wire, &sequence);
    size_t ncols = sizeof(wide_cols) / sizeof(wide_cols[0]);
    size_t bitmap_len = (ncols + 9) / 8;
    u8(&row, 0);
    reserve(&row, bitmap_len);
    memset(row.p + row.n, 0, bitmap_len);
    row.n += bitmap_len;
    for (size_t col = 29; col < 32; col++) row.p[1 + (col + 2) / 8] |= 1u << ((col + 2) % 8);
    u8(&row, 0x80); u8(&row, 0xff);
    u16(&row, 0x8000); u16(&row, 0xffff);
    u32(&row, 0xff800000u); u32(&row, 0x00ffffffu);
    u32(&row, 0x80000000u); u32(&row, 0xffffffffu);
    u64(&row, UINT64_C(0x8000000000000000)); u64(&row, UINT64_MAX);
    lenstr(&row, "-12345678901234.125000", strlen("-12345678901234.125000"));
    float f = -1.25f; uint32_t fb; memcpy(&fb, &f, sizeof(fb)); u32(&row, fb);
    double d = 3.141592653589793; uint64_t db; memcpy(&db, &d, sizeof(db)); u64(&row, db);
    u8(&row, 4); u16(&row, 2024); u8(&row, 2); u8(&row, 29);
    u8(&row, 12); u8(&row, 1); u32(&row, 5); u8(&row, 3); u8(&row, 4);
    u8(&row, 5); u32(&row, 678901);
    u8(&row, 11); u16(&row, 2024); u8(&row, 2); u8(&row, 29); u8(&row, 13);
    u8(&row, 45); u8(&row, 30); u32(&row, 123456);
    u8(&row, 11); u16(&row, 2038); u8(&row, 1); u8(&row, 19); u8(&row, 3);
    u8(&row, 14); u8(&row, 7); u32(&row, 500000);
    u16(&row, 2155);
    const uint8_t bit[] = { 0xa5 }; lenstr(&row, bit, sizeof(bit));
    lenstr(&row, "{\"k\":[1,2]}", 11); lenstr(&row, "green", 5); lenstr(&row, "x,y", 3);
    lenstr(&row, "", 0);
    uint8_t b250[250]; memset(b250, 'b', sizeof(b250)); lenstr(&row, b250, sizeof(b250));
    uint8_t b251[251]; memset(b251, 'm', sizeof(b251)); lenstr(&row, b251, sizeof(b251));
    size_t large_n = 65536;
    uint8_t *large = (uint8_t *)malloc(large_n); TEST_ASSERT_NOT_NULL(large);
    memset(large, 'L', large_n); lenstr(&row, large, large_n); free(large);
    lenstr(&row, "hello", 5); lenstr(&row, "world", 5); lenstr(&row, "text", 4);
    packet(&wire, sequence++, row.p, row.n);
    const uint8_t end[] = { 0xfe, 0, 0, 2, 0, 0, 0 };
    packet(&wire, sequence, end, sizeof(end));
    free(row.p);
    return wire;
}

static Buf wide_text_result(void) {
    static const char *values[] = {
        "-128", "255", "-32768", "65535", "-8388608", "16777215",
        "-2147483648", "4294967295", "-9223372036854775808",
        "18446744073709551615", "-12345678901234.125000", "-1.25",
        "3.141592653589793", "2024-02-29", "-123:04:05.678901",
        "2024-02-29 13:45:30.123456", "2038-01-19 03:14:07.500",
        "2155", "\xa5", "{\"k\":[1,2]}", "green", "x,y"
    };
    Buf wire = { 0 }, row = { 0 };
    uint8_t sequence = 1;
    wide_columns(&wire, &sequence);
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        lenstr(&row, values[i], strlen(values[i]));
    lenstr(&row, "", 0);
    uint8_t b250[250]; memset(b250, 'b', sizeof(b250)); lenstr(&row, b250, sizeof(b250));
    uint8_t b251[251]; memset(b251, 'm', sizeof(b251)); lenstr(&row, b251, sizeof(b251));
    size_t large_n = 65536;
    uint8_t *large = (uint8_t *)malloc(large_n); TEST_ASSERT_NOT_NULL(large);
    memset(large, 'L', large_n); lenstr(&row, large, large_n); free(large);
    lenstr(&row, "hello", 5); lenstr(&row, "world", 5); lenstr(&row, "text", 4);
    u8(&row, 0xfb); u8(&row, 0xfb); u8(&row, 0xfb);
    packet(&wire, sequence++, row.p, row.n);
    const uint8_t end[] = { 0xfe, 0, 0, 2, 0, 0, 0 };
    packet(&wire, sequence, end, sizeof(end));
    free(row.p);
    return wire;
}

static Iron_MysqlStringResult text_at(int64_t r, int col) {
    Iron_MysqlStringResult s = Iron_mysqlnative_res_text(r, 0, col);
    TEST_ASSERT_EQUAL_INT64(0, s.v1.code);
    err_free(&s.v1);
    return s;
}

static void test_binary_rows_cover_every_column_type_and_null_offsets(void) {
    Buf wire = wide_binary_result();
    int64_t r = iron_mysql_test_decode_result(
        wire.p, wire.n, 7, CLIENT_PROTOCOL_41 | CLIENT_DEPRECATE_EOF, 1, true);
    Iron_MysqlError e = Iron_mysqlnative_res_error(r);
    if (e.code) TEST_FAIL_MESSAGE(iron_string_cstr(&e.message));
    err_free(&e);
    TEST_ASSERT_EQUAL_INT64(32, Iron_mysqlnative_res_info(r, 1));
    static const int64_t signed_want[] = { -128, -32768, -8388608, INT32_MIN, INT64_MIN };
    static const int signed_col[] = { 0, 2, 4, 6, 8 };
    for (size_t i = 0; i < 5; i++) {
        Iron_MysqlIntResult v = Iron_mysqlnative_res_int(r, 0, signed_col[i]);
        TEST_ASSERT_EQUAL_INT64(0, v.v1.code);
        TEST_ASSERT_EQUAL_INT64(signed_want[i], v.v0);
    }
    static const uint64_t unsigned_want[] = { 255, 65535, 0xffffff, UINT32_MAX, UINT64_MAX };
    static const int unsigned_col[] = { 1, 3, 5, 7, 9 };
    for (size_t i = 0; i < 5; i++) {
        Iron_MysqlUIntResult v = Iron_mysqlnative_res_uint(r, 0, unsigned_col[i]);
        TEST_ASSERT_EQUAL_INT64(0, v.v1.code);
        TEST_ASSERT_EQUAL_UINT64(unsigned_want[i], v.v0);
    }
    Iron_MysqlFloatResult fv = Iron_mysqlnative_res_float(r, 0, 11);
    TEST_ASSERT_DOUBLE_WITHIN(0.000001, -1.25, fv.v0);
    fv = Iron_mysqlnative_res_float(r, 0, 12);
    TEST_ASSERT_DOUBLE_WITHIN(1e-15, 3.141592653589793, fv.v0);
    Iron_MysqlDateTimeResult date = Iron_mysqlnative_res_datetime(r, 0, 13);
    TEST_ASSERT_EQUAL_INT64(2024, date.v0.year); TEST_ASSERT_EQUAL_INT64(29, date.v0.day);
    Iron_MysqlTimeResult time = Iron_mysqlnative_res_time(r, 0, 14);
    TEST_ASSERT_TRUE(time.v0.negative); TEST_ASSERT_EQUAL_INT64(123, time.v0.hours);
    TEST_ASSERT_EQUAL_INT64(678901, time.v0.microsecond);
    Iron_MysqlDateTimeResult dt = Iron_mysqlnative_res_datetime(r, 0, 15);
    TEST_ASSERT_EQUAL_INT64(123456, dt.v0.microsecond);
    dt = Iron_mysqlnative_res_datetime(r, 0, 16);
    TEST_ASSERT_EQUAL_INT64(2038, dt.v0.year);
    Iron_MysqlIntResult year = Iron_mysqlnative_res_int(r, 0, 17);
    TEST_ASSERT_EQUAL_INT64(2155, year.v0);
    Iron_MysqlUIntResult bit = Iron_mysqlnative_res_uint(r, 0, 18);
    TEST_ASSERT_EQUAL_UINT64(0xa5, bit.v0);
    for (int col = 19; col <= 28; col++) {
        Iron_MysqlStringResult s = text_at(r, col);
        if (col == 23) TEST_ASSERT_EQUAL_size_t(250, iron_string_byte_len(&s.v0));
        if (col == 24) TEST_ASSERT_EQUAL_size_t(251, iron_string_byte_len(&s.v0));
        if (col == 25) TEST_ASSERT_EQUAL_size_t(65536, iron_string_byte_len(&s.v0));
        iron_string_release(&s.v0);
    }
    TEST_ASSERT_TRUE(Iron_mysqlnative_res_is_null(r, 0, 29));
    TEST_ASSERT_TRUE(Iron_mysqlnative_res_is_null(r, 0, 30));
    TEST_ASSERT_TRUE(Iron_mysqlnative_res_is_null(r, 0, 31));
    Iron_mysqlnative_res_free(r);
    free(wire.p);
}

static void column_free(Iron_MysqlColumn *col) {
    iron_string_release(&col->name);
    iron_string_release(&col->org_name);
    iron_string_release(&col->table);
    iron_string_release(&col->org_table);
    iron_string_release(&col->schema);
    iron_string_release(&col->type_name);
}

static void test_text_rows_cover_every_column_type(void) {
    Buf wire = wide_text_result();
    int64_t r = iron_mysql_test_decode_result(
        wire.p, wire.n, 13, CLIENT_PROTOCOL_41 | CLIENT_DEPRECATE_EOF, 1, false);
    Iron_MysqlError e = Iron_mysqlnative_res_error(r);
    if (e.code) TEST_FAIL_MESSAGE(iron_string_cstr(&e.message));
    err_free(&e);
    for (size_t i = 0; i < sizeof(wide_cols) / sizeof(wide_cols[0]); i++) {
        Iron_MysqlColumn col = Iron_mysqlnative_res_column(r, (int64_t)i);
        TEST_ASSERT_EQUAL_INT(wide_cols[i].type, col.type);
        TEST_ASSERT_EQUAL_INT(wide_cols[i].flags, col.flags);
        column_free(&col);
    }
    Iron_MysqlIntResult i = Iron_mysqlnative_res_int(r, 0, 8);
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, i.v0);
    Iron_MysqlUIntResult u = Iron_mysqlnative_res_uint(r, 0, 9);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, u.v0);
    Iron_MysqlDateTimeResult dt = Iron_mysqlnative_res_datetime(r, 0, 15);
    TEST_ASSERT_EQUAL_INT64(123456, dt.v0.microsecond);
    Iron_MysqlTimeResult tm = Iron_mysqlnative_res_time(r, 0, 14);
    TEST_ASSERT_TRUE(tm.v0.negative);
    TEST_ASSERT_EQUAL_INT64(123, tm.v0.hours);
    TEST_ASSERT_EQUAL_INT64(678901, tm.v0.microsecond);
    for (int col = 22; col <= 28; col++) {
        Iron_MysqlStringResult s = text_at(r, col);
        if (col == 23) TEST_ASSERT_EQUAL_size_t(250, iron_string_byte_len(&s.v0));
        if (col == 24) TEST_ASSERT_EQUAL_size_t(251, iron_string_byte_len(&s.v0));
        if (col == 25) TEST_ASSERT_EQUAL_size_t(65536, iron_string_byte_len(&s.v0));
        iron_string_release(&s.v0);
    }
    TEST_ASSERT_TRUE(Iron_mysqlnative_res_is_null(r, 0, 29));
    TEST_ASSERT_TRUE(Iron_mysqlnative_res_is_null(r, 0, 30));
    TEST_ASSERT_TRUE(Iron_mysqlnative_res_is_null(r, 0, 31));
    Iron_mysqlnative_res_free(r);
    free(wire.p);
}

static void test_handshake_v10_and_caching_sha2_fast_path(void) {
    Buf base = handshake_wire("caching_sha2_password"), wire = { 0 };
    bytes(&wire, base.p, base.n);
    const uint8_t fast[] = { 1, 3 };
    packet(&wire, 2, fast, sizeof(fast));
    ok_packet(&wire, 3);
    Iron_MysqlConfig cfg = test_config();
    for (size_t split = 1; split <= wire.n; split++) {
        Iron_MysqlTestBytes written;
        Iron_MysqlError e = iron_mysql_test_handshake(wire.p, wire.n, split, cfg, &written);
        TEST_ASSERT_EQUAL_INT64(0, e.code);
        TEST_ASSERT_TRUE(written.len > 40);
        TEST_ASSERT_EQUAL_UINT8(1, written.data[3]);
        TEST_ASSERT_EQUAL_MEMORY("tester", written.data + 4 + 32, 6);
        err_free(&e);
        iron_mysql_test_bytes_free(&written);
    }
    config_free(&cfg);
    free(base.p); free(wire.p);
}

static void test_auth_switch_to_mysql_native_password(void) {
    Buf base = handshake_wire("caching_sha2_password"), wire = { 0 }, sw = { 0 };
    bytes(&wire, base.p, base.n);
    u8(&sw, 0xfe); bytes(&sw, "mysql_native_password", 22);
    for (uint8_t i = 20; i > 0; i--) u8(&sw, i);
    u8(&sw, 0);
    packet(&wire, 2, sw.p, sw.n);
    ok_packet(&wire, 4);
    Iron_MysqlConfig cfg = test_config();
    Iron_MysqlTestBytes written;
    Iron_MysqlError e = iron_mysql_test_handshake(wire.p, wire.n, 1, cfg, &written);
    TEST_ASSERT_EQUAL_INT64(0, e.code);
    TEST_ASSERT_TRUE(written.len > 24);
    size_t second = ((size_t)written.data[0] | ((size_t)written.data[1] << 8) |
                     ((size_t)written.data[2] << 16)) + 4;
    TEST_ASSERT_EQUAL_size_t(20, (size_t)written.data[second]);
    TEST_ASSERT_EQUAL_UINT8(3, written.data[second + 3]);
    err_free(&e); iron_mysql_test_bytes_free(&written); config_free(&cfg);
    free(base.p); free(wire.p); free(sw.p);
}

static const char test_public_key[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA3hnoAUClC6EhTw/eB+iG\n"
    "WUlza0J+pzFoVKQuvZXSspYN5xkTs6j458+Pm5SCalZNEsfeWyp2+/7IBGMls7Fh\n"
    "I1IIrCquYzu98ZXp9llNFnyD3YTld3cIyJY+0QVL8S0exqNxHF0MJu+b7zL64C2N\n"
    "kINqTlLgv1DbioLPb0jRLg5WZrdkce2vX6hdCF18gGOmCKBDj0ogRwdXJYPhUGWQ\n"
    "FvxfVHHIi0zwPzP0wJza3RPQvv9KoEw25eThr6QU/0+c6WZBiHaMrR+Z1Y4YrXWi\n"
    "YCTEFacL7gsg4Mu2mCfBcn6EFnHr1pMCtKbD8xcz/TcMXXOWB66LQ1txcV+pToUy\n"
    "qQIDAQAB\n"
    "-----END PUBLIC KEY-----\n";

static void test_caching_sha2_full_path_with_public_key(void) {
    Buf base = handshake_wire("caching_sha2_password"), wire = { 0 }, p = { 0 };
    bytes(&wire, base.p, base.n);
    const uint8_t full[] = { 1, 4 };
    packet(&wire, 2, full, sizeof(full));
    u8(&p, 1); bytes(&p, test_public_key, strlen(test_public_key));
    packet(&wire, 4, p.p, p.n);
    ok_packet(&wire, 6);
    Iron_MysqlConfig cfg = test_config();
    cfg.allow_public_key_retrieval = true;
    Iron_MysqlTestBytes written;
    Iron_MysqlError e = iron_mysql_test_handshake(wire.p, wire.n, 17, cfg, &written);
    TEST_ASSERT_EQUAL_INT64(0, e.code);
    size_t off = ((size_t)written.data[0] | ((size_t)written.data[1] << 8) |
                  ((size_t)written.data[2] << 16)) + 4;
    TEST_ASSERT_EQUAL_UINT8(1, written.data[off]);
    TEST_ASSERT_EQUAL_UINT8(3, written.data[off + 3]);
    TEST_ASSERT_EQUAL_UINT8(2, written.data[off + 4]);
    off += 5;
    size_t encrypted = (size_t)written.data[off] | ((size_t)written.data[off + 1] << 8) |
                       ((size_t)written.data[off + 2] << 16);
    TEST_ASSERT_EQUAL_size_t(256, encrypted);
    TEST_ASSERT_EQUAL_UINT8(5, written.data[off + 3]);
    err_free(&e); iron_mysql_test_bytes_free(&written); config_free(&cfg);
    free(base.p); free(wire.p); free(p.p);
}

static void test_caching_sha2_full_path_rejects_insecure_password(void) {
    Buf wire = handshake_wire("caching_sha2_password");
    const uint8_t full[] = { 1, 4 };
    packet(&wire, 2, full, sizeof(full));
    Iron_MysqlConfig cfg = test_config();
    Iron_MysqlTestBytes written;
    Iron_MysqlError e = iron_mysql_test_handshake(wire.p, wire.n, 3, cfg, &written);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_INSECURE_AUTH, e.code);
    err_free(&e); iron_mysql_test_bytes_free(&written); config_free(&cfg); free(wire.p);
}

static void test_ok_packets_and_multi_result_sets(void) {
    Buf wire = { 0 };
    const uint8_t first[] = { 0, 2, 7, 8, 0, 3, 0, 'o', 'n', 'e' };
    const uint8_t second[] = { 0, 5, 9, 2, 0, 1, 0, 't', 'w', 'o' };
    packet(&wire, 1, first, sizeof(first));
    packet(&wire, 2, second, sizeof(second));
    int64_t r = iron_mysql_test_decode_result(
        wire.p, wire.n, 1, CLIENT_PROTOCOL_41 | CLIENT_DEPRECATE_EOF, 1, false);
    Iron_MysqlError e = Iron_mysqlnative_res_error(r);
    TEST_ASSERT_EQUAL_INT64(0, e.code); err_free(&e);
    TEST_ASSERT_EQUAL_INT64(2, Iron_mysqlnative_res_info(r, 5));
    TEST_ASSERT_EQUAL_INT64(2, Iron_mysqlnative_res_info(r, 2));
    TEST_ASSERT_EQUAL_INT64(7, Iron_mysqlnative_res_info(r, 3));
    TEST_ASSERT_TRUE(Iron_mysqlnative_res_next(r));
    TEST_ASSERT_EQUAL_INT64(5, Iron_mysqlnative_res_info(r, 2));
    TEST_ASSERT_EQUAL_INT64(9, Iron_mysqlnative_res_info(r, 3));
    TEST_ASSERT_FALSE(Iron_mysqlnative_res_next(r));
    Iron_mysqlnative_res_free(r); free(wire.p);
}

static void test_prepared_parameter_encoding_all_variants(void) {
    Iron_MysqlValue v[11]; memset(v, 0, sizeof(v));
    v[0].tag = Iron_MysqlValue_TAG_Null;
    v[1].tag = Iron_MysqlValue_TAG_Int; v[1].data.Int._0 = INT64_MIN;
    v[2].tag = Iron_MysqlValue_TAG_UInt; v[2].data.UInt._0 = UINT64_MAX;
    v[3].tag = Iron_MysqlValue_TAG_Float; v[3].data.Float._0 = 1.5;
    v[4].tag = Iron_MysqlValue_TAG_Decimal; v[4].data.Decimal._0 = S("12.50");
    v[5].tag = Iron_MysqlValue_TAG_Text; v[5].data.Text._0 = S("hello");
    v[6].tag = Iron_MysqlValue_TAG_Bytes; v[6].data.Bytes._0 = iron_string_from_cstr("a\0b", 3);
    v[7].tag = Iron_MysqlValue_TAG_Bool; v[7].data.Bool._0 = true;
    v[8].tag = Iron_MysqlValue_TAG_Date; v[8].data.Date._0 = (Iron_MysqlDateTime){ 2024, 2, 29, 0, 0, 0, 0 };
    v[9].tag = Iron_MysqlValue_TAG_DateTime;
    v[9].data.DateTime._0 = (Iron_MysqlDateTime){ 2024, 2, 29, 1, 2, 3, 456789 };
    v[10].tag = Iron_MysqlValue_TAG_Time;
    v[10].data.Time._0 = (Iron_MysqlTime){ true, 123, 4, 5, 678901 };
    Iron_MysqlValueList list = { v, 11, 11 };
    Iron_MysqlTestBytes encoded;
    Iron_MysqlError e = iron_mysql_test_encode_params(&list, &encoded);
    TEST_ASSERT_EQUAL_INT64(0, e.code); err_free(&e);
    TEST_ASSERT_BITS(1, 1, encoded.data[0]);
    TEST_ASSERT_EQUAL_UINT8(0, encoded.data[1]);
    TEST_ASSERT_EQUAL_UINT8(1, encoded.data[2]);
    const uint8_t types[] = { 6, 8, 8, 5, 246, 253, 252, 1, 10, 12, 11 };
    for (size_t i = 0; i < 11; i++) TEST_ASSERT_EQUAL_UINT8(types[i], encoded.data[3 + 2 * i]);
    TEST_ASSERT_EQUAL_UINT8(0x80, encoded.data[3 + 2 * 2 + 1]);
    iron_mysql_test_bytes_free(&encoded);
    iron_string_release(&v[4].data.Decimal._0);
    iron_string_release(&v[5].data.Text._0);
    iron_string_release(&v[6].data.Bytes._0);
}

static size_t next_packet(const Iron_MysqlTestBytes *wire, size_t off,
                          uint8_t *sequence, const uint8_t **payload, size_t *length) {
    TEST_ASSERT_TRUE(off + 4 <= wire->len);
    *length = (size_t)wire->data[off] | ((size_t)wire->data[off + 1] << 8) |
              ((size_t)wire->data[off + 2] << 16);
    *sequence = wire->data[off + 3];
    TEST_ASSERT_TRUE(off + 4 + *length <= wire->len);
    *payload = wire->data + off + 4;
    return off + 4 + *length;
}

static void test_prepared_statement_prepare_execute_close_exchange(void) {
    Buf wire = { 0 }, p = { 0 };
    u8(&p, 0); u32(&p, 0x78563412); u16(&p, 1); u16(&p, 1); u8(&p, 0); u16(&p, 0);
    packet(&wire, 1, p.p, p.n);
    p.n = 0; column(&p, "param", T_LONGLONG, 0); packet(&wire, 2, p.p, p.n);
    p.n = 0; column(&p, "answer", T_LONGLONG, 0); packet(&wire, 3, p.p, p.n);
    p.n = 0; u8(&p, 1); packet(&wire, 1, p.p, p.n);
    p.n = 0; column(&p, "answer", T_LONGLONG, 0); packet(&wire, 2, p.p, p.n);
    p.n = 0; u8(&p, 0); u8(&p, 0); u64(&p, 42); packet(&wire, 3, p.p, p.n);
    const uint8_t end[] = { 0xfe, 0, 0, 2, 0, 0, 0 };
    packet(&wire, 4, end, sizeof(end));
    ok_packet(&wire, 1);
    Iron_MysqlValue param; memset(&param, 0, sizeof(param));
    param.tag = Iron_MysqlValue_TAG_Int; param.data.Int._0 = 41;
    Iron_MysqlValueList params = { &param, 1, 1 };
    Iron_MysqlTestBytes written;
    const char sql[] = "SELECT ? + 1";
    int64_t r = iron_mysql_test_statement_exchange(
        wire.p, wire.n, 1, sql, strlen(sql), &params, &written);
    Iron_MysqlError e = Iron_mysqlnative_res_error(r);
    if (e.code) TEST_FAIL_MESSAGE(iron_string_cstr(&e.message));
    err_free(&e);
    Iron_MysqlIntResult answer = Iron_mysqlnative_res_int(r, 0, 0);
    TEST_ASSERT_EQUAL_INT64(42, answer.v0);
    uint8_t seq; const uint8_t *payload; size_t n, off = 0;
    off = next_packet(&written, off, &seq, &payload, &n);
    TEST_ASSERT_EQUAL_UINT8(0, seq); TEST_ASSERT_EQUAL_UINT8(0x16, payload[0]);
    TEST_ASSERT_EQUAL_MEMORY(sql, payload + 1, strlen(sql));
    off = next_packet(&written, off, &seq, &payload, &n);
    TEST_ASSERT_EQUAL_UINT8(0, seq); TEST_ASSERT_EQUAL_UINT8(0x17, payload[0]);
    TEST_ASSERT_EQUAL_UINT8(0x12, payload[1]); TEST_ASSERT_EQUAL_UINT8(0x34, payload[2]);
    off = next_packet(&written, off, &seq, &payload, &n);
    TEST_ASSERT_EQUAL_UINT8(0, seq); TEST_ASSERT_EQUAL_UINT8(0x19, payload[0]);
    TEST_ASSERT_EQUAL_size_t(5, n);
    off = next_packet(&written, off, &seq, &payload, &n);
    TEST_ASSERT_EQUAL_UINT8(0, seq); TEST_ASSERT_EQUAL_UINT8(0x0e, payload[0]);
    TEST_ASSERT_EQUAL_size_t(written.len, off);
    iron_mysql_test_bytes_free(&written); Iron_mysqlnative_res_free(r);
    free(p.p); free(wire.p);
}

static void test_binary_temporal_lengths_are_validated_while_decoding(void) {
    const uint8_t types[] = { T_DATE, T_DATETIME, T_TIMESTAMP, T_TIME };
    const uint8_t bad_lengths[] = { 5, 8, 12, 7 };
    for (size_t k = 0; k < sizeof(types); k++) {
        Buf wire = { 0 }, p = { 0 }; uint8_t seq = 1;
        u8(&p, 1); packet(&wire, seq++, p.p, p.n);
        p.n = 0; column(&p, "temporal", types[k], 0); packet(&wire, seq++, p.p, p.n);
        p.n = 0; u8(&p, 0); u8(&p, 0); u8(&p, bad_lengths[k]);
        for (uint8_t i = 0; i < bad_lengths[k]; i++) u8(&p, 0);
        packet(&wire, seq, p.p, p.n);
        int64_t r = iron_mysql_test_decode_result(
            wire.p, wire.n, 2, CLIENT_PROTOCOL_41 | CLIENT_DEPRECATE_EOF, 1, true);
        Iron_MysqlError e = Iron_mysqlnative_res_error(r);
        TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PROTOCOL, e.code);
        err_free(&e); Iron_mysqlnative_res_free(r); free(wire.p); free(p.p);
    }
}

static void test_forbidden_null_lenenc_and_header_suffixes_are_rejected(void) {
    Buf wire = { 0 };
    const uint8_t null_ok[] = { 0, 0xfb, 0, 2, 0, 0, 0 };
    packet(&wire, 1, null_ok, sizeof(null_ok));
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PROTOCOL,
                            result_error(&wire, CLIENT_PROTOCOL_41 | CLIENT_DEPRECATE_EOF));
    free(wire.p);

    wire = (Buf){ 0 }; Buf p = { 0 };
    const uint8_t count_suffix[] = { 1, 0 };
    packet(&wire, 1, count_suffix, sizeof(count_suffix));
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PROTOCOL,
                            result_error(&wire, CLIENT_PROTOCOL_41 | CLIENT_DEPRECATE_EOF));
    free(wire.p);

    wire = (Buf){ 0 }; u8(&p, 1); packet(&wire, 1, p.p, p.n);
    p.n = 0; column(&p, "v", T_VAR_STRING, 0); p.p[0] = 0xfb;
    packet(&wire, 2, p.p, p.n);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PROTOCOL,
                            result_error(&wire, CLIENT_PROTOCOL_41 | CLIENT_DEPRECATE_EOF));
    free(wire.p); free(p.p);
}

static void test_prepare_ok_rejects_trailing_bytes(void) {
    Buf wire = { 0 }, p = { 0 };
    u8(&p, 0); u32(&p, 1); u16(&p, 0); u16(&p, 0); u8(&p, 0); u16(&p, 0); u8(&p, 0xaa);
    packet(&wire, 1, p.p, p.n);
    Iron_MysqlValueList params = { NULL, 0, 0 };
    Iron_MysqlTestBytes written;
    int64_t r = iron_mysql_test_statement_exchange(
        wire.p, wire.n, 1, "SELECT 1", 8, &params, &written);
    Iron_MysqlError e = Iron_mysqlnative_res_error(r);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_MYSQL_PROTOCOL, e.code);
    err_free(&e); Iron_mysqlnative_res_free(r); iron_mysql_test_bytes_free(&written);
    free(wire.p); free(p.p);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_packet_header_and_every_read_split);
    RUN_TEST(test_packet_continuation_and_empty_trailer);
    RUN_TEST(test_packet_rejects_bad_sequence_truncation_and_limit);
    RUN_TEST(test_length_encoded_integer_size_classes);
    RUN_TEST(test_truncated_result_is_always_an_error);
    RUN_TEST(test_column_definition_rejects_inconsistent_fixed_fields);
    RUN_TEST(test_legacy_eof_requires_warnings_and_status);
    RUN_TEST(test_err_packet_requires_error_code_and_sqlstate);
    RUN_TEST(test_binary_rows_cover_every_column_type_and_null_offsets);
    RUN_TEST(test_text_rows_cover_every_column_type);
    RUN_TEST(test_handshake_v10_and_caching_sha2_fast_path);
    RUN_TEST(test_auth_switch_to_mysql_native_password);
    RUN_TEST(test_caching_sha2_full_path_with_public_key);
    RUN_TEST(test_caching_sha2_full_path_rejects_insecure_password);
    RUN_TEST(test_ok_packets_and_multi_result_sets);
    RUN_TEST(test_prepared_parameter_encoding_all_variants);
    RUN_TEST(test_prepared_statement_prepare_execute_close_exchange);
    RUN_TEST(test_binary_temporal_lengths_are_validated_while_decoding);
    RUN_TEST(test_forbidden_null_lenenc_and_header_suffixes_are_rejected);
    RUN_TEST(test_prepare_ok_rejects_trailing_bytes);
    return UNITY_END();
}
