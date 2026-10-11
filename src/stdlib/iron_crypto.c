/* iron_crypto.c: src/stdlib/crypto.iron on OpenSSL 3 libcrypto.
 *
 * Every primitive is libcrypto's (EVP digests, EVP_MAC HMAC, EVP_KDF HKDF,
 * PKCS5_PBKDF2_HMAC, EVP_PBE_scrypt, EVP AEAD ciphers, RAND_bytes,
 * CRYPTO_memcmp): constant time where it matters and accelerated where the
 * CPU allows. Nothing here implements an algorithm itself.
 *
 * Built twice, like iron_tls.c: with IRON_HAVE_OPENSSL=1 for programs that
 * import crypto (the bundles link libiron_tls.a, which carries this object,
 * against their static OpenSSL), and without it, where fallible calls
 * report IRON_ERR_CRYPTO_UNAVAILABLE and infallible ones panic.
 *
 * Strings cross the boundary by value and are borrowed: inputs are read in
 * place through iron_string_cstr, never copied, and outputs are written
 * directly into the returned string (iron_string_alloc_bytes). */

#include "iron_crypto.h"
#include "iron_encoding.h"
#include "runtime/iron_errors.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(IRON_HAVE_OPENSSL)
#include <openssl/opensslv.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#define IRON_CRYPTO_OPENSSL 1
#endif
#endif

#ifdef IRON_CRYPTO_OPENSSL
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/decoder.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#endif

/* ── Shared helpers ───────────────────────────────────────────────────── */

#define CR_TAG_LEN   16
#define CR_NONCE_LEN 12
#define CR_KEY_LEN   32
/* Strings hold at most 4 GiB (a 32-bit byte length). */
#define CR_MAX_STRING ((size_t)UINT32_MAX)

static inline const uint8_t *cr_bytes(const Iron_String *s) {
    return (const uint8_t *)iron_string_cstr(s);
}
static inline size_t cr_len(const Iron_String *s) {
    return iron_string_byte_len(s);
}

static Iron_String cr_cstr(const char *text) {
    return iron_string_from_cstr(text, strlen(text));
}

static Iron_CryptoResult cr_fail(int64_t code, const char *message) {
#ifdef IRON_CRYPTO_OPENSSL
    ERR_clear_error();  /* leave no stale entries for the next call or for TLS */
#endif
    Iron_CryptoResult out;
    out.data = iron_string_from_cstr("", 0);
    out.error = code;
    out.error_message = cr_cstr(message);
    return out;
}

IRON_NORETURN static void cr_panic(const char *message) {
    fflush(stdout);
    fprintf(stderr, "iron: panic: crypto: %s\n", message);
    fflush(stderr);
    iron_abort();
}

void Iron_cryptoresult_release(Iron_CryptoResult result) {
    /* Strings are released by the value's own drop glue; kept so the
     * module matches the other result types. */
    (void)result;
}

/* Constant-time comparison and XOR need no OpenSSL. */

bool Iron_crypto_constant_time_equal(Iron_String a, Iron_String b) {
    size_t la = cr_len(&a), lb = cr_len(&b);
    if (la != lb) return false;  /* lengths are not secret */
#ifdef IRON_CRYPTO_OPENSSL
    return CRYPTO_memcmp(cr_bytes(&a), cr_bytes(&b), la) == 0;
#else
    const volatile uint8_t *x = cr_bytes(&a), *y = cr_bytes(&b);
    uint8_t diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (uint8_t)(x[i] ^ y[i]);
    return diff == 0;
#endif
}

Iron_String Iron_crypto_xor(Iron_String data, Iron_String key) {
    const uint8_t *in = cr_bytes(&data), *k = cr_bytes(&key);
    size_t len = cr_len(&data), klen = cr_len(&key);
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, len, &p);
    if (klen == 0) {
        memcpy(p, in, len);
    } else {
        for (size_t i = 0, j = 0; i < len; i++) {
            p[i] = (char)(in[i] ^ k[j]);
            if (++j == klen) j = 0;
        }
    }
    iron_string_seal_bytes(&out, len);
    return out;
}

#ifdef IRON_CRYPTO_OPENSSL

static Iron_CryptoResult cr_ok(Iron_String data) {
    Iron_CryptoResult out;
    out.data = data;
    out.error = 0;
    out.error_message = iron_string_from_cstr("", 0);
    return out;
}

/* ── Algorithm tables (fetched once, shared by every thread) ──────────── */

#define CR_HASH_COUNT 9
static const char *const k_hash_names[CR_HASH_COUNT] = {
    NULL, "SHA1", "SHA2-224", "SHA2-256", "SHA2-384", "SHA2-512", "SHA3-256", "SHA3-512", "MD5",
};
/* The names PHC-style password hash strings use. */
static const char *const k_hash_phc[CR_HASH_COUNT] = {
    NULL, "sha1", "sha224", "sha256", "sha384", "sha512", NULL, NULL, NULL,
};

static EVP_MD *s_md[CR_HASH_COUNT];
static EVP_MAC *s_hmac;
static EVP_KDF *s_hkdf;
static EVP_CIPHER *s_aead[3];
static iron_once_t s_fetch_once = IRON_ONCE_INIT;

/* Explicit fetches: the EVP_sha256()-style getters re-fetch implicitly on
 * every init in OpenSSL 3, which costs a lock per call. */
static void cr_fetch_all(void) {
    for (int i = 1; i < CR_HASH_COUNT; i++) s_md[i] = EVP_MD_fetch(NULL, k_hash_names[i], NULL);
    s_hmac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    s_hkdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    s_aead[1] = EVP_CIPHER_fetch(NULL, "AES-256-GCM", NULL);
    s_aead[2] = EVP_CIPHER_fetch(NULL, "ChaCha20-Poly1305", NULL);
    ERR_clear_error();
}

static const EVP_MD *cr_md(int64_t alg) {
    if (alg < 1 || alg >= CR_HASH_COUNT) return NULL;
    iron_once(&s_fetch_once, cr_fetch_all);
    return s_md[alg];
}

/* For the infallible hashing calls: an unknown or unavailable algorithm
 * is a broken build (crypto.iron only passes valid ids), not user input. */
static const EVP_MD *cr_need_md(int64_t alg) {
    const EVP_MD *md = cr_md(alg);
    if (!md) {
        char message[96];
        snprintf(message, sizeof(message), "hash algorithm %s is not available in this OpenSSL",
                 (alg >= 1 && alg < CR_HASH_COUNT) ? k_hash_names[alg] : "(unknown)");
        cr_panic(message);
    }
    return md;
}

static EVP_MAC *cr_hmac_mac(void) {
    iron_once(&s_fetch_once, cr_fetch_all);
    if (!s_hmac) cr_panic("HMAC is not available in this OpenSSL");
    return s_hmac;
}

static const EVP_CIPHER *cr_aead(int64_t alg) {
    if (alg < 1 || alg > 2) return NULL;
    iron_once(&s_fetch_once, cr_fetch_all);
    return s_aead[alg];
}

/* Feeds `len` bytes to an int-sized libcrypto update in 1 GiB steps. */
#define CR_CHUNK ((size_t)1 << 30)

/* ── Hashing ─────────────────────────────────────────────────────────── */

Iron_String Iron_crypto__digest(int64_t alg, Iron_String data) {
    const EVP_MD *md = cr_need_md(alg);
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (!EVP_Digest(cr_bytes(&data), cr_len(&data), out, &n, md, NULL)) cr_panic("digest failed");
    return iron_string_from_cstr((const char *)out, n);
}

static EVP_MAC_CTX *cr_hmac_ctx(int64_t alg, const uint8_t *key, size_t key_len) {
    const EVP_MD *md = cr_need_md(alg);
    EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(cr_hmac_mac());
    if (!ctx) iron_oom_abort("iron_crypto.c:cr_hmac_ctx");
    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
                                                 (char *)EVP_MD_get0_name(md), 0);
    params[1] = OSSL_PARAM_construct_end();
    /* HMAC allows an empty key; libcrypto wants a non-NULL pointer. */
    static const uint8_t empty = 0;
    if (!EVP_MAC_init(ctx, key_len ? key : &empty, key_len, params)) cr_panic("HMAC init failed");
    return ctx;
}

Iron_String Iron_crypto__hmac(int64_t alg, Iron_String key, Iron_String data) {
    EVP_MAC_CTX *ctx = cr_hmac_ctx(alg, cr_bytes(&key), cr_len(&key));
    unsigned char out[EVP_MAX_MD_SIZE];
    size_t n = 0;
    if (!EVP_MAC_update(ctx, cr_bytes(&data), cr_len(&data)) ||
        !EVP_MAC_final(ctx, out, &n, sizeof(out)))
        cr_panic("HMAC failed");
    EVP_MAC_CTX_free(ctx);
    return iron_string_from_cstr((const char *)out, n);
}

/* Streaming state behind a Hasher's Int handle: a digest or an HMAC. */
typedef struct {
    const EVP_MD *md;
    EVP_MD_CTX *digest;
    EVP_MAC_CTX *mac;
} CryptoHasher;

int64_t Iron_crypto__hasher_new(int64_t alg) {
    const EVP_MD *md = cr_need_md(alg);
    CryptoHasher *h = (CryptoHasher *)calloc(1, sizeof(*h));
    if (!h) iron_oom_abort("iron_crypto.c:hasher_new");
    h->md = md;
    h->digest = EVP_MD_CTX_new();
    if (!h->digest) iron_oom_abort("iron_crypto.c:hasher_new");
    if (!EVP_DigestInit_ex2(h->digest, md, NULL)) cr_panic("digest init failed");
    return (int64_t)(intptr_t)h;
}

int64_t Iron_crypto__hmac_new(int64_t alg, Iron_String key) {
    CryptoHasher *h = (CryptoHasher *)calloc(1, sizeof(*h));
    if (!h) iron_oom_abort("iron_crypto.c:hmac_new");
    h->md = cr_need_md(alg);
    h->mac = cr_hmac_ctx(alg, cr_bytes(&key), cr_len(&key));
    return (int64_t)(intptr_t)h;
}

void Iron_crypto__hasher_update(int64_t handle, Iron_String data) {
    CryptoHasher *h = (CryptoHasher *)(intptr_t)handle;
    if (!h) return;
    int ok = h->mac ? EVP_MAC_update(h->mac, cr_bytes(&data), cr_len(&data))
                    : EVP_DigestUpdate(h->digest, cr_bytes(&data), cr_len(&data));
    if (!ok) cr_panic("hash update failed");
}

/* The digest of everything written since creation or the previous finish;
 * the hasher starts over (an HMAC keeps its key), so it can be reused. */
Iron_String Iron_crypto__hasher_finish(int64_t handle) {
    CryptoHasher *h = (CryptoHasher *)(intptr_t)handle;
    if (!h) return iron_string_from_cstr("", 0);
    unsigned char out[EVP_MAX_MD_SIZE];
    size_t n = 0;
    if (h->mac) {
        if (!EVP_MAC_final(h->mac, out, &n, sizeof(out)) ||
            !EVP_MAC_init(h->mac, NULL, 0, NULL))
            cr_panic("HMAC finish failed");
    } else {
        unsigned int dn = 0;
        if (!EVP_DigestFinal_ex(h->digest, out, &dn) ||
            !EVP_DigestInit_ex2(h->digest, h->md, NULL))
            cr_panic("digest finish failed");
        n = dn;
    }
    return iron_string_from_cstr((const char *)out, n);
}

void Iron_crypto__hasher_free(int64_t handle) {
    CryptoHasher *h = (CryptoHasher *)(intptr_t)handle;
    if (!h) return;
    EVP_MD_CTX_free(h->digest);
    EVP_MAC_CTX_free(h->mac);
    free(h);
}

/* ── Key derivation ──────────────────────────────────────────────────── */

Iron_CryptoResult Iron_crypto__hkdf(int64_t alg, Iron_String ikm, Iron_String salt,
                                    Iron_String info, int64_t length) {
    const EVP_MD *md = cr_md(alg);
    if (!md) return cr_fail(IRON_ERR_CRYPTO_INVALID_ARGUMENT, "hash algorithm not available");
    int64_t max = 255 * (int64_t)EVP_MD_get_size(md);
    if (length < 1 || length > max)
        return cr_fail(IRON_ERR_CRYPTO_INVALID_LENGTH, "HKDF length must be 1 to 255 * the hash size");
    if (cr_len(&ikm) == 0)
        return cr_fail(IRON_ERR_CRYPTO_INVALID_KEY, "HKDF input key material is empty");
    iron_once(&s_fetch_once, cr_fetch_all);
    if (!s_hkdf) return cr_fail(IRON_ERR_CRYPTO_INTERNAL, "HKDF is not available in this OpenSSL");
    EVP_KDF_CTX *kctx = EVP_KDF_CTX_new(s_hkdf);
    if (!kctx) iron_oom_abort("iron_crypto.c:hkdf");
    OSSL_PARAM params[5], *p = params;
    *p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char *)EVP_MD_get0_name(md), 0);
    *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void *)cr_bytes(&ikm), cr_len(&ikm));
    if (cr_len(&salt) > 0)  /* absent salt is HashLen zero bytes (RFC 5869 2.2) */
        *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)cr_bytes(&salt), cr_len(&salt));
    if (cr_len(&info) > 0)
        *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void *)cr_bytes(&info), cr_len(&info));
    *p = OSSL_PARAM_construct_end();
    Iron_String out;
    char *buf;
    iron_string_alloc_bytes(&out, (size_t)length, &buf);
    int ok = EVP_KDF_derive(kctx, (unsigned char *)buf, (size_t)length, params);
    EVP_KDF_CTX_free(kctx);
    if (!ok) {
        iron_string_release(&out);
        return cr_fail(IRON_ERR_CRYPTO_INTERNAL, "HKDF failed");
    }
    iron_string_seal_bytes(&out, (size_t)length);
    return cr_ok(out);
}

#define CR_MAX_DERIVED ((int64_t)1 << 20)  /* 1 MiB of derived key is plenty */

static int64_t cr_pbkdf2_into(const EVP_MD *md, const Iron_String *password, const uint8_t *salt,
                              size_t salt_len, int64_t iterations, uint8_t *out, size_t out_len) {
    if (cr_len(password) > INT_MAX || salt_len > INT_MAX) return IRON_ERR_CRYPTO_INVALID_ARGUMENT;
    static const char empty = 0;
    const char *pw = cr_len(password) ? (const char *)cr_bytes(password) : &empty;
    if (!PKCS5_PBKDF2_HMAC(pw, (int)cr_len(password), salt_len ? salt : (const uint8_t *)&empty,
                           (int)salt_len, (int)iterations, md, (int)out_len, out))
        return IRON_ERR_CRYPTO_INTERNAL;
    return 0;
}

Iron_CryptoResult Iron_crypto__pbkdf2(int64_t alg, Iron_String password, Iron_String salt,
                                      int64_t iterations, int64_t length) {
    const EVP_MD *md = cr_md(alg);
    if (!md) return cr_fail(IRON_ERR_CRYPTO_INVALID_ARGUMENT, "hash algorithm not available");
    if (iterations < 1 || iterations > INT_MAX)
        return cr_fail(IRON_ERR_CRYPTO_INVALID_PARAMETERS, "PBKDF2 iterations must be 1 to 2147483647");
    if (length < 1 || length > CR_MAX_DERIVED)
        return cr_fail(IRON_ERR_CRYPTO_INVALID_LENGTH, "PBKDF2 length must be 1 to 1048576 bytes");
    Iron_String out;
    char *buf;
    iron_string_alloc_bytes(&out, (size_t)length, &buf);
    int64_t code = cr_pbkdf2_into(md, &password, cr_bytes(&salt), cr_len(&salt), iterations,
                                  (uint8_t *)buf, (size_t)length);
    if (code != 0) {
        iron_string_release(&out);
        return cr_fail(code, code == IRON_ERR_CRYPTO_INTERNAL ? "PBKDF2 failed"
                                                              : "password or salt too long");
    }
    iron_string_seal_bytes(&out, (size_t)length);
    return cr_ok(out);
}

/* scrypt cost limits: N = 2^log_n with log_n in 1..30, r * p < 2^30, and
 * at most 1 GiB of working memory (128 * r * N, plus 128 * r * p). */
#define CR_SCRYPT_MAX_MEM ((uint64_t)1 << 30)

static int64_t cr_scrypt_check(int64_t log_n, int64_t r, int64_t p, uint64_t *maxmem) {
    if (log_n < 1 || log_n > 30 || r < 1 || p < 1 || r > (1 << 30) || p > (1 << 30) ||
        (uint64_t)r * (uint64_t)p >= ((uint64_t)1 << 30))
        return IRON_ERR_CRYPTO_INVALID_PARAMETERS;
    uint64_t n = (uint64_t)1 << log_n;
    uint64_t v = 128 * (uint64_t)r * (n + 2);
    if (128 * (uint64_t)r * n > CR_SCRYPT_MAX_MEM) return IRON_ERR_CRYPTO_INVALID_PARAMETERS;
    *maxmem = v + 128 * (uint64_t)r * (uint64_t)p;
    return 0;
}

static int64_t cr_scrypt_into(const Iron_String *password, const uint8_t *salt, size_t salt_len,
                              int64_t log_n, int64_t r, int64_t p, uint8_t *out, size_t out_len) {
    uint64_t maxmem = 0;
    int64_t code = cr_scrypt_check(log_n, r, p, &maxmem);
    if (code != 0) return code;
    static const char empty = 0;
    const char *pw = cr_len(password) ? (const char *)cr_bytes(password) : &empty;
    if (!EVP_PBE_scrypt(pw, cr_len(password), salt_len ? salt : (const uint8_t *)&empty, salt_len,
                        (uint64_t)1 << log_n, (uint64_t)r, (uint64_t)p, maxmem, out, out_len))
        return IRON_ERR_CRYPTO_INTERNAL;
    return 0;
}

static const char *cr_scrypt_message(int64_t code) {
    return code == IRON_ERR_CRYPTO_INVALID_PARAMETERS
               ? "scrypt parameters out of range (log_n 1..30, r * p < 2^30, memory at most 1 GiB)"
               : "scrypt failed";
}

Iron_CryptoResult Iron_crypto_scrypt(Iron_String password, Iron_String salt, int64_t log_n,
                                     int64_t r, int64_t p, int64_t length) {
    if (length < 1 || length > CR_MAX_DERIVED)
        return cr_fail(IRON_ERR_CRYPTO_INVALID_LENGTH, "scrypt length must be 1 to 1048576 bytes");
    Iron_String out;
    char *buf;
    iron_string_alloc_bytes(&out, (size_t)length, &buf);
    int64_t code = cr_scrypt_into(&password, cr_bytes(&salt), cr_len(&salt), log_n, r, p,
                                  (uint8_t *)buf, (size_t)length);
    if (code != 0) {
        iron_string_release(&out);
        return cr_fail(code, cr_scrypt_message(code));
    }
    iron_string_seal_bytes(&out, (size_t)length);
    return cr_ok(out);
}

/* ── Randomness ──────────────────────────────────────────────────────── */

static void cr_random_fill(uint8_t *out, size_t len) {
    while (len > 0) {
        size_t step = len > CR_CHUNK ? CR_CHUNK : len;
        if (RAND_bytes(out, (int)step) != 1) cr_panic("the random number generator failed");
        out += step;
        len -= step;
    }
}

Iron_String Iron_crypto__random_bytes(int64_t count) {
    if (count < 0 || (uint64_t)count > CR_MAX_STRING) cr_panic("random_bytes count out of range");
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, (size_t)count, &p);
    cr_random_fill((uint8_t *)p, (size_t)count);
    iron_string_seal_bytes(&out, (size_t)count);
    return out;
}

/* Uniform in [low, high) by rejection: values below 2^64 mod range are
 * redrawn, so every residue is equally likely. */
int64_t Iron_crypto__random_int(int64_t low, int64_t high) {
    if (high <= low) cr_panic("random_int needs low < high");
    uint64_t range = (uint64_t)high - (uint64_t)low;
    uint64_t floor = (0 - range) % range;
    uint64_t x;
    do {
        cr_random_fill((uint8_t *)&x, sizeof(x));
    } while (x < floor);
    return (int64_t)((uint64_t)low + x % range);
}

Iron_String Iron_crypto__random_token(int64_t count) {
    if (count < 0 || count > (1 << 20)) cr_panic("random_token byte count out of range");
    uint8_t stack[256];
    uint8_t *raw = count <= (int64_t)sizeof(stack) ? stack : (uint8_t *)malloc((size_t)count);
    if (!raw) iron_oom_abort("iron_crypto.c:random_token");
    cr_random_fill(raw, (size_t)count);
    size_t n = iron_base64_encoded_len((size_t)count, false);
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, n, &p);
    iron_base64_encode_raw(raw, (size_t)count, p, true, false);
    iron_string_seal_bytes(&out, n);
    OPENSSL_cleanse(raw, (size_t)count);
    if (raw != stack) free(raw);
    return out;
}

/* ── Password hashing ────────────────────────────────────────────────── */

/* PHC string format with standard base64 without padding:
 *   $scrypt$ln=<log2 N>,r=<r>,p=<p>$<salt>$<hash>
 *   $pbkdf2-<sha1|sha224|sha256|sha384|sha512>$i=<iterations>$<salt>$<hash> */
#define CR_SALT_LEN 16
#define CR_SCRYPT_DK_LEN 32

static Iron_String cr_phc_encode(const char *head, const uint8_t *salt, size_t salt_len,
                                 const uint8_t *hash, size_t hash_len) {
    size_t hl = strlen(head);
    size_t sl = iron_base64_encoded_len(salt_len, false);
    size_t dl = iron_base64_encoded_len(hash_len, false);
    size_t total = hl + sl + 1 + dl;
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, total, &p);
    memcpy(p, head, hl);
    iron_base64_encode_raw(salt, salt_len, p + hl, false, false);
    p[hl + sl] = '$';
    iron_base64_encode_raw(hash, hash_len, p + hl + sl + 1, false, false);
    iron_string_seal_bytes(&out, total);
    return out;
}

Iron_CryptoResult Iron_crypto__hash_password_scrypt(Iron_String password, int64_t log_n,
                                                    int64_t r, int64_t p) {
    uint8_t salt[CR_SALT_LEN], dk[CR_SCRYPT_DK_LEN];
    cr_random_fill(salt, sizeof(salt));
    int64_t code = cr_scrypt_into(&password, salt, sizeof(salt), log_n, r, p, dk, sizeof(dk));
    if (code != 0) return cr_fail(code, cr_scrypt_message(code));
    char head[96];
    snprintf(head, sizeof(head), "$scrypt$ln=%lld,r=%lld,p=%lld$", (long long)log_n,
             (long long)r, (long long)p);
    Iron_String out = cr_phc_encode(head, salt, sizeof(salt), dk, sizeof(dk));
    OPENSSL_cleanse(dk, sizeof(dk));
    return cr_ok(out);
}

Iron_CryptoResult Iron_crypto__hash_password_pbkdf2(Iron_String password, int64_t alg,
                                                    int64_t iterations) {
    const EVP_MD *md = cr_md(alg);
    if (!md || !k_hash_phc[alg])
        return cr_fail(IRON_ERR_CRYPTO_INVALID_ARGUMENT,
                       "PBKDF2 password hashes use SHA-1, SHA-224, SHA-256, SHA-384 or SHA-512");
    if (iterations < 1 || iterations > INT_MAX)
        return cr_fail(IRON_ERR_CRYPTO_INVALID_PARAMETERS, "PBKDF2 iterations must be 1 to 2147483647");
    uint8_t salt[CR_SALT_LEN], dk[EVP_MAX_MD_SIZE];
    size_t dk_len = (size_t)EVP_MD_get_size(md);
    cr_random_fill(salt, sizeof(salt));
    int64_t code = cr_pbkdf2_into(md, &password, salt, sizeof(salt), iterations, dk, dk_len);
    if (code != 0) return cr_fail(code, "PBKDF2 failed");
    char head[96];
    snprintf(head, sizeof(head), "$pbkdf2-%s$i=%lld$", k_hash_phc[alg], (long long)iterations);
    Iron_String out = cr_phc_encode(head, salt, sizeof(salt), dk, dk_len);
    OPENSSL_cleanse(dk, sizeof(dk));
    return cr_ok(out);
}

/* A strict little parser for the strings above. */
typedef struct { const char *p, *end; } CrCursor;

static bool cr_take(CrCursor *c, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) return false;
    c->p += n;
    return true;
}

static bool cr_take_uint(CrCursor *c, int64_t max, int64_t *value) {
    int64_t v = 0;
    const char *start = c->p;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        v = v * 10 + (*c->p - '0');
        if (v > max) return false;
        c->p++;
    }
    /* digits required, no leading zeros */
    if (c->p == start || (c->p - start > 1 && *start == '0')) return false;
    *value = v;
    return true;
}

/* Decodes a '$'-terminated (or final) unpadded base64 field. */
static bool cr_take_b64(CrCursor *c, bool last, uint8_t *out, size_t cap, size_t *out_len) {
    const char *start = c->p;
    while (c->p < c->end && *c->p != '$') c->p++;
    size_t len = (size_t)(c->p - start);
    if (len == 0 || len / 4 * 3 + 3 > cap) return false;
    if (last ? c->p != c->end : !cr_take(c, "$")) return false;
    size_t bad = 0;
    return iron_base64_decode_raw(start, len, false, false, out, out_len, &bad) == 0;
}

#define CR_MAX_HASH_BYTES 256

bool Iron_crypto_verify_password(Iron_String password, Iron_String encoded) {
    CrCursor c = { iron_string_cstr(&encoded), iron_string_cstr(&encoded) + cr_len(&encoded) };
    uint8_t salt[CR_MAX_HASH_BYTES], want[CR_MAX_HASH_BYTES], got[CR_MAX_HASH_BYTES];
    size_t salt_len = 0, want_len = 0;
    int64_t code;
    if (cr_take(&c, "$scrypt$")) {
        int64_t log_n, r, p;
        if (!cr_take(&c, "ln=") || !cr_take_uint(&c, 30, &log_n) || !cr_take(&c, ",r=") ||
            !cr_take_uint(&c, 1 << 30, &r) || !cr_take(&c, ",p=") ||
            !cr_take_uint(&c, 1 << 30, &p) || !cr_take(&c, "$") ||
            !cr_take_b64(&c, false, salt, sizeof(salt), &salt_len) ||
            !cr_take_b64(&c, true, want, sizeof(want), &want_len) || want_len < 16)
            return false;
        code = cr_scrypt_into(&password, salt, salt_len, log_n, r, p, got, want_len);
    } else if (cr_take(&c, "$pbkdf2-")) {
        int64_t alg = 0, iterations;
        for (int64_t i = 1; i < CR_HASH_COUNT; i++) {
            if (!k_hash_phc[i]) continue;
            CrCursor t = c;
            if (cr_take(&t, k_hash_phc[i]) && cr_take(&t, "$")) {
                alg = i;
                c = t;
                break;
            }
        }
        const EVP_MD *md = cr_md(alg);
        if (!md || !cr_take(&c, "i=") || !cr_take_uint(&c, INT_MAX, &iterations) ||
            iterations < 1 || !cr_take(&c, "$") ||
            !cr_take_b64(&c, false, salt, sizeof(salt), &salt_len) ||
            !cr_take_b64(&c, true, want, sizeof(want), &want_len) || want_len < 16)
            return false;
        code = cr_pbkdf2_into(md, &password, salt, salt_len, iterations, got, want_len);
    } else {
        return false;
    }
    bool match = code == 0 && CRYPTO_memcmp(got, want, want_len) == 0;
    OPENSSL_cleanse(got, sizeof(got));
    ERR_clear_error();
    return match;
}

/* ── Authenticated encryption ────────────────────────────────────────── */

static int64_t cr_aead_check(int64_t alg, const Iron_String *key, size_t nonce_len,
                             const EVP_CIPHER **cipher, const char **message) {
    *cipher = cr_aead(alg);
    if (!*cipher) {
        *message = "AEAD algorithm not available";
        return IRON_ERR_CRYPTO_INVALID_ARGUMENT;
    }
    if (cr_len(key) != CR_KEY_LEN) {
        *message = "the key must be 32 bytes";
        return IRON_ERR_CRYPTO_INVALID_KEY;
    }
    if (nonce_len != CR_NONCE_LEN) {
        *message = "the nonce must be 12 bytes";
        return IRON_ERR_CRYPTO_INVALID_NONCE;
    }
    return 0;
}

static bool cr_aad(EVP_CIPHER_CTX *ctx, bool enc, const uint8_t *aad, size_t len) {
    while (len > 0) {
        size_t step = len > CR_CHUNK ? CR_CHUNK : len;
        int outl = 0;
        int ok = enc ? EVP_EncryptUpdate(ctx, NULL, &outl, aad, (int)step)
                     : EVP_DecryptUpdate(ctx, NULL, &outl, aad, (int)step);
        if (!ok) return false;
        aad += step;
        len -= step;
    }
    return true;
}

static bool cr_crypt(EVP_CIPHER_CTX *ctx, bool enc, const uint8_t *in, size_t len, uint8_t *out) {
    while (len > 0) {
        size_t step = len > CR_CHUNK ? CR_CHUNK : len;
        int outl = 0;
        int ok = enc ? EVP_EncryptUpdate(ctx, out, &outl, in, (int)step)
                     : EVP_DecryptUpdate(ctx, out, &outl, in, (int)step);
        if (!ok || (size_t)outl != step) return false;  /* AEAD stream modes do not buffer */
        in += step;
        out += step;
        len -= step;
    }
    return true;
}

/* Writes ciphertext || tag (len + 16 bytes) to `out`. */
static bool cr_seal_into(const EVP_CIPHER *cipher, const uint8_t *key, const uint8_t *nonce,
                         const uint8_t *pt, size_t len, const uint8_t *aad, size_t aad_len,
                         uint8_t *out) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) iron_oom_abort("iron_crypto.c:seal");
    int outl = 0;
    bool ok = EVP_EncryptInit_ex2(ctx, cipher, key, nonce, NULL) &&
              cr_aad(ctx, true, aad, aad_len) && cr_crypt(ctx, true, pt, len, out) &&
              EVP_EncryptFinal_ex(ctx, out + len, &outl) && outl == 0 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, CR_TAG_LEN, out + len) > 0;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

/* Verifies the tag of ct || tag (len >= 16) and writes len - 16 bytes of
 * plaintext to `out`. Returns false on a forged or corrupt message. */
static bool cr_open_into(const EVP_CIPHER *cipher, const uint8_t *key, const uint8_t *nonce,
                         const uint8_t *sealed, size_t len, const uint8_t *aad, size_t aad_len,
                         uint8_t *out) {
    size_t ct_len = len - CR_TAG_LEN;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) iron_oom_abort("iron_crypto.c:open");
    int outl = 0;
    bool ok = EVP_DecryptInit_ex2(ctx, cipher, key, nonce, NULL) &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, CR_TAG_LEN,
                                  (void *)(sealed + ct_len)) > 0 &&
              cr_aad(ctx, false, aad, aad_len) && cr_crypt(ctx, false, sealed, ct_len, out) &&
              EVP_DecryptFinal_ex(ctx, out + ct_len, &outl) > 0 && outl == 0;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

Iron_CryptoResult Iron_crypto__seal(int64_t alg, Iron_String key, Iron_String nonce,
                                    Iron_String plaintext, Iron_String aad) {
    const EVP_CIPHER *cipher;
    const char *message;
    int64_t code = cr_aead_check(alg, &key, cr_len(&nonce), &cipher, &message);
    if (code != 0) return cr_fail(code, message);
    size_t len = cr_len(&plaintext);
    if (len > CR_MAX_STRING - CR_TAG_LEN)
        return cr_fail(IRON_ERR_CRYPTO_INVALID_LENGTH, "the plaintext is too long");
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, len + CR_TAG_LEN, &p);
    if (!cr_seal_into(cipher, cr_bytes(&key), cr_bytes(&nonce), cr_bytes(&plaintext), len,
                      cr_bytes(&aad), cr_len(&aad), (uint8_t *)p)) {
        iron_string_release(&out);
        return cr_fail(IRON_ERR_CRYPTO_INTERNAL, "encryption failed");
    }
    iron_string_seal_bytes(&out, len + CR_TAG_LEN);
    return cr_ok(out);
}

static Iron_CryptoResult cr_auth_fail(Iron_String *out, size_t len) {
    /* Never hand back unauthenticated plaintext, not even in freed memory. */
    char *p = (char *)iron_string_cstr(out);
    OPENSSL_cleanse(p, len);
    iron_string_release(out);
    return cr_fail(IRON_ERR_CRYPTO_AUTHENTICATION,
                   "authentication failed: the message, nonce, key or associated data do not match");
}

Iron_CryptoResult Iron_crypto__open(int64_t alg, Iron_String key, Iron_String nonce,
                                    Iron_String sealed, Iron_String aad) {
    const EVP_CIPHER *cipher;
    const char *message;
    int64_t code = cr_aead_check(alg, &key, cr_len(&nonce), &cipher, &message);
    if (code != 0) return cr_fail(code, message);
    size_t len = cr_len(&sealed);
    if (len < CR_TAG_LEN)
        return cr_fail(IRON_ERR_CRYPTO_AUTHENTICATION, "authentication failed: the message is shorter than its tag");
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, len - CR_TAG_LEN, &p);
    if (!cr_open_into(cipher, cr_bytes(&key), cr_bytes(&nonce), cr_bytes(&sealed), len,
                      cr_bytes(&aad), cr_len(&aad), (uint8_t *)p))
        return cr_auth_fail(&out, len - CR_TAG_LEN);
    iron_string_seal_bytes(&out, len - CR_TAG_LEN);
    return cr_ok(out);
}

Iron_CryptoResult Iron_crypto__encrypt(int64_t alg, Iron_String key, Iron_String plaintext,
                                       Iron_String aad) {
    const EVP_CIPHER *cipher;
    const char *message;
    int64_t code = cr_aead_check(alg, &key, CR_NONCE_LEN, &cipher, &message);
    if (code != 0) return cr_fail(code, message);
    size_t len = cr_len(&plaintext);
    if (len > CR_MAX_STRING - CR_TAG_LEN - CR_NONCE_LEN)
        return cr_fail(IRON_ERR_CRYPTO_INVALID_LENGTH, "the plaintext is too long");
    size_t total = CR_NONCE_LEN + len + CR_TAG_LEN;
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, total, &p);
    cr_random_fill((uint8_t *)p, CR_NONCE_LEN);
    if (!cr_seal_into(cipher, cr_bytes(&key), (const uint8_t *)p, cr_bytes(&plaintext), len,
                      cr_bytes(&aad), cr_len(&aad), (uint8_t *)p + CR_NONCE_LEN)) {
        iron_string_release(&out);
        return cr_fail(IRON_ERR_CRYPTO_INTERNAL, "encryption failed");
    }
    iron_string_seal_bytes(&out, total);
    return cr_ok(out);
}

Iron_CryptoResult Iron_crypto__decrypt(int64_t alg, Iron_String key, Iron_String message_in,
                                       Iron_String aad) {
    const EVP_CIPHER *cipher;
    const char *message;
    int64_t code = cr_aead_check(alg, &key, CR_NONCE_LEN, &cipher, &message);
    if (code != 0) return cr_fail(code, message);
    size_t len = cr_len(&message_in);
    if (len < CR_NONCE_LEN + CR_TAG_LEN)
        return cr_fail(IRON_ERR_CRYPTO_AUTHENTICATION,
                       "authentication failed: the message is shorter than its nonce and tag");
    const uint8_t *in = cr_bytes(&message_in);
    size_t pt_len = len - CR_NONCE_LEN - CR_TAG_LEN;
    Iron_String out;
    char *p;
    iron_string_alloc_bytes(&out, pt_len, &p);
    if (!cr_open_into(cipher, cr_bytes(&key), in, in + CR_NONCE_LEN, len - CR_NONCE_LEN,
                      cr_bytes(&aad), cr_len(&aad), (uint8_t *)p))
        return cr_auth_fail(&out, pt_len);
    iron_string_seal_bytes(&out, pt_len);
    return cr_ok(out);
}

/* ── RSA-OAEP ────────────────────────────────────────────────────────── */

/* Parses a PEM key: SubjectPublicKeyInfo or PKCS#1 for public keys,
 * unencrypted PKCS#8 or PKCS#1 for private keys. */
static EVP_PKEY *cr_load_rsa(const Iron_String *pem, bool private_key) {
    EVP_PKEY *pkey = NULL;
    OSSL_DECODER_CTX *dctx = OSSL_DECODER_CTX_new_for_pkey(
        &pkey, "PEM", NULL, "RSA",
        private_key ? OSSL_KEYMGMT_SELECT_KEYPAIR : OSSL_KEYMGMT_SELECT_PUBLIC_KEY, NULL, NULL);
    if (!dctx) return NULL;
    const unsigned char *data = cr_bytes(pem);
    size_t len = cr_len(pem);
    if (!OSSL_DECODER_from_data(dctx, &data, &len)) pkey = NULL;
    OSSL_DECODER_CTX_free(dctx);
    return pkey;
}

static Iron_CryptoResult cr_rsa_oaep(const Iron_String *pem, int64_t alg, const Iron_String *in,
                                     bool encrypt) {
    const EVP_MD *md = cr_md(alg);
    if (!md) return cr_fail(IRON_ERR_CRYPTO_INVALID_ARGUMENT, "hash algorithm not available");
    EVP_PKEY *pkey = cr_load_rsa(pem, !encrypt);
    if (!pkey)
        return cr_fail(IRON_ERR_CRYPTO_INVALID_KEY,
                       encrypt ? "not a PEM RSA public key" : "not an unencrypted PEM RSA private key");
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
    if (!pctx) iron_oom_abort("iron_crypto.c:rsa_oaep");
    Iron_CryptoResult result;
    size_t out_len = 0;
    int ok = (encrypt ? EVP_PKEY_encrypt_init(pctx) : EVP_PKEY_decrypt_init(pctx)) > 0 &&
             EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_OAEP_PADDING) > 0 &&
             EVP_PKEY_CTX_set_rsa_oaep_md(pctx, md) > 0 &&
             EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, md) > 0;
    if (ok) {
        ok = (encrypt ? EVP_PKEY_encrypt(pctx, NULL, &out_len, cr_bytes(in), cr_len(in))
                      : EVP_PKEY_decrypt(pctx, NULL, &out_len, cr_bytes(in), cr_len(in))) > 0;
    }
    if (!ok) {
        result = cr_fail(IRON_ERR_CRYPTO_INTERNAL, "RSA-OAEP setup failed");
    } else {
        Iron_String out;
        char *p;
        iron_string_alloc_bytes(&out, out_len, &p);
        ok = (encrypt ? EVP_PKEY_encrypt(pctx, (unsigned char *)p, &out_len, cr_bytes(in), cr_len(in))
                      : EVP_PKEY_decrypt(pctx, (unsigned char *)p, &out_len, cr_bytes(in), cr_len(in))) > 0;
        if (ok) {
            iron_string_seal_bytes(&out, out_len);
            result = cr_ok(out);
        } else {
            iron_string_release(&out);
            result = encrypt ? cr_fail(IRON_ERR_CRYPTO_INVALID_LENGTH,
                                       "the plaintext is too long for this key and hash")
                             : cr_fail(IRON_ERR_CRYPTO_AUTHENTICATION, "RSA-OAEP decryption failed");
        }
    }
    EVP_PKEY_CTX_free(pctx);
    EVP_PKEY_free(pkey);
    return result;
}

Iron_CryptoResult Iron_crypto__rsa_encrypt_oaep(Iron_String public_key_pem, int64_t alg,
                                                Iron_String plaintext) {
    return cr_rsa_oaep(&public_key_pem, alg, &plaintext, true);
}

Iron_CryptoResult Iron_crypto__rsa_decrypt_oaep(Iron_String private_key_pem, int64_t alg,
                                                Iron_String ciphertext) {
    return cr_rsa_oaep(&private_key_pem, alg, &ciphertext, false);
}

#else /* !IRON_CRYPTO_OPENSSL */

/* Built without OpenSSL 3: the fallible calls say so in their result, the
 * rest stop the program with the same explanation. */
#define CR_UNAVAILABLE "this program was built without OpenSSL 3, which the crypto module needs"

static Iron_CryptoResult cr_unavailable(void) {
    return cr_fail(IRON_ERR_CRYPTO_UNAVAILABLE, CR_UNAVAILABLE);
}

Iron_String Iron_crypto__digest(int64_t alg, Iron_String data) {
    (void)alg; (void)data;
    cr_panic(CR_UNAVAILABLE);
    return iron_string_from_cstr("", 0);
}
Iron_String Iron_crypto__hmac(int64_t alg, Iron_String key, Iron_String data) {
    (void)alg; (void)key; (void)data;
    cr_panic(CR_UNAVAILABLE);
    return iron_string_from_cstr("", 0);
}
int64_t Iron_crypto__hasher_new(int64_t alg) {
    (void)alg;
    cr_panic(CR_UNAVAILABLE);
    return 0;
}
int64_t Iron_crypto__hmac_new(int64_t alg, Iron_String key) {
    (void)alg; (void)key;
    cr_panic(CR_UNAVAILABLE);
    return 0;
}
void Iron_crypto__hasher_update(int64_t handle, Iron_String data) { (void)handle; (void)data; }
Iron_String Iron_crypto__hasher_finish(int64_t handle) {
    (void)handle;
    return iron_string_from_cstr("", 0);
}
void Iron_crypto__hasher_free(int64_t handle) { (void)handle; }
Iron_CryptoResult Iron_crypto__hkdf(int64_t alg, Iron_String ikm, Iron_String salt,
                                    Iron_String info, int64_t length) {
    (void)alg; (void)ikm; (void)salt; (void)info; (void)length;
    return cr_unavailable();
}
Iron_CryptoResult Iron_crypto__pbkdf2(int64_t alg, Iron_String password, Iron_String salt,
                                      int64_t iterations, int64_t length) {
    (void)alg; (void)password; (void)salt; (void)iterations; (void)length;
    return cr_unavailable();
}
Iron_CryptoResult Iron_crypto_scrypt(Iron_String password, Iron_String salt, int64_t log_n,
                                     int64_t r, int64_t p, int64_t length) {
    (void)password; (void)salt; (void)log_n; (void)r; (void)p; (void)length;
    return cr_unavailable();
}
Iron_CryptoResult Iron_crypto__hash_password_scrypt(Iron_String password, int64_t log_n,
                                                    int64_t r, int64_t p) {
    (void)password; (void)log_n; (void)r; (void)p;
    return cr_unavailable();
}
Iron_CryptoResult Iron_crypto__hash_password_pbkdf2(Iron_String password, int64_t alg,
                                                    int64_t iterations) {
    (void)password; (void)alg; (void)iterations;
    return cr_unavailable();
}
bool Iron_crypto_verify_password(Iron_String password, Iron_String encoded) {
    (void)password; (void)encoded;
    cr_panic(CR_UNAVAILABLE);
    return false;
}
Iron_String Iron_crypto__random_bytes(int64_t count) {
    (void)count;
    cr_panic(CR_UNAVAILABLE);
    return iron_string_from_cstr("", 0);
}
int64_t Iron_crypto__random_int(int64_t low, int64_t high) {
    (void)low; (void)high;
    cr_panic(CR_UNAVAILABLE);
    return 0;
}
Iron_String Iron_crypto__random_token(int64_t count) {
    (void)count;
    cr_panic(CR_UNAVAILABLE);
    return iron_string_from_cstr("", 0);
}
Iron_CryptoResult Iron_crypto__seal(int64_t alg, Iron_String key, Iron_String nonce,
                                    Iron_String plaintext, Iron_String aad) {
    (void)alg; (void)key; (void)nonce; (void)plaintext; (void)aad;
    return cr_unavailable();
}
Iron_CryptoResult Iron_crypto__open(int64_t alg, Iron_String key, Iron_String nonce,
                                    Iron_String sealed, Iron_String aad) {
    (void)alg; (void)key; (void)nonce; (void)sealed; (void)aad;
    return cr_unavailable();
}
Iron_CryptoResult Iron_crypto__encrypt(int64_t alg, Iron_String key, Iron_String plaintext,
                                       Iron_String aad) {
    (void)alg; (void)key; (void)plaintext; (void)aad;
    return cr_unavailable();
}
Iron_CryptoResult Iron_crypto__decrypt(int64_t alg, Iron_String key, Iron_String message,
                                       Iron_String aad) {
    (void)alg; (void)key; (void)message; (void)aad;
    return cr_unavailable();
}
Iron_CryptoResult Iron_crypto__rsa_encrypt_oaep(Iron_String public_key_pem, int64_t alg,
                                                Iron_String plaintext) {
    (void)public_key_pem; (void)alg; (void)plaintext;
    return cr_unavailable();
}
Iron_CryptoResult Iron_crypto__rsa_decrypt_oaep(Iron_String private_key_pem, int64_t alg,
                                                Iron_String ciphertext) {
    (void)private_key_pem; (void)alg; (void)ciphertext;
    return cr_unavailable();
}

#endif /* IRON_CRYPTO_OPENSSL */
