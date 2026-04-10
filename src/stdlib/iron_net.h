#ifndef IRON_NET_H
#define IRON_NET_H

/* iron_net.h — Phase 59 P02 TCP stdlib surface.
 *
 * This header is consumed by:
 *   - src/stdlib/iron_net.c (the C implementation)
 *   - tests/unit/test_stdlib_net_tcp.c (the Unity tests)
 *
 * It is intentionally NOT included by the Iron codegen output. Generated
 * C files receive extern prototypes for the TCP stub functions through
 * the emit_c.c Phase 3 "is_extern && !extern_c_name" branch, which lands
 * the prototypes in ctx.prototypes AFTER the compiler's own struct body
 * emission — avoiding the double-definition conflict that arises when
 * both the header and the compiler declare struct Iron_NetError etc.
 *
 * All blocking operations take an Int millisecond timeout and honour it
 * via Iron_Deadline (P01a) + iron_net_poll (non-blocking socket +
 * poll/WSAPoll). Error codes come from iron_errors.h (P01a).
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

#endif /* IRON_NET_H */
