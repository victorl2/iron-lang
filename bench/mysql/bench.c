/* In-memory baseline benchmark for the production MySQL protocol codec. */
#include "stdlib/iron_mysql.h"
#include "runtime/iron_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CAPS (0x00000200u | 0x01000000u)

typedef struct Buf { uint8_t *p; size_t n, cap; } Buf;
typedef struct Fixture { const char *name; Buf wire; size_t rows; int binary; } Fixture;

static void reserve(Buf *b, size_t n) {
    if (b->n + n <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + n) cap *= 2;
    b->p = (uint8_t *)realloc(b->p, cap);
    if (!b->p) { fputs("out of memory\n", stderr); exit(2); }
    b->cap = cap;
}
static void put(Buf *b, const void *p, size_t n) { reserve(b, n); memcpy(b->p + b->n, p, n); b->n += n; }
static void u8(Buf *b, uint8_t v) { put(b, &v, 1); }
static void u16(Buf *b, uint16_t v) { uint8_t p[2] = { v, v >> 8 }; put(b, p, 2); }
static void u32(Buf *b, uint32_t v) { uint8_t p[4] = { v, v >> 8, v >> 16, v >> 24 }; put(b, p, 4); }
static void u64(Buf *b, uint64_t v) { u32(b, (uint32_t)v); u32(b, (uint32_t)(v >> 32)); }
static void lenenc(Buf *b, size_t n) {
    if (n < 251) u8(b, (uint8_t)n);
    else if (n < 65536) { u8(b, 0xfc); u16(b, (uint16_t)n); }
    else { u8(b, 0xfd); u8(b, n); u8(b, n >> 8); u8(b, n >> 16); }
}
static void lenstr(Buf *b, const void *p, size_t n) { lenenc(b, n); put(b, p, n); }
static void packet(Buf *b, uint8_t seq, const void *p, size_t n) {
    uint8_t h[4] = { n, n >> 8, n >> 16, seq }; put(b, h, 4); put(b, p, n);
}
static void column(Buf *p, const char *name, uint8_t type, uint16_t flags, uint8_t decimals) {
    lenstr(p, "def", 3); lenstr(p, "bench", 5); lenstr(p, "t", 1); lenstr(p, "t", 1);
    lenstr(p, name, strlen(name)); lenstr(p, name, strlen(name)); u8(p, 0x0c);
    u16(p, type >= 249 && type <= 252 ? 63 : 45); u32(p, 0xffffffffu);
    u8(p, type); u16(p, flags); u8(p, decimals); u16(p, 0);
}
static void finish(Buf *wire, uint8_t seq) {
    const uint8_t end[] = { 0xfe, 0, 0, 2, 0, 0, 0 }; packet(wire, seq, end, sizeof(end));
}

static Fixture narrow_fixture(void) {
    Fixture f = { "narrow_integer", { 0 }, 20000, 0 }; Buf p = { 0 }; uint8_t seq = 1;
    u8(&p, 4); packet(&f.wire, seq++, p.p, p.n);
    const uint8_t types[] = { 1, 2, 3, 8 };
    for (int i = 0; i < 4; i++) { char n[8]; snprintf(n, sizeof(n), "c%d", i); p.n = 0; column(&p, n, types[i], 0, 0); packet(&f.wire, seq++, p.p, p.n); }
    for (size_t r = 0; r < f.rows; r++) {
        char value[32]; p.n = 0;
        for (int c = 0; c < 4; c++) { int n = snprintf(value, sizeof(value), "%lld", (long long)(r * 4 + c)); lenstr(&p, value, (size_t)n); }
        packet(&f.wire, seq++, p.p, p.n);
    }
    finish(&f.wire, seq); free(p.p); return f;
}

static Fixture wide_fixture(void) {
    Fixture f = { "wide_mixed", { 0 }, 4000, 0 }; Buf p = { 0 }; uint8_t seq = 1;
    const uint8_t types[] = { 8, 246, 5, 10, 11, 12, 13, 16, 245, 247, 248, 253 };
    u8(&p, 12); packet(&f.wire, seq++, p.p, p.n);
    for (int i = 0; i < 12; i++) { char n[8]; snprintf(n, sizeof(n), "c%d", i); p.n = 0; column(&p, n, types[i], 0, i == 4 || i == 5 ? 6 : 0); packet(&f.wire, seq++, p.p, p.n); }
    static const char *v[] = { "9223372036854775807", "123456789.125000", "3.141592653589793", "2024-02-29", "-123:45:06.789000", "2024-02-29 13:45:30.123456", "2155", "165", "{\"active\":true,\"items\":[1,2,3]}", "green", "x,y", "the quick brown fox jumps over the lazy dog" };
    for (size_t r = 0; r < f.rows; r++) { p.n = 0; for (int c = 0; c < 12; c++) lenstr(&p, v[c], strlen(v[c])); packet(&f.wire, seq++, p.p, p.n); }
    finish(&f.wire, seq); free(p.p); return f;
}

static Fixture large_fixture(void) {
    Fixture f = { "large_text_blob", { 0 }, 64, 0 }; Buf p = { 0 }; uint8_t seq = 1;
    u8(&p, 2); packet(&f.wire, seq++, p.p, p.n);
    p.n = 0; column(&p, "text", 253, 0, 0); packet(&f.wire, seq++, p.p, p.n);
    p.n = 0; column(&p, "blob", 251, 0, 0); packet(&f.wire, seq++, p.p, p.n);
    size_t size = 64 * 1024; uint8_t *data = (uint8_t *)malloc(size);
    if (!data) exit(2); for (size_t i = 0; i < size; i++) data[i] = (uint8_t)(i * 17u);
    for (size_t r = 0; r < f.rows; r++) { p.n = 0; lenstr(&p, data, size); lenstr(&p, data, size); packet(&f.wire, seq++, p.p, p.n); }
    finish(&f.wire, seq); free(data); free(p.p); return f;
}

static Fixture binary_fixture(void) {
    Fixture f = { "binary_integer", { 0 }, 20000, 1 }; Buf p = { 0 }; uint8_t seq = 1;
    u8(&p, 4); packet(&f.wire, seq++, p.p, p.n);
    for (int i = 0; i < 4; i++) { char n[8]; snprintf(n, sizeof(n), "c%d", i); p.n = 0; column(&p, n, 8, i & 1 ? 32 : 0, 0); packet(&f.wire, seq++, p.p, p.n); }
    for (size_t r = 0; r < f.rows; r++) { p.n = 0; u8(&p, 0); u8(&p, 0); for (int c = 0; c < 4; c++) u64(&p, r * 4 + (size_t)c); packet(&f.wire, seq++, p.p, p.n); }
    finish(&f.wire, seq); free(p.p); return f;
}

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x > y ? 1 : x < y ? -1 : 0; }
static void stats(uint64_t *v, int n, uint64_t *median, uint64_t *mad) {
    qsort(v, (size_t)n, sizeof(*v), cmp_u64); *median = v[n / 2];
    uint64_t *d = (uint64_t *)malloc((size_t)n * sizeof(*d));
    for (int i = 0; i < n; i++) d[i] = v[i] > *median ? v[i] - *median : *median - v[i];
    qsort(d, (size_t)n, sizeof(*d), cmp_u64); *mad = d[n / 2]; free(d);
}
static void decode_once(const Fixture *f) {
    int64_t r = iron_mysql_test_decode_result(f->wire.p, f->wire.n, f->wire.n, CAPS, 1, f->binary != 0);
    Iron_MysqlError e = Iron_mysqlnative_res_error(r);
    if (e.code || (size_t)Iron_mysqlnative_res_info(r, 0) != f->rows) { fprintf(stderr, "decode failed for %s: %lld\n", f->name, (long long)e.code); exit(2); }
    iron_string_release(&e.sqlstate); iron_string_release(&e.message); Iron_mysqlnative_res_free(r);
}
static void bench_decode(const Fixture *f, int warmup, int samples) {
    for (int i = 0; i < warmup; i++) decode_once(f);
    uint64_t *times = (uint64_t *)malloc((size_t)samples * sizeof(*times));
    for (int i = 0; i < samples; i++) { uint64_t a = now_ns(); decode_once(f); times[i] = now_ns() - a; }
    uint64_t med, mad; stats(times, samples, &med, &mad); free(times);
    printf("RESULT decode %s %zu %zu %llu %llu\n", f->name, f->wire.n, f->rows, (unsigned long long)med, (unsigned long long)mad);
}
static void release_values(Iron_MysqlValue *v, size_t n) { for (size_t i = 0; i < n; i++) if (v[i].tag == Iron_MysqlValue_TAG_Text) iron_string_release(&v[i].data.Text._0); }
static void bench_encode(int warmup, int samples) {
    enum { COUNT = 256, INNER = 200 };
    Iron_MysqlValue v[COUNT]; memset(v, 0, sizeof(v));
    for (int i = 0; i < COUNT; i++) { if (i % 4 == 0) { v[i].tag = Iron_MysqlValue_TAG_Int; v[i].data.Int._0 = i; } else if (i % 4 == 1) { v[i].tag = Iron_MysqlValue_TAG_Float; v[i].data.Float._0 = i + 0.5; } else if (i % 4 == 2) { v[i].tag = Iron_MysqlValue_TAG_Text; v[i].data.Text._0 = iron_string_from_cstr("prepared parameter payload", 26); } else v[i].tag = Iron_MysqlValue_TAG_Null; }
    Iron_MysqlValueList list = { v, COUNT, COUNT }; Iron_MysqlTestBytes out; Iron_MysqlError e = iron_mysql_test_encode_params(&list, &out);
    if (e.code) exit(2); size_t bytes_per = out.len; iron_mysql_test_bytes_free(&out);
    for (int w = 0; w < warmup; w++) for (int j = 0; j < INNER; j++) { e = iron_mysql_test_encode_params(&list, &out); if (e.code) exit(2); iron_mysql_test_bytes_free(&out); }
    uint64_t *times = (uint64_t *)malloc((size_t)samples * sizeof(*times));
    for (int i = 0; i < samples; i++) { uint64_t a = now_ns(); for (int j = 0; j < INNER; j++) { e = iron_mysql_test_encode_params(&list, &out); if (e.code) exit(2); iron_mysql_test_bytes_free(&out); } times[i] = now_ns() - a; }
    uint64_t med, mad; stats(times, samples, &med, &mad); free(times);
    printf("RESULT encode prepared_params %zu %d %llu %llu\n", bytes_per * INNER, COUNT * INNER, (unsigned long long)med, (unsigned long long)mad);
    release_values(v, COUNT);
}

int main(int argc, char **argv) {
    int warmup = argc > 1 ? atoi(argv[1]) : 3, samples = argc > 2 ? atoi(argv[2]) : 15;
    if (warmup < 0 || samples < 3) return 2;
    iron_runtime_init(0, NULL);
    Fixture fixtures[] = { narrow_fixture(), wide_fixture(), large_fixture(), binary_fixture() };
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) bench_decode(&fixtures[i], warmup, samples);
    bench_encode(warmup, samples);
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) free(fixtures[i].wire.p);
    iron_runtime_shutdown();
    return 0;
}
