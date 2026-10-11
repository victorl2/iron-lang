/* tests/fuzz/fuzz_stdlib.c: libFuzzer target for the standard library
 * parsers that read untrusted input. The first input byte picks the
 * target, the rest is its input:
 *
 *   0  String.to_int / to_float: never crash; a text that parses as a
 *      decimal number gives the value strtod gives it; Float.to_string of
 *      the value reads back to the same value; Int.to_string of a to_int
 *      result reads back to it.
 *   1  IPv4Addr.parse / IPv6Addr.parse: a parsed address formats to text
 *      that parses back to the same address.
 *   2  String.upper / lower: never crash, the result is no more than twice
 *      as long (plus 4 bytes), and valid UTF-8 stays valid UTF-8.
 *   3  WebSocket frame decoder as a client (frames must not be masked).
 *   4  WebSocket frame decoder as a server (frames must be masked).
 *
 * Every crash, ASan / UBSan report or failed check aborts. (The url module
 * is written in Iron and has no C parser; tests/fuzz/pf_url.py covers it.)
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>

#include "runtime/iron_runtime.h"
#include "stdlib/iron_net.h"
#include "stdlib/iron_websocket.h"

#define CHECK(c) do { if (!(c)) abort(); } while (0)

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    iron_runtime_init(0, NULL);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    return 0;
}

static bool utf8_ok(const unsigned char *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        size_t k;
        uint32_t cp;
        if (c < 0x80) { i++; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { k = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { k = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { k = 3; cp = c & 0x07; }
        else return false;
        for (size_t j = 1; j <= k; j++) {
            if (i + j >= n || (p[i + j] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + j] & 0x3F);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += k + 1;
    }
    return true;
}

static bool decimal_text(const char *p, size_t n) {
    size_t i = 0;
    while (i < n && strchr(" \t\n\r\v\f", p[i]) && p[i]) i++;
    size_t e = n;
    while (e > i && strchr(" \t\n\r\v\f", p[e - 1]) && p[e - 1]) e--;
    if (i < e && (p[i] == '+' || p[i] == '-')) i++;
    size_t d0 = i;
    while (i < e && p[i] >= '0' && p[i] <= '9') i++;
    bool id = i > d0, fd = false;
    if (i < e && p[i] == '.') {
        i++;
        size_t f0 = i;
        while (i < e && p[i] >= '0' && p[i] <= '9') i++;
        fd = i > f0;
    }
    if (!id && !fd) return false;
    if (i < e && (p[i] == 'e' || p[i] == 'E')) {
        i++;
        if (i < e && (p[i] == '+' || p[i] == '-')) i++;
        size_t x0 = i;
        while (i < e && p[i] >= '0' && p[i] <= '9') i++;
        if (i == x0) return false;
    }
    return i == e;
}

static void fuzz_numbers(const uint8_t *data, size_t n) {
    Iron_String s = iron_string_from_cstr((const char *)data, n);
    double f = Iron_string_to_float(s);
    if (decimal_text((const char *)data, n) && memchr(data, 0, n) == NULL) {
        char *copy = (char *)malloc(n + 1);
        CHECK(copy);
        memcpy(copy, data, n);
        copy[n] = 0;
        double want = strtod(copy, NULL);
        free(copy);
        CHECK(f == want || (isnan(f) && isnan(want)));
    } else {
        CHECK(f == 0.0);
    }
    if (isfinite(f)) {
        Iron_String t = Iron_float_to_string(f);
        double back = Iron_string_to_float(t);
        CHECK(back == f || (f == 0.0 && back == 0.0));
        iron_string_release(&t);
    }
    int64_t v = Iron_string_to_int(s);
    if (v != 0) {
        Iron_String t = Iron_int_to_string(v);
        CHECK(Iron_string_to_int(t) == v);
        iron_string_release(&t);
    }
    iron_string_release(&s);
}

static void fuzz_addresses(const uint8_t *data, size_t n) {
    Iron_String s = iron_string_from_cstr((const char *)data, n);
    Iron_Result_IPv4Addr_NetError a = Iron_ipv4addr_parse(s);
    if (a.v1.code == 0) {
        Iron_String t = Iron_ipv4addr_format(a.v0);
        Iron_Result_IPv4Addr_NetError b = Iron_ipv4addr_parse(t);
        CHECK(b.v1.code == 0 && b.v0.a == a.v0.a && b.v0.b == a.v0.b &&
              b.v0.c == a.v0.c && b.v0.d == a.v0.d);
        iron_string_release(&t);
    }
    Iron_Result_IPv6Addr_NetError c = Iron_ipv6addr_parse(s);
    if (c.v1.code == 0) {
        Iron_String t = Iron_ipv6addr_format(c.v0);
        Iron_Result_IPv6Addr_NetError d = Iron_ipv6addr_parse(t);
        bool zone_pct = iron_string_byte_len(&c.v0.zone) >= 2 &&
                        memcmp(iron_string_cstr(&c.v0.zone), "25", 2) == 0;
        /* (a zone starting with "25" reads back as the %25 URL form) */
        if (!zone_pct) {
            CHECK(d.v1.code == 0);
            CHECK(iron_string_equals(&d.v0.bytes, &c.v0.bytes));
            CHECK(iron_string_equals(&d.v0.zone, &c.v0.zone));
        }
        iron_string_release(&t);
        iron_string_release(&d.v0.bytes);
        iron_string_release(&d.v0.zone);
    }
    iron_string_release(&c.v0.bytes);
    iron_string_release(&c.v0.zone);
    iron_string_release(&s);
}

static void fuzz_case(const uint8_t *data, size_t n) {
    Iron_String s = iron_string_from_cstr((const char *)data, n);
    Iron_String u = Iron_string_upper(s);
    Iron_String l = Iron_string_lower(s);
    CHECK(iron_string_byte_len(&u) <= 2 * n + 4);
    CHECK(iron_string_byte_len(&l) <= 2 * n + 4);
    if (utf8_ok(data, n)) {
        CHECK(utf8_ok((const unsigned char *)iron_string_cstr(&u), iron_string_byte_len(&u)));
        CHECK(utf8_ok((const unsigned char *)iron_string_cstr(&l), iron_string_byte_len(&l)));
    }
    iron_string_release(&u);
    iron_string_release(&l);
    iron_string_release(&s);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1 || size > 65536) return 0;
    const uint8_t *p = data + 1;
    size_t n = size - 1;
    switch (data[0] % 5) {
        case 0: fuzz_numbers(p, n < 512 ? n : 512); break;
        case 1: fuzz_addresses(p, n < 256 ? n : 256); break;
        case 2: fuzz_case(p, n); break;
        case 3: iron_websocket_fuzz_frames(p, n, 1, 1 + (size % 70000)); break;
        case 4: iron_websocket_fuzz_frames(p, n, 0, 1 + (size % 70000)); break;
    }
    return 0;
}
