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
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <errno.h>
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
    /* Iron_String is value-typed from the Iron side. The bytes live at
     * iron_string_cstr(&buf) and the capacity is iron_string_byte_len(&buf).
     * In practice Iron_String is immutable, so reads into it are only
     * meaningful if the caller supplies a preallocated buffer — this path
     * is here for API symmetry with the raw recv engine. */
    const char *base = iron_string_cstr(&buf);
    int64_t     cap  = (int64_t)iron_string_byte_len(&buf);
    return Iron_net_tcp_recv_bytes(s, (uint8_t *)(uintptr_t)base, cap, timeout);
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
