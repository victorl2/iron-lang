/* Unit tests for src/stdlib/iron_crypto.c and iron_encoding.c through their
 * C entry points, so the sanitizer jobs (ASan, LeakSanitizer) see every
 * allocation and release path: results, errors, forged messages, the
 * streaming hasher and the in-place string helpers. The Iron-level known
 * answers live in tests/integration/v4/7.5-stdlib/crypto_*.iron. */
#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_errors.h"
#include "stdlib/iron_crypto.h"
#include "stdlib/iron_encoding.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

static Iron_String bytes(const void *p, size_t n) { return iron_string_from_cstr((const char *)p, n); }
static Iron_String text(const char *s) { return bytes(s, strlen(s)); }
static Iron_String repeat(const char *s, int64_t n) {
    Iron_String unit = text(s);
    Iron_String out = Iron_string_repeat(unit, n);
    iron_string_release(&unit);
    return out;
}

/* Hex of a string, released by the caller with free(). */
static char *hex_of(Iron_String s) {
    Iron_String h = Iron_hex_encode(s);
    size_t n = iron_string_byte_len(&h);
    char *out = (char *)malloc(n + 1);
    memcpy(out, iron_string_cstr(&h), n);
    out[n] = 0;
    iron_string_release(&h);
    return out;
}

static void assert_hex(const char *want, Iron_String got) {
    char *h = hex_of(got);
    TEST_ASSERT_EQUAL_STRING(want, h);
    free(h);
}

static void release_result(Iron_CryptoResult *r) {
    iron_string_release(&r->data);
    iron_string_release(&r->error_message);
}

static void release_decode(Iron_DecodeResult *r) {
    iron_string_release(&r->data);
    iron_string_release(&r->error_message);
}

static void test_alloc_and_seal_bytes(void) {
    /* inline */
    Iron_String s;
    char *p;
    iron_string_alloc_bytes(&s, 5, &p);
    memcpy(p, "hello", 5);
    iron_string_seal_bytes(&s, 5);
    TEST_ASSERT_EQUAL_UINT64(5, iron_string_byte_len(&s));
    TEST_ASSERT_EQUAL_STRING("hello", iron_string_cstr(&s));
    iron_string_release(&s);
    /* heap, kept */
    iron_string_alloc_bytes(&s, 100, &p);
    memset(p, 'x', 100);
    iron_string_seal_bytes(&s, 100);
    TEST_ASSERT_EQUAL_UINT64(100, iron_string_byte_len(&s));
    TEST_ASSERT_EQUAL_UINT64(100, iron_string_codepoint_count(&s));
    TEST_ASSERT_EQUAL_CHAR(0, iron_string_cstr(&s)[100]);
    iron_string_release(&s);
    /* heap, shrunk to the inline size (moves inline, frees the block) */
    iron_string_alloc_bytes(&s, 100, &p);
    memcpy(p, "short", 5);
    iron_string_seal_bytes(&s, 5);
    TEST_ASSERT_EQUAL_UINT64(5, iron_string_byte_len(&s));
    TEST_ASSERT_EQUAL_STRING("short", iron_string_cstr(&s));
    iron_string_release(&s);
    /* heap, shrunk but still heap */
    iron_string_alloc_bytes(&s, 100, &p);
    memset(p, 'y', 100);
    iron_string_seal_bytes(&s, 40);
    TEST_ASSERT_EQUAL_UINT64(40, iron_string_byte_len(&s));
    TEST_ASSERT_EQUAL_CHAR(0, iron_string_cstr(&s)[40]);
    iron_string_release(&s);
    /* byte_slice clamps */
    Iron_String src = text("abcdef");
    Iron_String sl = Iron_string_byte_slice(src, 2, 99);
    TEST_ASSERT_EQUAL_STRING("cdef", iron_string_cstr(&sl));
    iron_string_release(&sl);
    sl = Iron_string_byte_slice(src, 4, 1);
    TEST_ASSERT_EQUAL_UINT64(0, iron_string_byte_len(&sl));
    iron_string_release(&sl);
    iron_string_release(&src);
}

static void test_encoding(void) {
    Iron_String in = bytes("\x00\xff\x10", 3);
    Iron_String h = Iron_hex_encode(in);
    TEST_ASSERT_EQUAL_STRING("00ff10", iron_string_cstr(&h));
    Iron_DecodeResult d = Iron_hex_decode(h);
    TEST_ASSERT_EQUAL_INT64(0, d.error);
    TEST_ASSERT_TRUE(iron_string_equals(&d.data, &in));
    release_decode(&d);
    iron_string_release(&h);

    Iron_String b = Iron_base64_encode(in);
    TEST_ASSERT_EQUAL_STRING("AP8Q", iron_string_cstr(&b));
    iron_string_release(&b);
    Iron_String big = repeat("\xfb\xff", 100);
    Iron_String bu = Iron_base64_url_encode(big);
    d = Iron_base64_url_decode(bu);
    TEST_ASSERT_EQUAL_INT64(0, d.error);
    TEST_ASSERT_TRUE(iron_string_equals(&d.data, &big));
    release_decode(&d);
    iron_string_release(&bu);
    iron_string_release(&big);
    iron_string_release(&in);

    Iron_String bad = text("ab!d");
    d = Iron_base64_decode(bad);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_ENCODING_INVALID_CHARACTER, d.error);
    TEST_ASSERT_EQUAL_STRING("invalid base64 character at byte 2", iron_string_cstr(&d.error_message));
    release_decode(&d);
    d = Iron_hex_decode(bad);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_ENCODING_INVALID_CHARACTER, d.error);
    release_decode(&d);
    iron_string_release(&bad);
    /* a long invalid input: the output buffer is released on the error path */
    Iron_String long_bad = repeat("QUJD", 500);
    Iron_String tail = text("Q=Q=");
    Iron_String joined = iron_string_concat(&long_bad, &tail);
    d = Iron_base64_decode(joined);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_ENCODING_INVALID_PADDING, d.error);
    release_decode(&d);
    iron_string_release(&joined);
    iron_string_release(&tail);
    iron_string_release(&long_bad);
}

#ifdef IRON_HAVE_OPENSSL

static void test_digests(void) {
    Iron_String abc = text("abc");
    Iron_String d = Iron_crypto__digest(3, abc);
    assert_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", d);
    iron_string_release(&d);
    d = Iron_crypto__digest(7, abc);
    assert_hex("b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
               "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0", d);
    iron_string_release(&d);
    Iron_String key = text("key");
    Iron_String fox = text("The quick brown fox jumps over the lazy dog");
    d = Iron_crypto__hmac(3, key, fox);
    assert_hex("f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", d);
    iron_string_release(&d);

    int64_t h = Iron_crypto__hasher_new(3);
    Iron_String a = text("a"), bc = text("bc");
    Iron_crypto__hasher_update(h, a);
    Iron_crypto__hasher_update(h, bc);
    d = Iron_crypto__hasher_finish(h);
    assert_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", d);
    iron_string_release(&d);
    Iron_crypto__hasher_update(h, abc);
    d = Iron_crypto__hasher_finish(h);
    assert_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", d);
    iron_string_release(&d);
    Iron_crypto__hasher_free(h);
    Iron_crypto__hasher_free(0);

    h = Iron_crypto__hmac_new(3, key);
    Iron_crypto__hasher_update(h, fox);
    d = Iron_crypto__hasher_finish(h);
    assert_hex("f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", d);
    iron_string_release(&d);
    Iron_crypto__hasher_update(h, fox);
    d = Iron_crypto__hasher_finish(h);
    assert_hex("f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", d);
    iron_string_release(&d);
    Iron_crypto__hasher_free(h);
    iron_string_release(&a);
    iron_string_release(&bc);
    iron_string_release(&key);
    iron_string_release(&fox);
    iron_string_release(&abc);
}

static void test_kdfs(void) {
    Iron_String pw = text("password"), salt = text("salt"), empty = text("");
    Iron_CryptoResult r = Iron_crypto__pbkdf2(1, pw, salt, 2, 20);
    TEST_ASSERT_EQUAL_INT64(0, r.error);
    assert_hex("ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957", r.data);
    release_result(&r);
    r = Iron_crypto__pbkdf2(1, pw, salt, 0, 20);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_CRYPTO_INVALID_PARAMETERS, r.error);
    release_result(&r);
    r = Iron_crypto__hkdf(3, pw, empty, empty, 42);
    TEST_ASSERT_EQUAL_INT64(0, r.error);
    TEST_ASSERT_EQUAL_UINT64(42, iron_string_byte_len(&r.data));
    release_result(&r);
    r = Iron_crypto__hkdf(3, pw, salt, empty, 255 * 32 + 1);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_CRYPTO_INVALID_LENGTH, r.error);
    release_result(&r);
    r = Iron_crypto_scrypt(empty, empty, 4, 1, 1, 64);
    TEST_ASSERT_EQUAL_INT64(0, r.error);
    assert_hex("77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442"
               "fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906", r.data);
    release_result(&r);
    r = Iron_crypto_scrypt(pw, salt, 25, 8, 1, 32);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_CRYPTO_INVALID_PARAMETERS, r.error);
    release_result(&r);

    r = Iron_crypto__hash_password_scrypt(pw, 4, 8, 1);
    TEST_ASSERT_EQUAL_INT64(0, r.error);
    TEST_ASSERT_TRUE(Iron_crypto_verify_password(pw, r.data));
    TEST_ASSERT_FALSE(Iron_crypto_verify_password(salt, r.data));
    release_result(&r);
    r = Iron_crypto__hash_password_pbkdf2(pw, 3, 10);
    TEST_ASSERT_EQUAL_INT64(0, r.error);
    TEST_ASSERT_TRUE(Iron_crypto_verify_password(pw, r.data));
    release_result(&r);
    Iron_String junk = text("$scrypt$ln=4,r=8,p=1$$");
    TEST_ASSERT_FALSE(Iron_crypto_verify_password(pw, junk));
    iron_string_release(&junk);
    iron_string_release(&pw);
    iron_string_release(&salt);
    iron_string_release(&empty);
}

static void test_aead(void) {
    for (int64_t alg = 1; alg <= 2; alg++) {
        Iron_String key = Iron_crypto__random_bytes(32);
        Iron_String nonce = Iron_crypto__random_bytes(12);
        Iron_String pt = repeat("plaintext ", 50);
        Iron_String aad = text("aad");
        Iron_CryptoResult s = Iron_crypto__seal(alg, key, nonce, pt, aad);
        TEST_ASSERT_EQUAL_INT64(0, s.error);
        TEST_ASSERT_EQUAL_UINT64(iron_string_byte_len(&pt) + 16, iron_string_byte_len(&s.data));
        Iron_CryptoResult o = Iron_crypto__open(alg, key, nonce, s.data, aad);
        TEST_ASSERT_EQUAL_INT64(0, o.error);
        TEST_ASSERT_TRUE(iron_string_equals(&o.data, &pt));
        release_result(&o);
        /* forged: flip the last tag byte */
        Iron_String forged = iron_string_from_cstr(iron_string_cstr(&s.data), iron_string_byte_len(&s.data));
        ((char *)iron_string_cstr(&forged))[iron_string_byte_len(&forged) - 1] ^= 1;
        o = Iron_crypto__open(alg, key, nonce, forged, aad);
        TEST_ASSERT_EQUAL_INT64(IRON_ERR_CRYPTO_AUTHENTICATION, o.error);
        TEST_ASSERT_EQUAL_UINT64(0, iron_string_byte_len(&o.data));
        release_result(&o);
        iron_string_release(&forged);
        release_result(&s);

        Iron_CryptoResult e = Iron_crypto__encrypt(alg, key, pt, aad);
        TEST_ASSERT_EQUAL_INT64(0, e.error);
        Iron_CryptoResult dr = Iron_crypto__decrypt(alg, key, e.data, aad);
        TEST_ASSERT_TRUE(iron_string_equals(&dr.data, &pt));
        release_result(&dr);
        dr = Iron_crypto__decrypt(alg, key, e.data, pt);
        TEST_ASSERT_EQUAL_INT64(IRON_ERR_CRYPTO_AUTHENTICATION, dr.error);
        release_result(&dr);
        release_result(&e);

        Iron_String short_key = text("short");
        s = Iron_crypto__seal(alg, short_key, nonce, pt, aad);
        TEST_ASSERT_EQUAL_INT64(IRON_ERR_CRYPTO_INVALID_KEY, s.error);
        release_result(&s);
        s = Iron_crypto__seal(alg, key, short_key, pt, aad);
        TEST_ASSERT_EQUAL_INT64(IRON_ERR_CRYPTO_INVALID_NONCE, s.error);
        release_result(&s);
        iron_string_release(&short_key);
        iron_string_release(&key);
        iron_string_release(&nonce);
        iron_string_release(&pt);
        iron_string_release(&aad);
    }
    TEST_ASSERT_EQUAL_INT64(0, Iron_crypto__random_int(0, 1));
    int64_t r = Iron_crypto__random_int(INT64_MIN, INT64_MAX);
    (void)r;
    Iron_String tok = Iron_crypto__random_token(300);
    TEST_ASSERT_EQUAL_UINT64(400, iron_string_byte_len(&tok));
    iron_string_release(&tok);
}

static void test_rsa_errors(void) {
    Iron_String junk = text("-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n");
    Iron_String msg = text("m");
    Iron_CryptoResult r = Iron_crypto__rsa_encrypt_oaep(junk, 1, msg);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_CRYPTO_INVALID_KEY, r.error);
    release_result(&r);
    r = Iron_crypto__rsa_decrypt_oaep(junk, 3, msg);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_CRYPTO_INVALID_KEY, r.error);
    release_result(&r);
    iron_string_release(&junk);
    iron_string_release(&msg);
}

#endif /* IRON_HAVE_OPENSSL */

static void test_compare_and_xor(void) {
    Iron_String a = text("secret"), b = text("secreT"), c = text("secret");
    TEST_ASSERT_TRUE(Iron_crypto_constant_time_equal(a, c));
    TEST_ASSERT_FALSE(Iron_crypto_constant_time_equal(a, b));
    Iron_String k = text("\x01");
    Iron_String x = Iron_crypto_xor(a, k);
    Iron_String back = Iron_crypto_xor(x, k);
    TEST_ASSERT_TRUE(iron_string_equals(&back, &a));
    iron_string_release(&x);
    iron_string_release(&back);
    iron_string_release(&k);
    iron_string_release(&a);
    iron_string_release(&b);
    iron_string_release(&c);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_alloc_and_seal_bytes);
    RUN_TEST(test_encoding);
    RUN_TEST(test_compare_and_xor);
#ifdef IRON_HAVE_OPENSSL
    RUN_TEST(test_digests);
    RUN_TEST(test_kdfs);
    RUN_TEST(test_aead);
    RUN_TEST(test_rsa_errors);
#endif
    return UNITY_END();
}
