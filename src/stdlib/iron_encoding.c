/* iron_encoding.c: hex and base64 (RFC 4648) for src/stdlib/encoding.iron.
 *
 * No dependencies beyond the runtime: every program can use these, whether
 * or not it links OpenSSL. Output is written straight into the result
 * string (iron_string_alloc_bytes), so each call allocates once. */

#include "iron_encoding.h"
#include "runtime/iron_errors.h"

#include <stdio.h>
#include <string.h>

static Iron_String enc_cstr(const char *text) {
    return iron_string_from_cstr(text, strlen(text));
}

static Iron_DecodeResult enc_ok(Iron_String data) {
    Iron_DecodeResult out;
    out.data = data;
    out.error = 0;
    out.error_message = iron_string_from_cstr("", 0);
    return out;
}

static Iron_DecodeResult enc_fail(int64_t code, const char *what, size_t at) {
    char message[160];
    switch (code) {
    case IRON_ERR_ENCODING_INVALID_CHARACTER:
        snprintf(message, sizeof(message), "invalid %s character at byte %zu", what, at);
        break;
    case IRON_ERR_ENCODING_INVALID_LENGTH:
        snprintf(message, sizeof(message), "invalid %s length", what);
        break;
    default:
        snprintf(message, sizeof(message), "invalid %s padding at byte %zu", what, at);
        break;
    }
    Iron_DecodeResult out;
    out.data = iron_string_from_cstr("", 0);
    out.error = code;
    out.error_message = enc_cstr(message);
    return out;
}

void Iron_decoderesult_release(Iron_DecodeResult result) {
    /* Strings are released by the value's own drop glue; kept so the
     * module matches the other result types. */
    (void)result;
}

/* ── Hex ──────────────────────────────────────────────────────────────── */

Iron_String Iron_hex_encode(Iron_String data) {
    static const char digits[] = "0123456789abcdef";
    const uint8_t *in = (const uint8_t *)iron_string_cstr(&data);
    size_t len = iron_string_byte_len(&data);
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, len * 2, &p);
    for (size_t i = 0; i < len; i++) {
        p[2 * i]     = digits[in[i] >> 4];
        p[2 * i + 1] = digits[in[i] & 15];
    }
    iron_string_seal_bytes(&out, len * 2);
    return out;
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Iron_DecodeResult Iron_hex_decode(Iron_String text) {
    const unsigned char *in = (const unsigned char *)iron_string_cstr(&text);
    size_t len = iron_string_byte_len(&text);
    /* Report a bad character before an odd length: "abz" names the z. */
    for (size_t i = 0; i < len; i++) {
        if (hex_value(in[i]) < 0) return enc_fail(IRON_ERR_ENCODING_INVALID_CHARACTER, "hex", i);
    }
    if (len % 2 != 0) return enc_fail(IRON_ERR_ENCODING_INVALID_LENGTH, "hex", len);
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, len / 2, &p);
    for (size_t i = 0; i < len / 2; i++) {
        p[i] = (char)((hex_value(in[2 * i]) << 4) | hex_value(in[2 * i + 1]));
    }
    iron_string_seal_bytes(&out, len / 2);
    return enc_ok(out);
}

/* ── Base64 ───────────────────────────────────────────────────────────── */

static const char b64_std[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char b64_url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t iron_base64_encoded_len(size_t len, bool pad) {
    if (pad) return (len + 2) / 3 * 4;
    return len / 3 * 4 + (len % 3 == 0 ? 0 : len % 3 + 1);
}

void iron_base64_encode_raw(const uint8_t *in, size_t len, char *out, bool url, bool pad) {
    const char *alpha = url ? b64_url : b64_std;
    size_t i = 0, o = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = alpha[(v >> 18) & 63];
        out[o++] = alpha[(v >> 12) & 63];
        out[o++] = alpha[(v >> 6) & 63];
        out[o++] = alpha[v & 63];
    }
    size_t rest = len - i;
    if (rest == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = alpha[(v >> 18) & 63];
        out[o++] = alpha[(v >> 12) & 63];
        if (pad) { out[o++] = '='; out[o++] = '='; }
    } else if (rest == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = alpha[(v >> 18) & 63];
        out[o++] = alpha[(v >> 12) & 63];
        out[o++] = alpha[(v >> 6) & 63];
        if (pad) out[o++] = '=';
    }
}

static int b64_value(unsigned char c, bool url) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (url) {
        if (c == '-') return 62;
        if (c == '_') return 63;
    } else {
        if (c == '+') return 62;
        if (c == '/') return 63;
    }
    return -1;
}

/* Strict RFC 4648 decoding: only the alphabet, '=' only as trailing
 * padding, no whitespace. Like Python's and Go's default decoders, nonzero
 * bits in the final character are ignored. `out` needs len / 4 * 3 + 3. */
int64_t iron_base64_decode_raw(const char *in_chars, size_t len, bool url, bool require_pad,
                               uint8_t *out, size_t *out_len, size_t *bad_at) {
    const unsigned char *in = (const unsigned char *)in_chars;
    size_t data_len = len;
    size_t pad = 0;
    while (data_len > 0 && in[data_len - 1] == '=' && pad < 2) {
        data_len--;
        pad++;
    }
    for (size_t i = 0; i < data_len; i++) {
        if (b64_value(in[i], url) < 0) {
            *bad_at = i;
            return in[i] == '=' ? IRON_ERR_ENCODING_INVALID_PADDING
                                : IRON_ERR_ENCODING_INVALID_CHARACTER;
        }
    }
    size_t rem = data_len % 4;
    if (rem == 1) {
        *bad_at = data_len;
        return IRON_ERR_ENCODING_INVALID_LENGTH;
    }
    if (pad > 0 || require_pad) {
        /* Padding, when present (or required), must complete the last
         * quantum exactly: 2 chars + "==" or 3 chars + "=". */
        size_t want = rem == 0 ? 0 : 4 - rem;
        if (pad != want) {
            *bad_at = data_len;
            return IRON_ERR_ENCODING_INVALID_PADDING;
        }
    }
    size_t o = 0, i = 0;
    for (; i + 4 <= data_len; i += 4) {
        uint32_t v = ((uint32_t)b64_value(in[i], url) << 18) |
                     ((uint32_t)b64_value(in[i + 1], url) << 12) |
                     ((uint32_t)b64_value(in[i + 2], url) << 6) |
                     (uint32_t)b64_value(in[i + 3], url);
        out[o++] = (uint8_t)(v >> 16);
        out[o++] = (uint8_t)(v >> 8);
        out[o++] = (uint8_t)v;
    }
    if (rem == 2) {
        uint32_t v = ((uint32_t)b64_value(in[i], url) << 18) |
                     ((uint32_t)b64_value(in[i + 1], url) << 12);
        out[o++] = (uint8_t)(v >> 16);
    } else if (rem == 3) {
        uint32_t v = ((uint32_t)b64_value(in[i], url) << 18) |
                     ((uint32_t)b64_value(in[i + 1], url) << 12) |
                     ((uint32_t)b64_value(in[i + 2], url) << 6);
        out[o++] = (uint8_t)(v >> 16);
        out[o++] = (uint8_t)(v >> 8);
    }
    *out_len = o;
    return 0;
}

static Iron_String b64_encode(Iron_String data, bool url, bool pad) {
    const uint8_t *in = (const uint8_t *)iron_string_cstr(&data);
    size_t len = iron_string_byte_len(&data);
    size_t n = iron_base64_encoded_len(len, pad);
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, n, &p);
    iron_base64_encode_raw(in, len, p, url, pad);
    iron_string_seal_bytes(&out, n);
    return out;
}

static Iron_DecodeResult b64_decode(Iron_String text, bool url, bool require_pad) {
    const char *in = iron_string_cstr(&text);
    size_t len = iron_string_byte_len(&text);
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, len / 4 * 3 + 3, &p);
    size_t n = 0, bad = 0;
    int64_t code = iron_base64_decode_raw(in, len, url, require_pad, (uint8_t *)p, &n, &bad);
    if (code != 0) {
        iron_string_release(&out);
        return enc_fail(code, url ? "base64url" : "base64", bad);
    }
    iron_string_seal_bytes(&out, n);
    return enc_ok(out);
}

Iron_String Iron_base64_encode(Iron_String data) { return b64_encode(data, false, true); }
Iron_DecodeResult Iron_base64_decode(Iron_String text) { return b64_decode(text, false, true); }
Iron_String Iron_base64_url_encode(Iron_String data) { return b64_encode(data, true, false); }
Iron_DecodeResult Iron_base64_url_decode(Iron_String text) { return b64_decode(text, true, false); }
