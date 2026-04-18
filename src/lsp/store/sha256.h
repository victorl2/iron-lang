#ifndef IRON_LSP_STORE_SHA256_H
#define IRON_LSP_STORE_SHA256_H

/* Phase 2 Plan 04 Task 01 (CORE-12) -- SHA-256 digest.
 *
 * Public-domain implementation (Brad Conte); see sha256.c header. No
 * openssl / libsodium dependency. ~150 LOC. Used to detect silent
 * drift between editor buffer and server buffer after each didChange:
 * the server stores sha256(buffer) on every apply, and if the client
 * sends a `contentHash` extension field with its own hash, we log a
 * WARN when they diverge. Recovery is client-driven (didClose+didOpen). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compute the SHA-256 digest of (data, len). Writes 32 raw bytes to
 * out[32]. Does not NUL-terminate. */
void ilsp_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* Hex-encode a 32-byte digest into a 65-byte buffer (64 lowercase hex
 * chars + NUL). */
void ilsp_sha256_hex(const uint8_t digest[32], char out_hex[65]);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_STORE_SHA256_H */
