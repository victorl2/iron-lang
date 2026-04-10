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
 *   3000..3999 tls      (reserved for Phase 60)
 *   4000..4999 json     (reserved for Phase 61)
 *   5000..5999 http     (reserved for Phase 62/63)
 *   6000..6999 ws       (reserved for Phase 64)
 *   7000..7999 internal (runtime invariant violations)
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
/* 1024..1099 reserved for future net additions */

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

/* ── TLS (3000..3999) — reserved for Phase 60 ────────────────────────────── */
/* ── JSON (4000..4999) — reserved for Phase 61 ───────────────────────────── */
/* ── HTTP (5000..5999) — reserved for Phase 62/63 ────────────────────────── */
/* ── WS (6000..6999) — reserved for Phase 64 ─────────────────────────────── */
/* ── Internal (7000..7999) — reserved for runtime invariant violations ───── */

#endif /* IRON_ERRORS_H */
