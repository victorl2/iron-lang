#ifndef IRON_ENCODING_H
#define IRON_ENCODING_H

/* iron_encoding.h: hex and base64 codecs behind src/stdlib/encoding.iron.
 *
 * Binary data is an Iron String (binary safe, as in IO.read_bytes and
 * TcpSocket.read). Encoders cannot fail; decoders report malformed input in
 * a DecodeResult with an IRON_ERR_ENCODING_* code instead of panicking.
 *
 * Not included by generated C: the compiler emits its own prototypes for the
 * stubs and its own struct for DecodeResult, which must match this layout. */

#include <stdint.h>
#include <stdbool.h>
#include "runtime/iron_runtime.h"

typedef struct Iron_DecodeResult {
    Iron_String data;
    int64_t error;
    Iron_String error_message;
} Iron_DecodeResult;

Iron_String       Iron_hex_encode(Iron_String data);
Iron_DecodeResult Iron_hex_decode(Iron_String text);
Iron_String       Iron_base64_encode(Iron_String data);
Iron_DecodeResult Iron_base64_decode(Iron_String text);
Iron_String       Iron_base64_url_encode(Iron_String data);
Iron_DecodeResult Iron_base64_url_decode(Iron_String text);

/* The codecs on raw buffers, shared with iron_crypto.c (password hash
 * strings, tokens). Base64 output carries padding when `pad`; the decoder
 * takes the standard (url = false) or url-safe alphabet and requires
 * padding when `require_pad` (otherwise it is optional). Decoding returns
 * 0 or an IRON_ERR_ENCODING_* code and the offending byte offset. */
size_t  iron_base64_encoded_len(size_t len, bool pad);
void    iron_base64_encode_raw(const uint8_t *in, size_t len, char *out, bool url, bool pad);
int64_t iron_base64_decode_raw(const char *in, size_t len, bool url, bool require_pad,
                               uint8_t *out, size_t *out_len, size_t *bad_at);

void    Iron_decoderesult_release(Iron_DecodeResult result);

#endif /* IRON_ENCODING_H */
