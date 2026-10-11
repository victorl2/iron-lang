#ifndef IRON_ERRORS_H
#define IRON_ERRORS_H

/* iron_errors.h — Canonical Iron error code partitioning (INFRA-10).
 *
 * Ranges are 1000 codes per subsystem. Allocate conservatively — once a code
 * ships it cannot be renumbered without breaking every downstream program.
 * Leave gaps for obvious future additions.
 *
 *   0          no error (iron_error_none())
 *   1..999     general runtime errors (parser, VM, etc.)
 *   1000..1999 net (TCP, UDP, IP, DNS)
 *   2000..2999 url
 *   3000..3999 tls
 *   4000..4999 json     (reserved for Phase 61)
 *   5000..5999 http
 *   6000..6999 ws
 *   7000..7999 internal (runtime invariant violations)
 *   8000..8999 file I/O
 *   9000..9999 crypto (9000..9099) and encoding (9100..9199)
 *
 * Each subsystem header comment documents which codes are assigned and which
 * are reserved; downstream plans append new codes inside the reserved bands.
 */

/* ── Net (1000..1999) ─────────────────────────────────────────────────────── */
#define IRON_ERR_NET_UNKNOWN           1000
#define IRON_ERR_NET_CONN_REFUSED      1001   /* ECONNREFUSED / WSAECONNREFUSED */
#define IRON_ERR_NET_CONN_RESET        1002   /* ECONNRESET / WSAECONNRESET */
#define IRON_ERR_NET_CONN_ABORTED      1003   /* ECONNABORTED / WSAECONNABORTED */
#define IRON_ERR_NET_TIMEOUT           1004   /* deadline-budget expiry */
#define IRON_ERR_NET_UNREACHABLE       1005   /* EHOSTUNREACH / ENETUNREACH */
#define IRON_ERR_NET_ADDR_IN_USE       1006   /* EADDRINUSE / WSAEADDRINUSE */
#define IRON_ERR_NET_ADDR_NOT_AVAIL    1007   /* EADDRNOTAVAIL */
#define IRON_ERR_NET_BAD_IP            1008   /* inet_pton returned 0 */
#define IRON_ERR_NET_BAD_HOST          1009   /* getaddrinfo EAI_NONAME / EAI_NODATA */
#define IRON_ERR_NET_DNS_TEMP_FAIL     1010   /* getaddrinfo EAI_AGAIN */
#define IRON_ERR_NET_DNS_FAIL          1011   /* getaddrinfo EAI_FAIL */
#define IRON_ERR_NET_DNS_OTHER         1012   /* other EAI_* */
#define IRON_ERR_NET_CLOSED            1013   /* operation on a closed socket */
#define IRON_ERR_NET_WOULD_BLOCK       1014   /* timeout:0 and operation not ready */
#define IRON_ERR_NET_BAD_FD            1015   /* EBADF / WSAENOTSOCK */
#define IRON_ERR_NET_PERMISSION        1016   /* EACCES / EPERM (privileged ports) */
#define IRON_ERR_NET_INTERRUPTED       1017   /* EINTR not already retried */
#define IRON_ERR_NET_MSG_TOO_LARGE     1018   /* EMSGSIZE — UDP-specific */
#define IRON_ERR_NET_NO_MEMORY         1019   /* ENOBUFS */
#define IRON_ERR_NET_PROTO             1020   /* EPROTO / EPROTOTYPE */
#define IRON_ERR_NET_AF_NOT_SUPPORTED  1021   /* EAFNOSUPPORT */
#define IRON_ERR_NET_NOT_CONNECTED     1022   /* ENOTCONN */
#define IRON_ERR_NET_IN_PROGRESS       1023   /* should never leak to user — internal use */
#define IRON_ERR_NET_INVALID_ARGUMENT  1024   /* negative/unsupported size or option */
/* 1025..1099 reserved for future net additions */

/* ── URL (2000..2999) ─────────────────────────────────────────────────────── */
#define IRON_ERR_URL_EMPTY             2000   /* empty input */
#define IRON_ERR_URL_NO_SCHEME         2001   /* missing scheme */
#define IRON_ERR_URL_BAD_SCHEME        2002   /* scheme contains invalid chars */
#define IRON_ERR_URL_BAD_HOST          2003   /* malformed host */
#define IRON_ERR_URL_BAD_IPV6          2004   /* unclosed bracket or invalid IPv6 literal */
#define IRON_ERR_URL_BAD_PORT          2005   /* port not integer or out of range */
#define IRON_ERR_URL_BAD_PATH          2006   /* path contains raw invalid char */
#define IRON_ERR_URL_BAD_PERCENT       2007   /* % not followed by two hex digits */
#define IRON_ERR_URL_BAD_UTF8          2008   /* decoded octets form invalid UTF-8 */
#define IRON_ERR_URL_TOO_LONG          2009   /* > 8 KB sanity cap */
/* 2010..2099 reserved for future URL additions */

/* ── TLS (3000..3999) ───────────────────────────────────────────────────── */
#define IRON_ERR_TLS_UNAVAILABLE        3000
#define IRON_ERR_TLS_CONTEXT            3001
#define IRON_ERR_TLS_TRUST_STORE        3002
#define IRON_ERR_TLS_HANDSHAKE          3003
#define IRON_ERR_TLS_VERIFY             3004
#define IRON_ERR_TLS_CERTIFICATE        3005
#define IRON_ERR_TLS_PRIVATE_KEY        3006
#define IRON_ERR_TLS_PROTOCOL           3007
#define IRON_ERR_TLS_CLOSED             3008
#define IRON_ERR_TLS_IO                 3009
#define IRON_ERR_TLS_INVALID_ARGUMENT   3010
/* 3011..3099 reserved for future TLS additions */
/* ── JSON (4000..4999) ──────────────────────────────────────────────────── */
#define IRON_ERR_JSON_SYNTAX       4000   /* malformed JSON text */
#define IRON_ERR_JSON_DEPTH        4001   /* nesting deeper than the limit */
#define IRON_ERR_JSON_TOO_LARGE    4002   /* input larger than the limit */
#define IRON_ERR_JSON_BAD_UTF8     4003   /* invalid UTF-8 in the input */
#define IRON_ERR_JSON_BAD_STRING   4004   /* bad escape, control char, lone surrogate */
#define IRON_ERR_JSON_BAD_NUMBER   4005   /* malformed or out-of-range number */
#define IRON_ERR_JSON_TYPE         4006   /* value has another type than asked */
#define IRON_ERR_JSON_MISSING      4007   /* key or index not present */
/* 4008..4099 reserved for future JSON additions */
/* ── Internal (7000..7999) — reserved for runtime invariant violations ───── */

#define IRON_ERR_HTTP_BAD_URL              5000
#define IRON_ERR_HTTP_UNSUPPORTED_SCHEME   5001
#define IRON_ERR_HTTP_MALFORMED_MESSAGE    5002
#define IRON_ERR_HTTP_HEADERS_TOO_LARGE    5003
#define IRON_ERR_HTTP_BODY_TOO_LARGE       5004
#define IRON_ERR_HTTP_BAD_CONTENT_LENGTH   5005
#define IRON_ERR_HTTP_TRUNCATED_MESSAGE    5006
#define IRON_ERR_HTTP_UNSUPPORTED_TRANSFER 5007
#define IRON_ERR_HTTP_INVALID_ARGUMENT     5008
#define IRON_ERR_HTTP_FILE                 5009
/* 5010..5099 reserved for future HTTP additions */

/* ── WebSocket (6000..6999) ─────────────────────────────────────────────── */
#define IRON_ERR_WS_BAD_URL                6000
#define IRON_ERR_WS_HANDSHAKE              6001
#define IRON_ERR_WS_PROTOCOL               6002
#define IRON_ERR_WS_MESSAGE_TOO_LARGE      6003
#define IRON_ERR_WS_INVALID_UTF8           6004
#define IRON_ERR_WS_CLOSED                 6005
#define IRON_ERR_WS_INVALID_ARGUMENT       6006
#define IRON_ERR_WS_NO_MEMORY              6007
/* 6008..6099 reserved for future WebSocket additions */

/* ── File I/O (8000..8999) ──────────────────────────────────────────────── */
#define IRON_ERR_IO_NOT_FOUND              8000
#define IRON_ERR_IO_PERMISSION             8001
#define IRON_ERR_IO_INVALID_ARGUMENT       8002
#define IRON_ERR_IO_TOO_LARGE              8003
#define IRON_ERR_IO_READ                   8004
#define IRON_ERR_IO_WRITE                  8005
#define IRON_ERR_IO_SEEK                   8006
#define IRON_ERR_IO_ALREADY_EXISTS         8007
#define IRON_ERR_IO_NOT_DIRECTORY          8008
#define IRON_ERR_IO_IS_DIRECTORY           8009
#define IRON_ERR_IO_NO_MEMORY              8010
#define IRON_ERR_IO_OTHER                  8011
/* 8012..8099 reserved for future file additions */

/* ── MySQL client (7000..7099) ──────────────────────────────────────────
 * Client-side failures of stdlib/mysql. Errors the server reports (ERR
 * packets) carry the server's own error number instead, with
 * MysqlError.server set; network and TLS failures keep their
 * IRON_ERR_NET_* / IRON_ERR_TLS_* codes. */
#define IRON_ERR_MYSQL_PROTOCOL            7000   /* malformed or unexpected packet */
#define IRON_ERR_MYSQL_AUTH                7001   /* unsupported auth plugin or exchange */
#define IRON_ERR_MYSQL_INVALID_ARGUMENT    7002
#define IRON_ERR_MYSQL_PACKET_TOO_LARGE    7003   /* over max_allowed_packet */
#define IRON_ERR_MYSQL_CLOSED              7004   /* connection closed or unusable */
#define IRON_ERR_MYSQL_NULL                7005   /* typed read of an SQL NULL */
#define IRON_ERR_MYSQL_CONVERSION          7006   /* value does not convert to the type */
#define IRON_ERR_MYSQL_RANGE               7007   /* row/column index or name not found */
#define IRON_ERR_MYSQL_TLS_REQUIRED        7008   /* server does not offer TLS */
#define IRON_ERR_MYSQL_POOL_TIMEOUT        7009   /* no pooled connection in time */
#define IRON_ERR_MYSQL_POOL_CLOSED         7010
#define IRON_ERR_MYSQL_PARAM_COUNT         7011   /* placeholders vs parameters */
#define IRON_ERR_MYSQL_BAD_URL             7012
#define IRON_ERR_MYSQL_INSECURE_AUTH       7013   /* password exchange needs TLS or an RSA key */
#define IRON_ERR_MYSQL_NO_MEMORY           7014
#define IRON_ERR_MYSQL_STMT_CLOSED         7015   /* statement closed or its connection released */
#define IRON_ERR_MYSQL_RESULT_TOO_LARGE    7016
#define IRON_ERR_MYSQL_LOCAL_INFILE        7017   /* LOAD DATA LOCAL is refused */
#define IRON_ERR_MYSQL_UNAVAILABLE         7018   /* built without OpenSSL */
/* 7019..7099 reserved for future MySQL client additions */
/* ── Crypto (9000..9099) ────────────────────────────────────────────────── */
#define IRON_ERR_CRYPTO_UNAVAILABLE        9000   /* built without OpenSSL 3 */
#define IRON_ERR_CRYPTO_INVALID_ARGUMENT   9001
#define IRON_ERR_CRYPTO_INVALID_KEY        9002   /* wrong key length, unparsable key */
#define IRON_ERR_CRYPTO_INVALID_NONCE      9003   /* wrong nonce length */
#define IRON_ERR_CRYPTO_AUTHENTICATION     9004   /* AEAD tag mismatch: forged or corrupt */
#define IRON_ERR_CRYPTO_INVALID_LENGTH     9005   /* output length out of range */
#define IRON_ERR_CRYPTO_INVALID_PARAMETERS 9006   /* KDF cost parameters */
#define IRON_ERR_CRYPTO_MALFORMED_HASH     9007   /* not a password hash string */
#define IRON_ERR_CRYPTO_INTERNAL           9008   /* libcrypto reported a failure */
/* 9009..9099 reserved for future crypto additions */

/* ── Encoding (9100..9199) ──────────────────────────────────────────────── */
#define IRON_ERR_ENCODING_INVALID_CHARACTER 9100
#define IRON_ERR_ENCODING_INVALID_LENGTH    9101
#define IRON_ERR_ENCODING_INVALID_PADDING   9102
/* 9103..9199 reserved for future encoding additions */

#endif /* IRON_ERRORS_H */
