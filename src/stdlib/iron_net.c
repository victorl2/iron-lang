/* iron_net.c — Phase 59 P02 TCP stdlib implementation.
 *
 * Cross-platform non-blocking TCP wrappers using a single-budget Iron_Deadline
 * (P01a) to thread timeouts through connect/accept/read/write. Errors are
 * translated to IRON_ERR_NET_* (P01a) via static-literal messages — no heap,
 * no snprintf, no thread-local buffers on the error path.
 *
 * Per the 2026-04-10 CONTEXT.md revision and the 59-01d tuple-codegen fix,
 * Iron-side APIs take `timeout: Int` (milliseconds) and return tuples of the
 * form (Handle, NetError). The C-side wrapper names must match the
 * compiler's method-call lowering:
 *     Net.tcp_dial         → Iron_net_tcp_dial
 *     Net.tcp_listen       → Iron_net_tcp_listen
 *     TcpListener.accept   → Iron_tcplistener_accept
 *     TcpListener.close    → Iron_tcplistener_close
 *     TcpSocket.read       → Iron_tcpsocket_read
 *     TcpSocket.write      → Iron_tcpsocket_write
 *     TcpSocket.close      → Iron_tcpsocket_close
 *
 * Platform notes:
 *   - POSIX uses poll(), recv/send, fcntl(O_NONBLOCK), errno translation.
 *   - Windows uses WSAPoll(), recv/send, ioctlsocket(FIONBIO),
 *     WSAGetLastError() translation. SIGPIPE does not apply on Windows.
 */

#include "iron_net.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifndef POLLIN
    #define POLLIN  0x0100
  #endif
  #ifndef POLLOUT
    #define POLLOUT 0x0010
  #endif
  typedef SOCKET iron_sock_t;
  #define IRON_INVALID_SOCK INVALID_SOCKET
  #define IRON_NET_LAST_ERR()   WSAGetLastError()
  #define IRON_NET_EWOULDBLOCK  WSAEWOULDBLOCK
  #define IRON_NET_EINPROGRESS  WSAEWOULDBLOCK
  #define IRON_NET_EAGAIN       WSAEWOULDBLOCK
  #define IRON_NET_EINTR        WSAEINTR
  #define IRON_NET_CLOSE(fd)    closesocket((SOCKET)(fd))
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <net/if.h>   /* if_nametoindex / if_indextoname for IPv6 zones */
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <errno.h>
  #include <stdio.h>    /* snprintf for P03 udp_bind port_buf */
  typedef int iron_sock_t;
  #define IRON_INVALID_SOCK (-1)
  #define IRON_NET_LAST_ERR()  errno
  #define IRON_NET_EWOULDBLOCK EWOULDBLOCK
  #define IRON_NET_EINPROGRESS EINPROGRESS
  #define IRON_NET_EAGAIN      EAGAIN
  #define IRON_NET_EINTR       EINTR
  #define IRON_NET_CLOSE(fd)   close((int)(fd))
#endif

/* ── Error translation ──────────────────────────────────────────────────
 *
 * Every platform error code maps to one of the IRON_ERR_NET_* constants
 * from iron_errors.h. Messages are static string literals so the hot
 * path never allocates. */

#ifdef _WIN32
static int iron_net_translate_wsa(int e) {
    switch (e) {
        case WSAECONNREFUSED:  return IRON_ERR_NET_CONN_REFUSED;
        case WSAECONNRESET:    return IRON_ERR_NET_CONN_RESET;
        case WSAECONNABORTED:  return IRON_ERR_NET_CONN_ABORTED;
        case WSAETIMEDOUT:     return IRON_ERR_NET_TIMEOUT;
        case WSAEHOSTUNREACH:
        case WSAENETUNREACH:   return IRON_ERR_NET_UNREACHABLE;
        case WSAEADDRINUSE:    return IRON_ERR_NET_ADDR_IN_USE;
        case WSAEADDRNOTAVAIL: return IRON_ERR_NET_ADDR_NOT_AVAIL;
        case WSAESHUTDOWN:     return IRON_ERR_NET_CLOSED;
        case WSAEWOULDBLOCK:   return IRON_ERR_NET_WOULD_BLOCK;
        case WSAEBADF:
        case WSAENOTSOCK:      return IRON_ERR_NET_BAD_FD;
        case WSAEACCES:        return IRON_ERR_NET_PERMISSION;
        case WSAEINTR:         return IRON_ERR_NET_UNKNOWN;
        case WSAENOTCONN:      return IRON_ERR_NET_NOT_CONNECTED;
        case WSAEINPROGRESS:
        case WSAEALREADY:      return IRON_ERR_NET_IN_PROGRESS;
        case 0:                return 0;
        default:               return IRON_ERR_NET_UNKNOWN;
    }
}
#else
static int iron_net_translate_errno(int e) {
    switch (e) {
        case ECONNREFUSED: return IRON_ERR_NET_CONN_REFUSED;
        case ECONNRESET:   return IRON_ERR_NET_CONN_RESET;
        case ECONNABORTED: return IRON_ERR_NET_CONN_ABORTED;
        case ETIMEDOUT:    return IRON_ERR_NET_TIMEOUT;
        case EHOSTUNREACH:
        case ENETUNREACH:  return IRON_ERR_NET_UNREACHABLE;
        case EADDRINUSE:   return IRON_ERR_NET_ADDR_IN_USE;
        case EADDRNOTAVAIL: return IRON_ERR_NET_ADDR_NOT_AVAIL;
        case EPIPE:        return IRON_ERR_NET_CONN_RESET;
        case ESHUTDOWN:    return IRON_ERR_NET_CLOSED;
        case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
        case EAGAIN:
#endif
                           return IRON_ERR_NET_WOULD_BLOCK;
        case EBADF:
        case ENOTSOCK:     return IRON_ERR_NET_BAD_FD;
        case EACCES:
        case EPERM:        return IRON_ERR_NET_PERMISSION;
        case EINTR:        return IRON_ERR_NET_UNKNOWN;
        case ENOTCONN:     return IRON_ERR_NET_NOT_CONNECTED;
        case EINPROGRESS:
        case EALREADY:     return IRON_ERR_NET_IN_PROGRESS;
        case 0:            return 0;
        default:           return IRON_ERR_NET_UNKNOWN;
    }
}
#endif

static Iron_NetError iron_net_err_code(int code) {
    Iron_NetError e;
    e.code = code;
    return e;
}

static Iron_NetError iron_net_err_from_last(void) {
#ifdef _WIN32
    return iron_net_err_code(iron_net_translate_wsa(IRON_NET_LAST_ERR()));
#else
    return iron_net_err_code(iron_net_translate_errno(IRON_NET_LAST_ERR()));
#endif
}

static Iron_NetError iron_net_err_none(void) {
    return iron_net_err_code(0);
}

static Iron_NetError iron_net_err_timeout(void) {
    return iron_net_err_code(IRON_ERR_NET_TIMEOUT);
}

/* ── Non-blocking mode toggle ───────────────────────────────────────── */

static int iron_net_set_nonblocking(int64_t fd) {
    if (fd < 0) return -1;
#ifdef _WIN32
    u_long mode = 1;
    return (ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0) ? 0 : -1;
#else
    int flags = fcntl((int)fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl((int)fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
#endif
}

/* ── Cross-platform poll wrapper ────────────────────────────────────────
 *
 * Returns >0 on ready, 0 on timeout, -1 on error (caller inspects
 * IRON_NET_LAST_ERR()). POSIX retries on EINTR; Windows WSAPoll returns
 * directly. A timeout_ms of 0 is "poll once / return immediately". */

static int iron_net_poll(int64_t fd, short events, int timeout_ms) {
    if (fd < 0) return -1;
#ifdef _WIN32
    WSAPOLLFD pfd;
    pfd.fd      = (SOCKET)fd;
    pfd.events  = events;
    pfd.revents = 0;
    int rc = WSAPoll(&pfd, 1, timeout_ms);
    return rc;
#else
    struct pollfd pfd;
    pfd.fd      = (int)fd;
    pfd.events  = events;
    pfd.revents = 0;
    for (;;) {
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0 && errno == EINTR) continue;
        return rc;
    }
#endif
}

/* ── Iron_Net_tcp_listen_result equivalent ───────────────────────────── */

/* Build a service string from an integer port so getaddrinfo(AI_NUMERICSERV)
 * can resolve it without DNS. */
static void port_to_service(int64_t port, char *out, size_t cap) {
    if (port < 0) port = 0;
    if (port > 65535) port = 65535;
    /* No snprintf on the hot path: do a manual unsigned-int→string. */
    char buf[8];
    int len = 0;
    if (port == 0) {
        buf[len++] = '0';
    } else {
        uint32_t v = (uint32_t)port;
        while (v > 0 && len < 8) {
            buf[len++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    size_t w = 0;
    while (len > 0 && w + 1 < cap) {
        out[w++] = buf[--len];
    }
    out[w] = '\0';
}

Iron_Result_TcpListener_Error Iron_net_tcp_listen(Iron_String host, int64_t port) {
    Iron_Result_TcpListener_Error out;
    out.v0.fd = -1;
    out.v1    = iron_net_err_none();

    const char *host_c = iron_string_cstr(&host);
    if (!host_c) host_c = "";

    char service[8];
    port_to_service(port, service, sizeof(service));

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE | AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host_c[0] ? host_c : NULL, service, &hints, &res);
    if (gai != 0 || !res) {
        out.v1 = iron_net_err_code(IRON_ERR_NET_ADDR_NOT_AVAIL);
        return out;
    }

    iron_sock_t listen_fd = IRON_INVALID_SOCK;
    int last_err = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        iron_sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == IRON_INVALID_SOCK) { last_err = IRON_NET_LAST_ERR(); continue; }

        /* INFRA-08: when the user asked for "::" explicitly, put the
         * listener in dual-stack mode. The setsockopt is a no-op on
         * AF_INET sockets so the check on ai_family is the gate. */
        if (ai->ai_family == AF_INET6) {
            int v6only = 0;
            setsockopt((iron_sock_t)s, IPPROTO_IPV6, IPV6_V6ONLY,
                       (const char*)&v6only, sizeof(v6only));
        }

        int one = 1;
        setsockopt((iron_sock_t)s, SOL_SOCKET, SO_REUSEADDR,
                   (const char*)&one, sizeof(one));

        if (bind(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) != 0) {
            last_err = IRON_NET_LAST_ERR();
            IRON_NET_CLOSE((int64_t)s);
            continue;
        }
        if (listen(s, SOMAXCONN) != 0) {
            last_err = IRON_NET_LAST_ERR();
            IRON_NET_CLOSE((int64_t)s);
            continue;
        }
        /* Make the listener non-blocking so accept-with-timeout works. */
        iron_net_set_nonblocking((int64_t)s);

        listen_fd = s;
        break;
    }
    freeaddrinfo(res);

    if (listen_fd == IRON_INVALID_SOCK) {
        if (last_err == 0) {
            out.v1 = iron_net_err_code(IRON_ERR_NET_UNKNOWN);
        } else {
#ifdef _WIN32
            out.v1 = iron_net_err_code(iron_net_translate_wsa(last_err));
#else
            out.v1 = iron_net_err_code(iron_net_translate_errno(last_err));
#endif
        }
        return out;
    }

    out.v0.fd = (int64_t)listen_fd;
    out.v1    = iron_net_err_none();
    return out;
}

/* ── Iron_Net_tcp_dial — non-blocking connect with deadline ──────────── */

Iron_Result_TcpSocket_Error Iron_net_tcp_dial(Iron_String host, int64_t port, int64_t timeout) {
    Iron_Result_TcpSocket_Error out;
    out.v0.fd = -1;
    out.v1    = iron_net_err_none();

    const char *host_c = iron_string_cstr(&host);
    if (!host_c) host_c = "";

    char service[8];
    port_to_service(port, service, sizeof(service));

    Iron_Deadline dl = Iron_deadline_from_timeout_ms(timeout);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host_c, service, &hints, &res);
    if (gai != 0 || !res) {
        out.v1 = iron_net_err_code(IRON_ERR_NET_ADDR_NOT_AVAIL);
        return out;
    }

    int  last_translated = 0;
    bool any_timeout     = false;

    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        iron_sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == IRON_INVALID_SOCK) {
#ifdef _WIN32
            last_translated = iron_net_translate_wsa(IRON_NET_LAST_ERR());
#else
            last_translated = iron_net_translate_errno(IRON_NET_LAST_ERR());
#endif
            continue;
        }
        if (iron_net_set_nonblocking((int64_t)s) != 0) {
            IRON_NET_CLOSE((int64_t)s);
            continue;
        }

        int rc = connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen);
        if (rc == 0) {
            out.v0.fd = (int64_t)s;
            out.v1    = iron_net_err_none();
            freeaddrinfo(res);
            return out;
        }
        int cerr = IRON_NET_LAST_ERR();
        if (cerr != IRON_NET_EINPROGRESS && cerr != IRON_NET_EWOULDBLOCK) {
#ifdef _WIN32
            last_translated = iron_net_translate_wsa(cerr);
#else
            last_translated = iron_net_translate_errno(cerr);
#endif
            IRON_NET_CLOSE((int64_t)s);
            continue;
        }

        /* In-progress — wait for POLLOUT up to the remaining budget. */
        int rem = Iron_deadline_remaining_ms(dl);
        if (rem <= 0) {
            IRON_NET_CLOSE((int64_t)s);
            any_timeout = true;
            continue;
        }
        int pr = iron_net_poll((int64_t)s, POLLOUT, rem);
        if (pr == 0) {
            IRON_NET_CLOSE((int64_t)s);
            any_timeout = true;
            continue;
        }
        if (pr < 0) {
#ifdef _WIN32
            last_translated = iron_net_translate_wsa(IRON_NET_LAST_ERR());
#else
            last_translated = iron_net_translate_errno(IRON_NET_LAST_ERR());
#endif
            IRON_NET_CLOSE((int64_t)s);
            continue;
        }

        /* Pitfall 1: check SO_ERROR to distinguish real success from
         * async failure. */
        int so_err = 0;
        socklen_t so_len = sizeof(so_err);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so_err, &so_len) != 0) {
#ifdef _WIN32
            last_translated = iron_net_translate_wsa(IRON_NET_LAST_ERR());
#else
            last_translated = iron_net_translate_errno(IRON_NET_LAST_ERR());
#endif
            IRON_NET_CLOSE((int64_t)s);
            continue;
        }
        if (so_err != 0) {
#ifdef _WIN32
            last_translated = iron_net_translate_wsa(so_err);
#else
            last_translated = iron_net_translate_errno(so_err);
#endif
            IRON_NET_CLOSE((int64_t)s);
            continue;
        }

        /* Connected. */
        out.v0.fd = (int64_t)s;
        out.v1    = iron_net_err_none();
        freeaddrinfo(res);
        return out;
    }

    freeaddrinfo(res);

    if (any_timeout && last_translated == 0) {
        out.v1 = iron_net_err_timeout();
    } else if (last_translated != 0) {
        out.v1 = iron_net_err_code(last_translated);
    } else {
        out.v1 = iron_net_err_code(IRON_ERR_NET_CONN_REFUSED);
    }
    return out;
}

/* ── accept with deadline ────────────────────────────────────────────── */

Iron_Result_TcpSocket_Error Iron_tcplistener_accept(Iron_TcpListener l, int64_t timeout) {
    Iron_Result_TcpSocket_Error out;
    out.v0.fd = -1;
    out.v1    = iron_net_err_none();

    Iron_Deadline dl = Iron_deadline_from_timeout_ms(timeout);

    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sslen = sizeof(ss);
        iron_sock_t cfd = accept((iron_sock_t)l.fd, (struct sockaddr*)&ss, &sslen);
        if (cfd != IRON_INVALID_SOCK) {
            iron_net_set_nonblocking((int64_t)cfd);
            out.v0.fd = (int64_t)cfd;
            out.v1    = iron_net_err_none();
            return out;
        }
        int e = IRON_NET_LAST_ERR();
        if (e != IRON_NET_EWOULDBLOCK
#ifndef _WIN32
            && e != EAGAIN && e != EINTR
#endif
            ) {
            out.v1 = iron_net_err_from_last();
            return out;
        }
        int rem = Iron_deadline_remaining_ms(dl);
        if (rem <= 0) {
            out.v1 = iron_net_err_timeout();
            return out;
        }
        int rc = iron_net_poll(l.fd, POLLIN, rem);
        if (rc == 0) {
            out.v1 = iron_net_err_timeout();
            return out;
        }
        if (rc < 0) {
            out.v1 = iron_net_err_from_last();
            return out;
        }
        /* Ready — loop to retry accept. */
    }
}

/* ── Raw recv/send work engines ──────────────────────────────────────── */

Iron_Result_Int_Error Iron_net_tcp_recv_bytes(Iron_TcpSocket s,
                                                uint8_t *buf,
                                                int64_t cap,
                                                int64_t timeout) {
    Iron_Result_Int_Error out;
    out.v0 = 0;
    out.v1 = iron_net_err_none();

    if (!buf || cap <= 0) return out;

    Iron_Deadline dl = Iron_deadline_from_timeout_ms(timeout);

    for (;;) {
#ifdef _WIN32
        int n = recv((SOCKET)s.fd, (char*)buf, (int)cap, 0);
#else
        ssize_t n = recv((int)s.fd, (char*)buf, (size_t)cap, 0);
#endif
        if (n >= 0) {
            out.v0 = (int64_t)n;
            out.v1 = iron_net_err_none();
            return out;
        }
        int e = IRON_NET_LAST_ERR();
#ifndef _WIN32
        if (e == EINTR) continue;
#endif
        if (e != IRON_NET_EWOULDBLOCK
#ifndef _WIN32
            && e != EAGAIN
#endif
            ) {
            out.v1 = iron_net_err_from_last();
            return out;
        }
        int rem = Iron_deadline_remaining_ms(dl);
        if (rem <= 0) {
            out.v1 = iron_net_err_timeout();
            return out;
        }
        int rc = iron_net_poll(s.fd, POLLIN, rem);
        if (rc == 0) {
            out.v1 = iron_net_err_timeout();
            return out;
        }
        if (rc < 0) {
            out.v1 = iron_net_err_from_last();
            return out;
        }
    }
}

Iron_Result_Int_Error Iron_net_tcp_send_bytes(Iron_TcpSocket s,
                                                const uint8_t *buf,
                                                int64_t len,
                                                int64_t timeout) {
    Iron_Result_Int_Error out;
    out.v0 = 0;
    out.v1 = iron_net_err_none();

    if (!buf || len <= 0) return out;

    Iron_Deadline dl = Iron_deadline_from_timeout_ms(timeout);

    for (;;) {
#ifdef _WIN32
        int n = send((SOCKET)s.fd, (const char*)buf, (int)len, 0);
#else
        ssize_t n = send((int)s.fd, (const char*)buf, (size_t)len, 0);
#endif
        if (n >= 0) {
            out.v0 = (int64_t)n;
            out.v1 = iron_net_err_none();
            return out;
        }
        int e = IRON_NET_LAST_ERR();
#ifndef _WIN32
        if (e == EINTR) continue;
#endif
        if (e != IRON_NET_EWOULDBLOCK
#ifndef _WIN32
            && e != EAGAIN
#endif
            ) {
            out.v1 = iron_net_err_from_last();
            return out;
        }
        int rem = Iron_deadline_remaining_ms(dl);
        if (rem <= 0) {
            out.v1 = iron_net_err_timeout();
            return out;
        }
        int rc = iron_net_poll(s.fd, POLLOUT, rem);
        if (rc == 0) {
            out.v1 = iron_net_err_timeout();
            return out;
        }
        if (rc < 0) {
            out.v1 = iron_net_err_from_last();
            return out;
        }
    }
}

/* ── Iron-facing read/write that take Iron_String buffers ────────────── */

Iron_Result_Int_Error Iron_tcpsocket_read(Iron_TcpSocket s, Iron_String buf, int64_t timeout) {
    /* PROT-03 row 24 (AUDIT-01 M-severity): iron_string_cstr returns a
     * const char * into the source Iron_String's SSO/heap buffer. The
     * previous code cast away const to feed `base` directly into
     * Iron_net_tcp_recv_bytes, which then wrote bytes into the source
     * string's immutable storage — silent data corruption of an interned
     * or SSO Iron_String. (Iron_String is value-typed and immutable from
     * the Iron side; the wrapper exists for API symmetry with the raw
     * recv engine, not for caller-visible byte transfer.)
     *
     * Fix: recv into a local mutable buffer sized to the requested
     * capacity. The (count, error) return value is unchanged; the
     * received bytes themselves are intentionally discarded because the
     * caller has no value-level path to retrieve them. Callers that need
     * the bytes must use Iron_net_tcp_recv_bytes directly with their own
     * mutable buffer. */
    int64_t cap = (int64_t)iron_string_byte_len(&buf);
    if (cap <= 0) {
        return Iron_net_tcp_recv_bytes(s, NULL, 0, timeout);
    }
    /* Cap the local buffer to a sane maximum to avoid unbounded stack
     * use. 64 KiB matches the default TCP recv chunk size on Linux and
     * is well below the typical 1 MiB stack budget. */
    enum { LOCAL_RECV_MAX = 65536 };
    int64_t local_cap = cap < (int64_t)LOCAL_RECV_MAX ? cap : (int64_t)LOCAL_RECV_MAX;
    uint8_t *local_recv_buf = (uint8_t *)malloc((size_t)local_cap);
    if (!local_recv_buf) {
        Iron_Result_Int_Error oom = {0, iron_net_err_from_last()};
        return oom;
    }
    Iron_Result_Int_Error result = Iron_net_tcp_recv_bytes(
        s, local_recv_buf, local_cap, timeout);
    free(local_recv_buf);
    return result;
}

Iron_Result_Int_Error Iron_tcpsocket_write(Iron_TcpSocket s, Iron_String buf, int64_t timeout) {
    const char *base = iron_string_cstr(&buf);
    int64_t     len  = (int64_t)iron_string_byte_len(&buf);
    return Iron_net_tcp_send_bytes(s, (const uint8_t *)base, len, timeout);
}

/* ── close — idempotent via the fd>=0 check ─────────────────────────── */

void Iron_tcpsocket_close(Iron_TcpSocket s) {
    if (s.fd < 0) return;
    IRON_NET_CLOSE(s.fd);
}

void Iron_tcplistener_close(Iron_TcpListener l) {
    if (l.fd < 0) return;
    IRON_NET_CLOSE(l.fd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 59 P03: UDP + IP address wrappers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── IPv4 parse/format ─────────────────────────────────────────────────── */

/* Iron_IPv4Addr is layout-compatible with the compiler-emitted
 * `struct Iron_IPv4Addr { int64_t a, b, c, d; }`. The low byte of each
 * int64 field holds the actual octet; the upper 7 bytes are zeros.
 * inet_pton/inet_ntop operate on a contiguous uint8_t[4], so we
 * marshal in/out explicitly. */

static void ipv4_octets_from_bytes(Iron_IPv4Addr *a, const uint8_t octets[4]) {
    a->a = (int64_t)octets[0];
    a->b = (int64_t)octets[1];
    a->c = (int64_t)octets[2];
    a->d = (int64_t)octets[3];
}

static void ipv4_octets_to_bytes(Iron_IPv4Addr a, uint8_t octets[4]) {
    octets[0] = (uint8_t)(a.a & 0xff);
    octets[1] = (uint8_t)(a.b & 0xff);
    octets[2] = (uint8_t)(a.c & 0xff);
    octets[3] = (uint8_t)(a.d & 0xff);
}

Iron_Result_IPv4Addr_NetError Iron_ipv4addr_parse(Iron_String s) {
    Iron_Result_IPv4Addr_NetError out;
    out.v0.a = 0;
    out.v0.b = 0;
    out.v0.c = 0;
    out.v0.d = 0;

    size_t len = iron_string_byte_len(&s);
    if (len == 0 || len >= INET_ADDRSTRLEN) {
        out.v1 = iron_net_err_code(IRON_ERR_NET_BAD_IP);
        return out;
    }
    char tmp[INET_ADDRSTRLEN];
    memcpy(tmp, iron_string_cstr(&s), len);
    tmp[len] = '\0';

    uint8_t octets[4];
    int rc = inet_pton(AF_INET, tmp, octets);
    if (rc != 1) {
        out.v1 = iron_net_err_code(IRON_ERR_NET_BAD_IP);
        return out;
    }
    ipv4_octets_from_bytes(&out.v0, octets);
    out.v1 = iron_net_err_none();
    return out;
}

Iron_String Iron_ipv4addr_format(Iron_IPv4Addr a) {
    uint8_t octets[4];
    ipv4_octets_to_bytes(a, octets);
    char buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, octets, buf, sizeof(buf))) {
        return iron_string_from_literal("", 0);
    }
    return iron_string_from_cstr(buf, strlen(buf));
}

/* ── IPv6 parse/format with zone-identifier support ────────────────────── */

/* RFC 4007 / 6874: addresses may carry a zone identifier after '%'.
 * URL-embedded forms use '%25' (percent-encoded '%') followed by the zone.
 * Both `fe80::1%eth0` and `fe80::1%25eth0` must parse to the same zone.
 *
 * The Iron-side `object IPv6Addr { val bytes: String; val zone: String }`
 * stores the 16 raw octets as an Iron_String payload — the C side
 * creates a fresh Iron_String from the inet_pton output. */
Iron_Result_IPv6Addr_NetError Iron_ipv6addr_parse(Iron_String s) {
    Iron_Result_IPv6Addr_NetError out;
    out.v0.bytes = iron_string_from_literal("", 0);
    out.v0.zone  = iron_string_from_literal("", 0);

    size_t len = iron_string_byte_len(&s);
    if (len == 0 || len >= INET6_ADDRSTRLEN + 64) {
        out.v1 = iron_net_err_code(IRON_ERR_NET_BAD_IP);
        return out;
    }
    const char *cs = iron_string_cstr(&s);

    /* Find the optional zone separator. */
    const char *percent = (const char *)memchr(cs, '%', len);
    size_t addr_len = percent ? (size_t)(percent - cs) : len;
    const char *zone_start = NULL;
    size_t      zone_len   = 0;
    if (percent) {
        zone_start = percent + 1;
        zone_len   = len - addr_len - 1;
        /* URL-encoded '%' prefix stripping: if zone begins with "25", the
         * intent was `%25<zone>` in an originally-URL-encoded string. */
        if (zone_len >= 2 && zone_start[0] == '2' && zone_start[1] == '5') {
            zone_start += 2;
            zone_len   -= 2;
        }
    }

    if (addr_len == 0 || addr_len >= INET6_ADDRSTRLEN || zone_len > 63) {
        out.v1 = iron_net_err_code(IRON_ERR_NET_BAD_IP);
        return out;
    }

    char tmp[INET6_ADDRSTRLEN];
    memcpy(tmp, cs, addr_len);
    tmp[addr_len] = '\0';

    uint8_t raw_octets[16];
    int rc = inet_pton(AF_INET6, tmp, raw_octets);
    if (rc != 1) {
        out.v1 = iron_net_err_code(IRON_ERR_NET_BAD_IP);
        return out;
    }

    out.v0.bytes = iron_string_from_cstr((const char *)raw_octets, 16);
    if (zone_len > 0) {
        out.v0.zone = iron_string_from_cstr(zone_start, zone_len);
    }
    out.v1 = iron_net_err_none();
    return out;
}

Iron_String Iron_ipv6addr_format(Iron_IPv6Addr a) {
    size_t blen = iron_string_byte_len(&a.bytes);
    if (blen != 16) {
        return iron_string_from_literal("", 0);
    }
    const uint8_t *octets = (const uint8_t *)iron_string_cstr(&a.bytes);

    /* buf must accommodate the longest canonical form + '%' + zone. */
    char buf[INET6_ADDRSTRLEN + 1 + 64];
    if (!inet_ntop(AF_INET6, octets, buf, INET6_ADDRSTRLEN)) {
        return iron_string_from_literal("", 0);
    }
    size_t alen = strlen(buf);
    size_t zlen = iron_string_byte_len(&a.zone);
    if (zlen > 0 && zlen < 64 && alen + 1 + zlen + 1 <= sizeof(buf)) {
        buf[alen] = '%';
        memcpy(buf + alen + 1, iron_string_cstr(&a.zone), zlen);
        size_t total = alen + 1 + zlen;
        buf[total] = '\0';
        return iron_string_from_cstr(buf, total);
    }
    return iron_string_from_cstr(buf, alen);
}

/* ── UDP bind ─────────────────────────────────────────────────────────── */

Iron_Result_UdpSocket_NetError Iron_net_udp_bind(Iron_String host, int64_t port) {
    Iron_Result_UdpSocket_NetError out;
    out.v0.fd = -1;
    out.v1    = iron_net_err_none();

    const char *host_c = iron_string_cstr(&host);
    if (!host_c) host_c = "";

    char service[8];
    port_to_service(port, service, sizeof(service));

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_PASSIVE | AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host_c[0] ? host_c : NULL, service, &hints, &res);
    if (gai != 0 || !res) {
        out.v1 = iron_net_err_code(IRON_ERR_NET_BAD_IP);
        return out;
    }

    iron_sock_t bound_fd = IRON_INVALID_SOCK;
    int last_err = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        iron_sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == IRON_INVALID_SOCK) {
            last_err = IRON_NET_LAST_ERR();
            continue;
        }

        /* UDP dual-stack: when the family is v6, drop V6ONLY so the socket
         * can receive IPv4 datagrams via v4-mapped addresses. Same policy
         * as the TCP listener above (INFRA-08). */
        if (ai->ai_family == AF_INET6) {
            int v6only = 0;
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
                       (const char *)&v6only, sizeof(v6only));
        }

        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&one, sizeof(one));

        if (bind(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) != 0) {
            last_err = IRON_NET_LAST_ERR();
            IRON_NET_CLOSE((int64_t)s);
            continue;
        }
        /* Non-blocking so recvfrom with timeout works. */
        iron_net_set_nonblocking((int64_t)s);

        bound_fd = s;
        break;
    }
    freeaddrinfo(res);

    if (bound_fd == IRON_INVALID_SOCK) {
        if (last_err == 0) {
            out.v1 = iron_net_err_code(IRON_ERR_NET_UNKNOWN);
        } else {
#ifdef _WIN32
            out.v1 = iron_net_err_code(iron_net_translate_wsa(last_err));
#else
            out.v1 = iron_net_err_code(iron_net_translate_errno(last_err));
#endif
        }
        return out;
    }

    out.v0.fd = (int64_t)bound_fd;
    out.v1    = iron_net_err_none();
    return out;
}

void Iron_udpsocket_close(Iron_UdpSocket s) {
    if (s.fd < 0) return;
    IRON_NET_CLOSE(s.fd);
}

/* ── UDP sendto: flat impl shared between v4 and v6 helpers ────────────── */

static Iron_Result_Int_NetError iron_udp_sendto_flat(Iron_UdpSocket s,
                                                       Iron_String    buf,
                                                       int            family,
                                                       const uint8_t *addr_bytes,
                                                       size_t         addr_len,
                                                       Iron_String    addr_zone,
                                                       int64_t        port,
                                                       int64_t        timeout_ms) {
    Iron_Result_Int_NetError out;
    out.v0 = 0;
    out.v1 = iron_net_err_none();

    Iron_Deadline dl = Iron_deadline_from_timeout_ms(timeout_ms);

    struct sockaddr_storage dst;
    memset(&dst, 0, sizeof(dst));
    socklen_t dst_len = 0;

    if (family == 4) {
        if (addr_len != 4) {
            out.v1 = iron_net_err_code(IRON_ERR_NET_BAD_IP);
            return out;
        }
        struct sockaddr_in *sin = (struct sockaddr_in *)&dst;
        sin->sin_family = AF_INET;
        sin->sin_port   = htons((uint16_t)port);
        memcpy(&sin->sin_addr, addr_bytes, 4);
        dst_len = sizeof(*sin);
    } else if (family == 6) {
        if (addr_len != 16) {
            out.v1 = iron_net_err_code(IRON_ERR_NET_BAD_IP);
            return out;
        }
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&dst;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port   = htons((uint16_t)port);
        memcpy(&sin6->sin6_addr, addr_bytes, 16);
#ifndef _WIN32
        size_t zlen = iron_string_byte_len(&addr_zone);
        if (zlen > 0 && zlen < IF_NAMESIZE) {
            char zbuf[IF_NAMESIZE];
            memcpy(zbuf, iron_string_cstr(&addr_zone), zlen);
            zbuf[zlen] = '\0';
            sin6->sin6_scope_id = if_nametoindex(zbuf);
        }
#else
        (void)addr_zone;
#endif
        dst_len = sizeof(*sin6);
    } else {
        out.v1 = iron_net_err_code(IRON_ERR_NET_AF_NOT_SUPPORTED);
        return out;
    }

    size_t      blen  = iron_string_byte_len(&buf);
    const char *bytes = iron_string_cstr(&buf);

    for (;;) {
#ifdef _WIN32
        int n = sendto((SOCKET)s.fd, bytes, (int)blen, 0,
                       (struct sockaddr *)&dst, dst_len);
#else
        ssize_t n = sendto((int)s.fd, bytes, blen, 0,
                           (struct sockaddr *)&dst, dst_len);
#endif
        if (n >= 0) {
            out.v0 = (int64_t)n;
            out.v1 = iron_net_err_none();
            return out;
        }
        int e = IRON_NET_LAST_ERR();
#ifndef _WIN32
        if (e == EINTR) continue;
#endif
        if (e != IRON_NET_EWOULDBLOCK
#ifndef _WIN32
            && e != EAGAIN
#endif
            ) {
            out.v1 = iron_net_err_from_last();
            return out;
        }
        int rem = Iron_deadline_remaining_ms(dl);
        if (rem <= 0) {
            out.v1 = iron_net_err_timeout();
            return out;
        }
        int rc = iron_net_poll(s.fd, POLLOUT, rem);
        if (rc == 0) {
            out.v1 = iron_net_err_timeout();
            return out;
        }
        if (rc < 0) {
            out.v1 = iron_net_err_from_last();
            return out;
        }
    }
}

Iron_Result_Int_NetError Iron_net_udp_sendto_v4(Iron_UdpSocket s,
                                                  Iron_String    buf,
                                                  Iron_IPv4Addr  addr,
                                                  int64_t        port,
                                                  int64_t        timeout) {
    uint8_t octets[4];
    ipv4_octets_to_bytes(addr, octets);
    Iron_String empty_zone = iron_string_from_literal("", 0);
    return iron_udp_sendto_flat(s, buf, 4, octets, 4, empty_zone,
                                 port, timeout);
}

Iron_Result_Int_NetError Iron_net_udp_sendto_v6(Iron_UdpSocket s,
                                                  Iron_String    buf,
                                                  Iron_IPv6Addr  addr,
                                                  int64_t        port,
                                                  int64_t        timeout) {
    size_t blen = iron_string_byte_len(&addr.bytes);
    if (blen != 16) {
        Iron_Result_Int_NetError err_out;
        err_out.v0 = 0;
        err_out.v1 = iron_net_err_code(IRON_ERR_NET_BAD_IP);
        return err_out;
    }
    const uint8_t *raw = (const uint8_t *)iron_string_cstr(&addr.bytes);
    return iron_udp_sendto_flat(s, buf, 6, raw, 16, addr.zone,
                                 port, timeout);
}

/* ── UDP recvfrom (struct-return ABI) ──────────────────────────────────── */

Iron_UdpRecvResult Iron_udpsocket_recvfrom(Iron_UdpSocket s,
                                             uint8_t       *buf,
                                             int64_t        cap,
                                             int64_t        timeout) {
    Iron_UdpRecvResult out;
    out.nbytes      = 0;
    out.addr_family = 0;
    out.addr_bytes  = iron_string_from_literal("", 0);
    out.addr_zone   = iron_string_from_literal("", 0);
    out.port        = 0;
    out.err         = iron_net_err_none();

    if (!buf || cap <= 0) {
        out.err = iron_net_err_code(IRON_ERR_NET_UNKNOWN);
        return out;
    }

    Iron_Deadline dl = Iron_deadline_from_timeout_ms(timeout);

    for (;;) {
        struct sockaddr_storage src;
        socklen_t src_len = sizeof(src);
        memset(&src, 0, sizeof(src));

#ifdef _WIN32
        int n = recvfrom((SOCKET)s.fd, (char *)buf, (int)cap, 0,
                         (struct sockaddr *)&src, &src_len);
#else
        ssize_t n = recvfrom((int)s.fd, (char *)buf, (size_t)cap, 0,
                             (struct sockaddr *)&src, &src_len);
#endif
        if (n >= 0) {
            out.nbytes = (int64_t)n;
            if (src.ss_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&src;
                out.addr_family = 4;
                out.addr_bytes  = iron_string_from_cstr(
                    (const char *)&sin->sin_addr, 4);
                out.port        = ntohs(sin->sin_port);
            } else if (src.ss_family == AF_INET6) {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&src;
                out.addr_family = 6;
                out.addr_bytes  = iron_string_from_cstr(
                    (const char *)&sin6->sin6_addr, 16);
                out.port        = ntohs(sin6->sin6_port);
#ifndef _WIN32
                if (sin6->sin6_scope_id != 0) {
                    char zbuf[IF_NAMESIZE];
                    if (if_indextoname(sin6->sin6_scope_id, zbuf)) {
                        out.addr_zone = iron_string_from_cstr(
                            zbuf, strlen(zbuf));
                    }
                }
#endif
            }
            out.err = iron_net_err_none();
            return out;
        }

        int e = IRON_NET_LAST_ERR();
#ifndef _WIN32
        if (e == EINTR) continue;
#endif
        if (e != IRON_NET_EWOULDBLOCK
#ifndef _WIN32
            && e != EAGAIN
#endif
            ) {
            out.err = iron_net_err_from_last();
            return out;
        }
        int rem = Iron_deadline_remaining_ms(dl);
        if (rem <= 0) {
            out.err = iron_net_err_timeout();
            return out;
        }
        int rc = iron_net_poll(s.fd, POLLIN, rem);
        if (rc == 0) {
            out.err = iron_net_err_timeout();
            return out;
        }
        if (rc < 0) {
            out.err = iron_net_err_from_last();
            return out;
        }
    }
}

/* ── Phase 59 P04: DNS lookup_host (Iron_Net_lookup_host_result) ─────────
 *
 * Iron source: `val (addrs, err) = Net.lookup_host("example.com", 2000)`
 * Iron method call `Net.lookup_host` lowers through hir_to_lir to the C
 * symbol `Iron_net_lookup_host`. The Unity-test-facing spelling is
 * `Iron_Net_lookup_host_result` (declared as a static-inline alias in
 * iron_net.h).
 *
 * Uses the `abandoned flag` variant of Iron_PoolWait per RESEARCH.md
 * Pitfall 2 (stuck getaddrinfo) + Pitfall 6 (freeaddrinfo double-free on
 * timeout race). The caller submits a DNS job on Iron_io_pool, blocks on
 * Iron_poolwait_wait_ms, and on timeout marks the wait abandoned +
 * Iron_pool_mark_one_leaked(Iron_io_pool). The stuck worker eventually
 * returns from getaddrinfo, runs Iron_poolwait_worker_finish which
 * detects the abandoned flag and destroys the addrinfo result without
 * touching caller memory.
 *
 * Host string lifetime (Pitfall 4): the worker may outlive the caller's
 * Iron_String on the timeout path, so the name is heap-copied into the
 * DnsJob before submit. iron_string_from_cstr handles SSO vs heap — for
 * short hostnames (<24 bytes) the copy lands in the inline SSO slot
 * with zero heap allocation; longer hostnames go through libc malloc
 * and are freed on the worker side (see dns_worker_fn comment). */

#include <stdlib.h>  /* calloc, free */

static int iron_net_translate_eai(int eai) {
#ifdef EAI_NONAME
    if (eai == EAI_NONAME) return IRON_ERR_NET_BAD_HOST;
#endif
#ifdef EAI_NODATA
    if (eai == EAI_NODATA) return IRON_ERR_NET_BAD_HOST;
#endif
#ifdef EAI_AGAIN
    if (eai == EAI_AGAIN)  return IRON_ERR_NET_DNS_TEMP_FAIL;
#endif
#ifdef EAI_FAIL
    if (eai == EAI_FAIL)   return IRON_ERR_NET_DNS_FAIL;
#endif
#ifdef EAI_MEMORY
    if (eai == EAI_MEMORY) return IRON_ERR_NET_NO_MEMORY;
#endif
#ifdef EAI_FAMILY
    if (eai == EAI_FAMILY) return IRON_ERR_NET_AF_NOT_SUPPORTED;
#endif
    if (eai == 0) return 0;
    return IRON_ERR_NET_DNS_OTHER;
}

/* DnsJob ownership model:
 *
 *   - Caller heap-allocates DnsJob before submit.
 *   - Caller owns DnsJob end-to-end on the NORMAL path: after wait_ms
 *     returns 1, the caller reads job->result + job->eai_err, builds
 *     the list, freeaddrinfo()'s the result, then free()'s the job.
 *   - Worker owns DnsJob on the ABANDONED path: the worker's
 *     dns_worker_fn will see w->abandoned in worker_finish, get control
 *     back, freeaddrinfo(job->result), and free(job) itself.
 *   - The Iron_PoolWait struct is ALSO caller-owned on the normal path,
 *     and intentionally LEAKED on the timeout path (the abandoned-flag
 *     variant mandates this — RESEARCH.md section 2209-2249).
 *
 * The wait->result stash from worker_finish(w, result, dtor) is not
 * read back here because we already have a direct pointer via the job.
 * The job pointer serves as both the job arg and the readback channel. */
typedef struct {
    Iron_String      name_copy;  /* heap/SSO-owned copy; lifetime = job */
    struct addrinfo *result;     /* worker output (NULL on error) */
    int              eai_err;    /* worker output: 0 = OK, else getaddrinfo code */
    Iron_PoolWait   *wait;       /* abandoned-flag coordination */
} DnsJob;

/* Result destructor: called from Iron_poolwait_worker_finish on the
 * abandoned branch to free the addrinfo the worker produced and to free
 * the DnsJob itself. The `result` arg here is the DnsJob pointer (not
 * the addrinfo), because we pass `job` to worker_finish's 2nd arg. */
static void dns_abandoned_destructor(void *p) {
    DnsJob *job = (DnsJob *)p;
    if (!job) return;
    if (job->result) freeaddrinfo(job->result);
    /* Free the wait struct too — caller walked away without destroying
     * it (that's the whole point of the abandoned-flag variant). */
    if (job->wait) Iron_poolwait_destroy(job->wait);
    free(job);
}

/* Worker body. Runs on an Iron_io_pool thread. After getaddrinfo
 * returns, calls Iron_poolwait_worker_finish. In the normal path,
 * worker_finish signals the caller and returns — the caller owns job
 * and will free it. In the abandoned path, worker_finish runs the
 * destructor (dns_abandoned_destructor) which frees everything. */
static void dns_worker_fn(void *arg) {
    DnsJob *job = (DnsJob *)arg;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const char *node = iron_string_cstr(&job->name_copy);
    if (!node) node = "";

    int rc = getaddrinfo(node, NULL, &hints, &job->result);
    job->eai_err = rc;

    /* Pass `job` as the result so the abandoned-path destructor has
     * everything it needs to tear down. In the normal path the caller
     * reads the job fields directly and then frees it. */
    Iron_poolwait_worker_finish(job->wait, job, dns_abandoned_destructor);
    /* IMPORTANT: do NOT touch `job` below this line. In the abandoned
     * branch worker_finish already called dns_abandoned_destructor
     * which freed job. In the normal branch the caller owns job and
     * will access it from another thread. */
}

/* Convert a linked list of addrinfo results into an Iron_List_Iron_Address.
 * Malloc-backed: items buffer is freed by the caller (Iron runtime or
 * the Unity test's direct free(r.v0.items)). */
static Iron_List_Iron_Address build_address_list_from_addrinfo(
    struct addrinfo *ai)
{
    Iron_List_Iron_Address list;
    list.items    = NULL;
    list.count    = 0;
    list.capacity = 0;

    /* First pass: count how many entries we'll keep (AF_INET + AF_INET6). */
    int64_t n = 0;
    for (struct addrinfo *p = ai; p; p = p->ai_next) {
        if (p->ai_family == AF_INET || p->ai_family == AF_INET6) n++;
    }
    if (n == 0) return list;

    list.items = (Iron_Address *)calloc((size_t)n, sizeof(Iron_Address));
    if (!list.items) return list;
    list.capacity = n;

    for (struct addrinfo *p = ai; p; p = p->ai_next) {
        Iron_Address addr;
        memset(&addr, 0, sizeof(addr));
        if (p->ai_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)p->ai_addr;
            addr.tag = Iron_Address_TAG_V4;
            uint8_t octets[4];
            memcpy(octets, &sin->sin_addr, 4);
            addr.data.V4._0.a = octets[0];
            addr.data.V4._0.b = octets[1];
            addr.data.V4._0.c = octets[2];
            addr.data.V4._0.d = octets[3];
            list.items[list.count++] = addr;
        } else if (p->ai_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)p->ai_addr;
            addr.tag = Iron_Address_TAG_V6;
            addr.data.V6._0.bytes = iron_string_from_cstr(
                (const char *)&sin6->sin6_addr, 16);
            addr.data.V6._0.zone  = iron_string_from_cstr("", 0);
#ifndef _WIN32
            if (sin6->sin6_scope_id != 0) {
                char zbuf[IF_NAMESIZE];
                memset(zbuf, 0, sizeof(zbuf));
                if (if_indextoname(sin6->sin6_scope_id, zbuf)) {
                    addr.data.V6._0.zone = iron_string_from_cstr(
                        zbuf, strlen(zbuf));
                }
            }
#endif
            list.items[list.count++] = addr;
        }
    }
    return list;
}

Iron_Tuple__Address__NetError Iron_net_lookup_host(Iron_String name,
                                                    int64_t timeout_ms)
{
    Iron_Tuple__Address__NetError out;
    memset(&out, 0, sizeof(out));
    out.v1 = iron_net_err_none();

    DnsJob *job = (DnsJob *)calloc(1, sizeof(*job));
    if (!job) {
        out.v1 = iron_net_err_code(IRON_ERR_NET_NO_MEMORY);
        return out;
    }

    /* Heap-copy the host string (Pitfall 4). iron_string_from_cstr
     * handles SSO vs heap automatically. */
    const char *src     = iron_string_cstr(&name);
    if (!src) src = "";
    size_t      src_len = iron_string_byte_len(&name);
    job->name_copy = iron_string_from_cstr(src, src_len);

    job->wait = Iron_poolwait_create();
    if (!job->wait) {
        free(job);
        out.v1 = iron_net_err_code(IRON_ERR_NET_NO_MEMORY);
        return out;
    }

    Iron_pool_submit_wait(Iron_io_pool, dns_worker_fn, job, job->wait);

    Iron_Deadline dl = Iron_deadline_from_timeout_ms(timeout_ms);
    bool completed = false;
    while (!completed) {
        int rem = Iron_deadline_remaining_ms(dl);
        if (rem <= 0) {
            /* Timeout: abandon the wait + mark the worker leaked so the
             * elastic pool can spawn a replacement. Once the stuck
             * worker's getaddrinfo returns, dns_worker_fn calls
             * Iron_poolwait_worker_finish which sees w->abandoned and
             * runs dns_abandoned_destructor → freeaddrinfo(job->result),
             * Iron_poolwait_destroy(job->wait), free(job). The caller
             * never touches job/wait again. */
            Iron_poolwait_set_abandoned(job->wait);
            Iron_pool_mark_one_leaked(Iron_io_pool);
            out.v1 = iron_net_err_timeout();
            return out;
        }
        int wrc = Iron_poolwait_wait_ms(job->wait, rem);
        if (wrc == 1) {
            completed = true;
        } else if (wrc == 0) {
            /* Spurious wake or partial timeout — recheck outer deadline. */
            continue;
        } else {
            /* Error path — treat as abandon so the worker cleans up. */
            Iron_poolwait_set_abandoned(job->wait);
            Iron_pool_mark_one_leaked(Iron_io_pool);
            out.v1 = iron_net_err_code(IRON_ERR_NET_UNKNOWN);
            return out;
        }
    }

    /* Normal path: worker completed and signalled. Caller now owns
     * job->result (addrinfo) and job->eai_err. The DnsJob struct is
     * still alive because the worker went through the non-abandoned
     * branch of worker_finish (which just signals and returns without
     * running the destructor). */
    int              eai = job->eai_err;
    struct addrinfo *ai  = job->result;

    if (eai != 0 || ai == NULL) {
        int code = iron_net_translate_eai(eai);
        if (code == 0) code = IRON_ERR_NET_DNS_OTHER;
        if (ai) freeaddrinfo(ai);
        Iron_poolwait_destroy(job->wait);
        free(job);
        out.v1 = iron_net_err_code(code);
        return out;
    }

    /* Build the Iron list. Takes ownership of nothing — we freeaddrinfo
     * below after the copy. */
    out.v0 = build_address_list_from_addrinfo(ai);

    freeaddrinfo(ai);
    Iron_poolwait_destroy(job->wait);
    free(job);

    out.v1 = iron_net_err_none();
    return out;
}
