#ifndef IRON_CRYPTO_H
#define IRON_CRYPTO_H

/* iron_crypto.h: the C side of src/stdlib/crypto.iron, backed by OpenSSL 3
 * libcrypto (EVP). Built with IRON_HAVE_OPENSSL=1 when a program imports
 * crypto; without OpenSSL every fallible call reports
 * IRON_ERR_CRYPTO_UNAVAILABLE and the infallible ones panic.
 *
 * Binary data is an Iron String (binary safe). Inputs are read in place;
 * outputs are written straight into the result string.
 *
 * Algorithms cross the boundary as small integers (crypto.iron maps its
 * HashAlgorithm and AeadAlgorithm enums onto them):
 *   hash: 1 SHA-1, 2 SHA-224, 3 SHA-256, 4 SHA-384, 5 SHA-512,
 *         6 SHA3-256, 7 SHA3-512, 8 MD5
 *   aead: 1 AES-256-GCM, 2 ChaCha20-Poly1305
 *
 * Not included by generated C, which declares its own prototypes and its
 * own CryptoResult struct; the layout below must match crypto.iron. */

#include <stdint.h>
#include <stdbool.h>
#include "runtime/iron_runtime.h"

typedef struct Iron_CryptoResult {
    Iron_String data;
    int64_t error;
    Iron_String error_message;
} Iron_CryptoResult;

void Iron_cryptoresult_release(Iron_CryptoResult result);

/* Hashing and MACs. */
Iron_String Iron_crypto__digest(int64_t alg, Iron_String data);
Iron_String Iron_crypto__hmac(int64_t alg, Iron_String key, Iron_String data);
int64_t     Iron_crypto__hasher_new(int64_t alg);
int64_t     Iron_crypto__hmac_new(int64_t alg, Iron_String key);
void        Iron_crypto__hasher_update(int64_t handle, Iron_String data);
Iron_String Iron_crypto__hasher_finish(int64_t handle);
void        Iron_crypto__hasher_free(int64_t handle);

/* Key derivation and password hashing. */
Iron_CryptoResult Iron_crypto__hkdf(int64_t alg, Iron_String ikm, Iron_String salt,
                                    Iron_String info, int64_t length);
Iron_CryptoResult Iron_crypto__pbkdf2(int64_t alg, Iron_String password, Iron_String salt,
                                      int64_t iterations, int64_t length);
Iron_CryptoResult Iron_crypto_scrypt(Iron_String password, Iron_String salt, int64_t log_n,
                                     int64_t r, int64_t p, int64_t length);
Iron_CryptoResult Iron_crypto__hash_password_scrypt(Iron_String password, int64_t log_n,
                                                    int64_t r, int64_t p);
Iron_CryptoResult Iron_crypto__hash_password_pbkdf2(Iron_String password, int64_t alg,
                                                    int64_t iterations);
bool              Iron_crypto_verify_password(Iron_String password, Iron_String encoded);

/* Comparison and byte helpers. */
bool        Iron_crypto_constant_time_equal(Iron_String a, Iron_String b);
Iron_String Iron_crypto_xor(Iron_String data, Iron_String key);

/* Randomness from the OpenSSL DRBG (seeded by the operating system). */
Iron_String Iron_crypto__random_bytes(int64_t count);
int64_t     Iron_crypto__random_int(int64_t low, int64_t high);
Iron_String Iron_crypto__random_token(int64_t count);

/* Authenticated encryption. Sealed output is ciphertext || 16-byte tag;
 * encrypt/decrypt prefix a random 12-byte nonce. */
Iron_CryptoResult Iron_crypto__seal(int64_t alg, Iron_String key, Iron_String nonce,
                                    Iron_String plaintext, Iron_String aad);
Iron_CryptoResult Iron_crypto__open(int64_t alg, Iron_String key, Iron_String nonce,
                                    Iron_String sealed, Iron_String aad);
Iron_CryptoResult Iron_crypto__encrypt(int64_t alg, Iron_String key, Iron_String plaintext,
                                       Iron_String aad);
Iron_CryptoResult Iron_crypto__decrypt(int64_t alg, Iron_String key, Iron_String message,
                                       Iron_String aad);

/* RSA-OAEP with a PEM key (MGF1 uses the same hash). */
Iron_CryptoResult Iron_crypto__rsa_encrypt_oaep(Iron_String public_key_pem, int64_t alg,
                                                Iron_String plaintext);
Iron_CryptoResult Iron_crypto__rsa_decrypt_oaep(Iron_String private_key_pem, int64_t alg,
                                                Iron_String ciphertext);

#endif /* IRON_CRYPTO_H */
