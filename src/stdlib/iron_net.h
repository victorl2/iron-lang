#ifndef IRON_NET_H
#define IRON_NET_H

/* iron_net.h — Phase 59 P02/P03 TCP + UDP + IP stdlib surface.
 *
 * This header is consumed by:
 *   - src/stdlib/iron_net.c (the C implementation)
 *   - tests/unit/test_stdlib_net_tcp.c (the P02 TCP Unity tests)
 *   - tests/unit/test_stdlib_net_udp.c (the P03 UDP Unity tests)
 *   - tests/unit/test_stdlib_net_ip.c  (the P03 IP address Unity tests)
 *
 * It is intentionally NOT included by the Iron codegen output. Generated
 * C files receive extern prototypes for the stub functions through the
 * emit_c.c Phase 3 "is_extern && !extern_c_name" branch, which lands the
 * prototypes in ctx.prototypes AFTER the compiler's own struct body
 * emission — avoiding the double-definition conflict that arises when
 * both the header and the compiler declare struct Iron_NetError etc.
 *
 * All blocking operations take an Int millisecond timeout and honour it
 * via Iron_Deadline (P01a) + iron_net_poll (non-blocking socket +
 * poll/WSAPoll). Error codes come from iron_errors.h (P01a).
 *
 * Phase 59 P03 addendum: UDP socket primitives (Iron_UdpSocket), typed
 * IPv4/IPv6 address types, and IP parse/format wrappers. See iron_net.c
 * for the zone-identifier handling and dual-stack bind policy. The Iron
 * frontend has no `variant` keyword, so the Iron-side enum lives in
 * stdlib/net.iron as `enum Address { V4(IPv4Addr), V6(IPv6Addr) }` — the
 * C side exposes flat v4/v6 sendto helpers that the Iron wrapper
 * dispatches into via `match`.
 */

#include <stdint.h>
#include <stdbool.h>
#include "runtime/iron_runtime.h"
#include "runtime/iron_errors.h"

/* ── Object types — must stay layout-compatible with the Iron-side
 * `object NetError { val code: Int }`,
 * `object TcpSocket { val fd: Int }`,
 * `object TcpListener { val fd: Int }`
 * declared in src/stdlib/net.iron. */
typedef struct Iron_NetError    { int64_t code; } Iron_NetError;
typedef struct Iron_TcpSocket   { int64_t fd;   } Iron_TcpSocket;
typedef struct Iron_TcpListener { int64_t fd;   } Iron_TcpListener;

/* ── Tuple typedefs — layout-compatible with emit_ensure_tuple output
 * for (TcpSocket, NetError), (TcpListener, NetError), (Int, NetError). */
typedef struct {
    Iron_TcpSocket v0;
    Iron_NetError  v1;
} Iron_Tuple_TcpSocket_NetError;

typedef struct {
    Iron_TcpListener v0;
    Iron_NetError    v1;
} Iron_Tuple_TcpListener_NetError;

typedef struct {
    int64_t       v0;
    Iron_NetError v1;
} Iron_Tuple_Int_NetError;

/* Convenience aliases for the test file and impl. */
typedef Iron_Tuple_TcpSocket_NetError   Iron_Result_TcpSocket_Error;
typedef Iron_Tuple_TcpListener_NetError Iron_Result_TcpListener_Error;
typedef Iron_Tuple_Int_NetError         Iron_Result_Int_Error;

/* ── C wrapper functions.
 *
 * The Iron compiler lowers `Net.tcp_dial(...)` etc. via hir_to_lir's
 * method-call mangling (lowercase type name) to `Iron_net_tcp_dial(...)`.
 * The symbols below must spell `Iron_net_*` / `Iron_tcpsocket_*` /
 * `Iron_tcplistener_*` exactly. */

Iron_Result_TcpSocket_Error   Iron_net_tcp_dial(Iron_String host, int64_t port, int64_t timeout);
Iron_Result_TcpListener_Error Iron_net_tcp_listen(Iron_String host, int64_t port);

Iron_Result_TcpSocket_Error Iron_tcplistener_accept(Iron_TcpListener l, int64_t timeout);
void                         Iron_tcplistener_close(Iron_TcpListener l);

Iron_Result_Int_Error Iron_tcpsocket_read (Iron_TcpSocket s, Iron_String buf, int64_t timeout);
Iron_Result_Int_Error Iron_tcpsocket_write(Iron_TcpSocket s, Iron_String buf, int64_t timeout);
void                  Iron_tcpsocket_close(Iron_TcpSocket s);

/* Raw work-engine declarations used by the Unity tests. These funnel to
 * the same recv()/send() path as the Iron_String-facing wrappers. */
Iron_Result_Int_Error Iron_net_tcp_recv_bytes(Iron_TcpSocket s,
                                                uint8_t *buf,
                                                int64_t cap,
                                                int64_t timeout);
Iron_Result_Int_Error Iron_net_tcp_send_bytes(Iron_TcpSocket s,
                                                const uint8_t *buf,
                                                int64_t len,
                                                int64_t timeout);

/* ── Capital-first aliases used by the Unity tests so callers can spell
 * Iron_Net_tcp_listen_result() etc. in the Iron_io_read_file_result style
 * the plan's "contains" references expect. These are static-inline
 * forwarders. */
static inline Iron_Result_TcpSocket_Error
Iron_Net_tcp_dial_result(Iron_String host, int64_t port, int64_t timeout) {
    return Iron_net_tcp_dial(host, port, timeout);
}
static inline Iron_Result_TcpListener_Error
Iron_Net_tcp_listen_result(Iron_String host, int64_t port) {
    return Iron_net_tcp_listen(host, port);
}
static inline Iron_Result_TcpSocket_Error
Iron_TcpListener_accept_result(Iron_TcpListener l, int64_t timeout) {
    return Iron_tcplistener_accept(l, timeout);
}
static inline Iron_Result_Int_Error
Iron_TcpSocket_read_result(Iron_TcpSocket s, uint8_t *buf, int64_t cap, int64_t timeout) {
    return Iron_net_tcp_recv_bytes(s, buf, cap, timeout);
}
static inline Iron_Result_Int_Error
Iron_TcpSocket_write_result(Iron_TcpSocket s, const uint8_t *buf, int64_t len, int64_t timeout) {
    return Iron_net_tcp_send_bytes(s, buf, len, timeout);
}
static inline void Iron_TcpSocket_close(Iron_TcpSocket s) {
    Iron_tcpsocket_close(s);
}
static inline void Iron_TcpListener_close(Iron_TcpListener l) {
    Iron_tcplistener_close(l);
}

/* ── Phase 59 P03: UDP + IP address types ─────────────────────────────── */

/* UDP socket handle — layout-compatible with Iron `object UdpSocket { val fd: Int }` */
typedef struct Iron_UdpSocket { int64_t fd; } Iron_UdpSocket;

/* IPv4 address — LAYOUT-COMPATIBLE with compiler-emitted
 * `struct Iron_IPv4Addr { int64_t a, b, c, d; }`.
 *
 * Rationale: the Iron-side `object IPv4Addr { val a, b, c, d: Int }` is
 * the canonical layout because it flows through Iron functions by value
 * and through the Iron tuple-return code path. The C side must match
 * byte-for-byte — same struct, same order, same int64_t fields. The C
 * wrappers (inet_pton/inet_ntop helpers) convert between the 4 int64
 * octet-in-low-byte representation and the contiguous uint8_t[4] that
 * libc expects. */
typedef struct Iron_IPv4Addr {
    int64_t a;
    int64_t b;
    int64_t c;
    int64_t d;
} Iron_IPv4Addr;

/* IPv6 address — LAYOUT-COMPATIBLE with compiler-emitted
 * `struct Iron_IPv6Addr { Iron_String bytes; Iron_String zone; }`.
 *
 * `bytes` is ALWAYS a 16-byte Iron_String payload containing the raw
 * octets (not a hex/canonical string — real bytes). `zone` is the
 * optional zone identifier (empty Iron_String when absent). The C
 * wrappers call iron_string_cstr(&bytes) to get a pointer into the
 * 16-byte payload for inet_ntop. */
typedef struct Iron_IPv6Addr {
    Iron_String bytes;
    Iron_String zone;
} Iron_IPv6Addr;

/* ── Result tuples for UDP and IP wrappers ────────────────────────────── */
typedef struct {
    Iron_UdpSocket v0;
    Iron_NetError  v1;
} Iron_Tuple_UdpSocket_NetError;

typedef struct {
    Iron_IPv4Addr v0;
    Iron_NetError v1;
} Iron_Tuple_IPv4Addr_NetError;

typedef struct {
    Iron_IPv6Addr v0;
    Iron_NetError v1;
} Iron_Tuple_IPv6Addr_NetError;

typedef Iron_Tuple_UdpSocket_NetError Iron_Result_UdpSocket_NetError;
typedef Iron_Tuple_IPv4Addr_NetError  Iron_Result_IPv4Addr_NetError;
typedef Iron_Tuple_IPv6Addr_NetError  Iron_Result_IPv6Addr_NetError;
typedef Iron_Tuple_Int_NetError       Iron_Result_Int_NetError;  /* shared with TCP */

/* ── UDP wrappers ──────────────────────────────────────────────────────── */
Iron_Result_UdpSocket_NetError Iron_net_udp_bind(Iron_String host, int64_t port);

/* Flat v4 / v6 sendto helpers. The Iron-side `UdpSocket.sendto(addr: Address, ...)`
 * stub pattern-matches and dispatches into one of these. */
Iron_Result_Int_NetError Iron_net_udp_sendto_v4(Iron_UdpSocket s,
                                                 Iron_String    buf,
                                                 Iron_IPv4Addr  addr,
                                                 int64_t        port,
                                                 int64_t        timeout);

Iron_Result_Int_NetError Iron_net_udp_sendto_v6(Iron_UdpSocket s,
                                                 Iron_String    buf,
                                                 Iron_IPv6Addr  addr,
                                                 int64_t        port,
                                                 int64_t        timeout);

/* recvfrom returns a flat struct-by-value descriptor. The Iron-side stub
 * exposes this as `object UdpRecv { nbytes, family, bytes, zone, port, err }`
 * so user Iron code can access fields with `r.nbytes` etc. */
typedef struct {
    int64_t       nbytes;
    int64_t       addr_family;  /* 4 = AF_INET, 6 = AF_INET6, 0 on error */
    Iron_String   addr_bytes;   /* 4 or 16 raw octets */
    Iron_String   addr_zone;    /* v6 zone or "" */
    int64_t       port;
    Iron_NetError err;
} Iron_UdpRecvResult;

Iron_UdpRecvResult Iron_udpsocket_recvfrom(Iron_UdpSocket s,
                                             uint8_t       *buf,
                                             int64_t        cap,
                                             int64_t        timeout);

void Iron_udpsocket_close(Iron_UdpSocket s);

/* ── IP address parse/format ───────────────────────────────────────────── */
/* Names match the hir_to_lir method-call mangling so the generated C
 * can call them as-is. `Iron_ipv4addr_parse` is the mangled form of
 * `IPv4Addr.parse` produced by hir_to_lir.c. */
Iron_Result_IPv4Addr_NetError Iron_ipv4addr_parse(Iron_String s);
Iron_String                    Iron_ipv4addr_format(Iron_IPv4Addr a);
Iron_Result_IPv6Addr_NetError Iron_ipv6addr_parse(Iron_String s);
Iron_String                    Iron_ipv6addr_format(Iron_IPv6Addr a);

/* ── Phase 59 P04: DNS lookup_host ────────────────────────────────────────
 *
 * Iron-side signature:
 *   func Net.lookup_host(name: String, timeout: Int) -> ([Address], NetError)
 *
 * The `Address` type is an Iron ADT: `enum Address { V4(IPv4Addr), V6(IPv6Addr) }`.
 * The compiler emits:
 *   typedef enum { Iron_Address_TAG_V4 = 0, Iron_Address_TAG_V6 = 1 } Iron_Address_Tag;
 *   struct Iron_Address { Iron_Address_Tag tag; Iron_Address_data_t data; };
 *
 * Forward-declared here with the IRON_ADDRESS_STRUCT_DEFINED guard. The
 * Unity tests include this header alongside iron_runtime.h and never see
 * the compiler-emitted version, so the forward decl + a minimal
 * tag-payload mirror is sufficient for the tests to inspect .tag. */
#ifndef IRON_ADDRESS_STRUCT_DEFINED
#define IRON_ADDRESS_STRUCT_DEFINED
typedef enum {
    Iron_Address_TAG_V4 = 0,
    Iron_Address_TAG_V6 = 1
} Iron_Address_Tag;

typedef struct { Iron_IPv4Addr _0; } Iron_Address_V4_data;
typedef struct { Iron_IPv6Addr _0; } Iron_Address_V6_data;

typedef union {
    char                  _dummy;
    Iron_Address_V4_data  V4;
    Iron_Address_V6_data  V6;
} Iron_Address_data_t;

typedef struct Iron_Address {
    Iron_Address_Tag    tag;
    Iron_Address_data_t data;
} Iron_Address;
#endif /* IRON_ADDRESS_STRUCT_DEFINED */

/* Iron's [Address] lowers to Iron_List_Iron_Address in C. This type is
 * layout-compatible with the compiler-emitted IRON_LIST_DECL expansion
 * (items/count/capacity). The Unity tests construct lists through the
 * C wrapper and free items via plain `free()` — they never touch the
 * compiler-emitted list helpers. */
#ifndef IRON_LIST_IRON_ADDRESS_STRUCT_DEFINED
#define IRON_LIST_IRON_ADDRESS_STRUCT_DEFINED
typedef struct Iron_List_Iron_Address {
    Iron_Address *items;
    int64_t       count;
    int64_t       capacity;
} Iron_List_Iron_Address;
#endif

/* The tuple type name MUST match what the Iron compiler emits for
 * `([Address], NetError)` through tuple_build_mangled_name in
 * analyzer/types.c. The tuple mangling algorithm runs iron_type_to_string
 * on each element then sanitizes non-identifier characters to `_`:
 *
 *   [Address]  →  "[Address]"  →  "_Address_"
 *   NetError   →  "NetError"
 *
 * Final:  Iron_Tuple + _ + _Address_ + _ + NetError
 *       = Iron_Tuple__Address__NetError
 *
 * iron_net.c, emit_c.c, and the Unity tests must all reference this
 * exact name or the C linker will see two distinct struct types with
 * compatible layouts but incompatible names. */
#ifndef IRON_TUPLE_ADDRESS_LIST_NETERROR_DEFINED
#define IRON_TUPLE_ADDRESS_LIST_NETERROR_DEFINED
typedef struct {
    Iron_List_Iron_Address v0;
    Iron_NetError          v1;
} Iron_Tuple__Address__NetError;
#endif

typedef Iron_Tuple__Address__NetError Iron_Result_AddressList_NetError;

/* Iron_net_lookup_host — the C impl invoked from the Iron side via
 * hir_to_lir method-call mangling (Net.lookup_host → Iron_net_lookup_host).
 * The capital-first `Iron_Net_lookup_host_result` alias below is the
 * Unity-test-facing spelling. */
Iron_Tuple__Address__NetError Iron_net_lookup_host(Iron_String name, int64_t timeout_ms);

static inline Iron_Result_AddressList_NetError
Iron_Net_lookup_host_result(Iron_String name, int64_t timeout_ms) {
    return Iron_net_lookup_host(name, timeout_ms);
}

/* ── Capital-first aliases for the Unity tests ─────────────────────────── */
static inline Iron_Result_UdpSocket_NetError
Iron_Net_udp_bind_result(Iron_String host, int64_t port) {
    return Iron_net_udp_bind(host, port);
}
static inline Iron_Result_Int_NetError
Iron_Net_udp_sendto_v4_result(Iron_UdpSocket s, Iron_String buf,
                                Iron_IPv4Addr addr, int64_t port, int64_t timeout) {
    return Iron_net_udp_sendto_v4(s, buf, addr, port, timeout);
}
static inline Iron_Result_Int_NetError
Iron_Net_udp_sendto_v6_result(Iron_UdpSocket s, Iron_String buf,
                                Iron_IPv6Addr addr, int64_t port, int64_t timeout) {
    return Iron_net_udp_sendto_v6(s, buf, addr, port, timeout);
}
static inline Iron_UdpRecvResult
Iron_UdpSocket_recvfrom_result(Iron_UdpSocket s, uint8_t *buf,
                                 int64_t cap, int64_t timeout) {
    return Iron_udpsocket_recvfrom(s, buf, cap, timeout);
}
static inline void Iron_UdpSocket_close(Iron_UdpSocket s) {
    Iron_udpsocket_close(s);
}
static inline Iron_Result_IPv4Addr_NetError
Iron_Net_ipv4addr_parse_result(Iron_String s) {
    return Iron_ipv4addr_parse(s);
}
static inline Iron_String
Iron_Net_ipv4addr_format(Iron_IPv4Addr a) {
    return Iron_ipv4addr_format(a);
}
static inline Iron_Result_IPv6Addr_NetError
Iron_Net_ipv6addr_parse_result(Iron_String s) {
    return Iron_ipv6addr_parse(s);
}
static inline Iron_String
Iron_Net_ipv6addr_format(Iron_IPv6Addr a) {
    return Iron_ipv6addr_format(a);
}

#endif /* IRON_NET_H */
