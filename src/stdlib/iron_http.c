/* iron_http.c -- bounded, synchronous HTTP/1.1 client/server support.
 *
 * HTTP composes over iron_net.c instead of owning a second socket layer. Every
 * parser allocation is bounded by an API limit, all writes handle partial
 * send(), and server framing rejects ambiguous Content-Length /
 * Transfer-Encoding combinations. Plain HTTP and verified HTTPS share the
 * same framing/parser implementation through a small transport adapter.
 * One-shot calls advertise `Connection: close`; explicit HttpClient pools and
 * server keep-alive responses support bounded persistent HTTP/1.1 sessions.
 */

#include "iron_http.h"
#include "iron_http_bench.h"
#include "iron_net.h"
#include "iron_tls.h"
#include "runtime/iron_errors.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netinet/tcp.h>
  #include <pthread.h>
#endif

#ifdef MSG_NOSIGNAL
  #define MSG_NOSIGNAL_IF_ANY MSG_NOSIGNAL
#else
  #define MSG_NOSIGNAL_IF_ANY 0
#endif

enum {
    HTTP_DEFAULT_MAX_BODY = 8 * 1024 * 1024,
    HTTP_DEFAULT_MAX_HEADER = 64 * 1024,
    HTTP_IO_CHUNK = 8192,
    HTTP_MAX_URL = 8192,
    HTTP_MAX_HOST = 1024
};

static Iron_String http_str(const char *s) {
    return iron_string_from_cstr(s ? s : "", s ? strlen(s) : 0);
}

static Iron_String http_slice(const char *s, size_t n) {
    return iron_string_from_cstr(s ? s : "", s ? n : 0);
}

/* Assemble a request head and body into one buffer so it leaves in one
 * write. Sent in pieces (request line, headers, separator, body), a request
 * on a persistent connection meets Nagle's algorithm and the server's
 * delayed ACK and waits ~40 ms on Linux. *out is malloc'd; 0 or an error. */
static int64_t http_build_request(char **out, size_t *out_len,
                                  const char *head, size_t head_len,
                                  const char *headers, size_t headers_len,
                                  const char *body, size_t body_len) {
    int headers_need_crlf = headers_len &&
        (headers_len < 2 || headers[headers_len - 2] != '\r' ||
         headers[headers_len - 1] != '\n');
    size_t total = head_len + headers_len + (headers_need_crlf ? 2 : 0) + 2;
    if (body_len > SIZE_MAX - total) return IRON_ERR_HTTP_INVALID_ARGUMENT;
    total += body_len;
    char *msg = (char *)malloc(total ? total : 1);
    if (!msg) return IRON_ERR_NET_NO_MEMORY;
    size_t at = 0;
    memcpy(msg, head, head_len);
    at += head_len;
    if (headers_len) {
        memcpy(msg + at, headers, headers_len);
        at += headers_len;
        if (headers_need_crlf) {
            memcpy(msg + at, "\r\n", 2);
            at += 2;
        }
    }
    memcpy(msg + at, "\r\n", 2);
    at += 2;
    if (body_len) memcpy(msg + at, body, body_len);
    *out = msg;
    *out_len = total;
    return 0;
}

static void http_set_nodelay(int64_t fd) {
#ifdef _WIN32
    BOOL one = TRUE;
    setsockopt((SOCKET)fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#else
    int one = 1;
    setsockopt((int)fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

/* Free the strings of a response being discarded (a retried attempt). */
static void http_response_dispose(Iron_HttpResponse *r) {
    iron_string_release(&r->reason);
    iron_string_release(&r->headers);
    iron_string_release(&r->body);
    iron_string_release(&r->error_message);
}

static const char *http_error_message(int64_t code) {
    switch (code) {
        case 0: return "";
        case IRON_ERR_HTTP_BAD_URL: return "invalid HTTP URL";
        case IRON_ERR_HTTP_UNSUPPORTED_SCHEME: return "unsupported URL scheme";
        case IRON_ERR_HTTP_MALFORMED_MESSAGE: return "malformed HTTP message";
        case IRON_ERR_HTTP_HEADERS_TOO_LARGE: return "HTTP headers exceed limit";
        case IRON_ERR_HTTP_BODY_TOO_LARGE: return "HTTP body exceeds limit";
        case IRON_ERR_HTTP_BAD_CONTENT_LENGTH: return "invalid Content-Length";
        case IRON_ERR_HTTP_TRUNCATED_MESSAGE: return "truncated HTTP message";
        case IRON_ERR_HTTP_UNSUPPORTED_TRANSFER: return "unsupported Transfer-Encoding";
        case IRON_ERR_HTTP_INVALID_ARGUMENT: return "invalid HTTP argument";
        case IRON_ERR_HTTP_FILE: return "could not read HTTP response file";
        case IRON_ERR_TLS_UNAVAILABLE: return "TLS backend is unavailable; install OpenSSL development files";
        case IRON_ERR_TLS_CONTEXT: return "could not create TLS context";
        case IRON_ERR_TLS_TRUST_STORE: return "could not load TLS trust roots";
        case IRON_ERR_TLS_HANDSHAKE: return "TLS handshake failed";
        case IRON_ERR_TLS_VERIFY: return "TLS certificate or hostname verification failed";
        case IRON_ERR_TLS_CERTIFICATE: return "could not load TLS certificate chain";
        case IRON_ERR_TLS_PRIVATE_KEY: return "could not load or verify TLS private key";
        case IRON_ERR_TLS_PROTOCOL: return "TLS protocol error";
        case IRON_ERR_TLS_CLOSED: return "TLS connection closed";
        case IRON_ERR_TLS_IO: return "TLS input/output error";
        case IRON_ERR_TLS_INVALID_ARGUMENT: return "invalid TLS argument";
        case IRON_ERR_NET_CONN_REFUSED: return "connection refused";
        case IRON_ERR_NET_CONN_RESET: return "connection reset";
        case IRON_ERR_NET_CONN_ABORTED: return "connection aborted";
        case IRON_ERR_NET_TIMEOUT: return "network timeout";
        case IRON_ERR_NET_UNREACHABLE: return "network unreachable";
        case IRON_ERR_NET_ADDR_IN_USE: return "address already in use";
        case IRON_ERR_NET_ADDR_NOT_AVAIL: return "address not available";
        case IRON_ERR_NET_BAD_HOST: return "host not found";
        case IRON_ERR_NET_CLOSED: return "connection closed";
        default: return "network or HTTP error";
    }
}

static Iron_HttpRequest http_request_empty(void) {
    Iron_HttpRequest r;
    memset(&r, 0, sizeof(r));
    r.method = http_str("");
    r.target = http_str("");
    r.path = http_str("");
    r.query = http_str("");
    r.version = http_str("");
    r.headers = http_str("");
    r.body = http_str("");
    r.error_message = http_str("");
    return r;
}

static Iron_HttpResponse http_response_empty(void) {
    Iron_HttpResponse r;
    memset(&r, 0, sizeof(r));
    r.reason = http_str("");
    r.headers = http_str("");
    r.body = http_str("");
    r.error_message = http_str("");
    return r;
}

static void http_request_error(Iron_HttpRequest *r, int64_t code) {
    r->error = code;
    r->error_message = http_str(http_error_message(code));
}

static void http_response_error(Iron_HttpResponse *r, int64_t code) {
    r->error = code;
    r->error_message = http_str(http_error_message(code));
}

void Iron_httpserverresult_release(Iron_HttpServerResult result) {
    /* Iron's automatic drop glue releases owned strings after this
     * compatibility helper returns. Direct C callers use _dispose. */
    (void)result;
}

void Iron_httpserverresult_dispose(Iron_HttpServerResult result) {
    iron_string_release(&result.error_message);
}

void Iron_httpconnectionresult_release(Iron_HttpConnectionResult result) {
    (void)result;
}

void Iron_httpconnectionresult_dispose(Iron_HttpConnectionResult result) {
    iron_string_release(&result.error_message);
}

void Iron_httpsserverresult_release(Iron_HttpsServerResult result) {
    (void)result;
}

void Iron_httpsserverresult_dispose(Iron_HttpsServerResult result) {
    iron_string_release(&result.error_message);
}

void Iron_httpsconnectionresult_release(Iron_HttpsConnectionResult result) {
    (void)result;
}

void Iron_httpsconnectionresult_dispose(Iron_HttpsConnectionResult result) {
    iron_string_release(&result.error_message);
}

void Iron_httpspendingconnectionresult_release(
    Iron_HttpsPendingConnectionResult result) {
    (void)result;
}

void Iron_httpspendingconnectionresult_dispose(
    Iron_HttpsPendingConnectionResult result) {
    iron_string_release(&result.error_message);
}

void Iron_httpclientresult_release(Iron_HttpClientResult result) {
    (void)result;
}

void Iron_httpclientresult_dispose(Iron_HttpClientResult result) {
    iron_string_release(&result.error_message);
}

void Iron_httprequest_release(Iron_HttpRequest request) {
    (void)request;
}

void Iron_httprequest_dispose(Iron_HttpRequest request) {
    iron_string_release(&request.method);
    iron_string_release(&request.target);
    iron_string_release(&request.path);
    iron_string_release(&request.query);
    iron_string_release(&request.version);
    iron_string_release(&request.headers);
    iron_string_release(&request.body);
    iron_string_release(&request.error_message);
}

void Iron_httpresponse_release(Iron_HttpResponse response) {
    (void)response;
}

void Iron_httpresponse_dispose(Iron_HttpResponse response) {
    iron_string_release(&response.reason);
    iron_string_release(&response.headers);
    iron_string_release(&response.body);
    iron_string_release(&response.error_message);
}

static int ascii_ieq_n(const char *a, size_t an, const char *b, size_t bn) {
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    }
    return 1;
}

static int http_is_tchar(unsigned char c) {
    return isalnum(c) || c == '!' || c == '#' || c == '$' || c == '%' ||
           c == '&' || c == '\'' || c == '*' || c == '+' || c == '-' ||
           c == '.' || c == '^' || c == '_' || c == '`' || c == '|' ||
           c == '~';
}

static int http_is_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static size_t find_header_end(const uint8_t *buf, size_t len) {
    if (len < 4) return SIZE_MAX;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') return i;
    }
    return SIZE_MAX;
}

static size_t find_crlf(const char *buf, size_t start, size_t len) {
    for (size_t i = start; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') return i;
    }
    return SIZE_MAX;
}

static int header_value_span(const char *headers, size_t len, const char *name,
                             const char **value_out, size_t *value_len_out) {
    size_t name_len = strlen(name);
    size_t pos = 0;
    while (pos < len) {
        size_t end = find_crlf(headers, pos, len);
        if (end == SIZE_MAX) end = len;
        size_t colon = pos;
        while (colon < end && headers[colon] != ':') colon++;
        if (colon < end && ascii_ieq_n(headers + pos, colon - pos, name, name_len)) {
            size_t vstart = colon + 1;
            while (vstart < end && (headers[vstart] == ' ' || headers[vstart] == '\t')) vstart++;
            size_t vend = end;
            while (vend > vstart && (headers[vend - 1] == ' ' || headers[vend - 1] == '\t')) vend--;
            *value_out = headers + vstart;
            *value_len_out = vend - vstart;
            return 1;
        }
        pos = end == len ? len : end + 2;
    }
    return 0;
}

static int header_has_token(const char *headers, size_t len, const char *name,
                            const char *wanted) {
    size_t name_len = strlen(name);
    size_t wanted_len = strlen(wanted);
    size_t pos = 0;
    while (pos < len) {
        size_t line_end = find_crlf(headers, pos, len);
        if (line_end == SIZE_MAX) line_end = len;
        size_t colon = pos;
        while (colon < line_end && headers[colon] != ':') colon++;
        if (colon < line_end &&
            ascii_ieq_n(headers + pos, colon - pos, name, name_len)) {
            size_t item = colon + 1;
            while (item <= line_end) {
                while (item < line_end &&
                       (headers[item] == ' ' || headers[item] == '\t')) item++;
                size_t end = item;
                while (end < line_end && headers[end] != ',') end++;
                size_t finish = end;
                while (finish > item &&
                       (headers[finish - 1] == ' ' || headers[finish - 1] == '\t'))
                    finish--;
                if (ascii_ieq_n(headers + item, finish - item,
                                wanted, wanted_len)) return 1;
                if (end == line_end) break;
                item = end + 1;
            }
        }
        pos = line_end == len ? len : line_end + 2;
    }
    return 0;
}

static int parse_content_length(const char *headers, size_t len,
                                size_t *out, int *present) {
    *out = 0;
    *present = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t line_end = find_crlf(headers, pos, len);
        if (line_end == SIZE_MAX) line_end = len;
        size_t colon = pos;
        while (colon < line_end && headers[colon] != ':') colon++;
        if (colon < line_end &&
            ascii_ieq_n(headers + pos, colon - pos,
                        "Content-Length", strlen("Content-Length"))) {
            size_t item = colon + 1;
            int saw_item = 0;
            while (item <= line_end) {
                while (item < line_end &&
                       (headers[item] == ' ' || headers[item] == '\t')) item++;
                size_t end = item;
                while (end < line_end && headers[end] != ',') end++;
                size_t finish = end;
                while (finish > item &&
                       (headers[finish - 1] == ' ' || headers[finish - 1] == '\t'))
                    finish--;
                if (finish == item) return 0;
                size_t n = 0;
                for (size_t i = item; i < finish; i++) {
                    if (headers[i] < '0' || headers[i] > '9') return 0;
                    unsigned digit = (unsigned)(headers[i] - '0');
                    if (n > (SIZE_MAX - digit) / 10) return 0;
                    n = n * 10 + digit;
                }
                if (*present && n != *out) return 0;
                *out = n;
                *present = 1;
                saw_item = 1;
                if (end == line_end) break;
                item = end + 1;
            }
            if (!saw_item) return 0;
        }
        pos = line_end == len ? len : line_end + 2;
    }
    return 1;
}

static int headers_have_forbidden_framing(const char *headers, size_t len) {
    const char *unused = NULL;
    size_t unused_len = 0;
    return header_value_span(headers, len, "Content-Length", &unused, &unused_len) ||
           header_value_span(headers, len, "Transfer-Encoding", &unused, &unused_len) ||
           header_value_span(headers, len, "Connection", &unused, &unused_len) ||
           header_value_span(headers, len, "Host", &unused, &unused_len);
}

static int raw_headers_valid(const char *headers, size_t len) {
    if (len == 0) return 1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)headers[i];
        if (c == 0 || c == 127 ||
            (c < 32 && c != '\r' && c != '\n' && c != '\t')) return 0;
        if (i + 3 < len && headers[i] == '\r' && headers[i + 1] == '\n' &&
            headers[i + 2] == '\r' && headers[i + 3] == '\n') return 0;
        if (c == '\n' && (i == 0 || headers[i - 1] != '\r')) return 0;
        if (c == '\r' && (i + 1 >= len || headers[i + 1] != '\n')) return 0;
    }
    size_t pos = 0;
    while (pos < len) {
        size_t end = find_crlf(headers, pos, len);
        if (end == SIZE_MAX) end = len;
        if (end == pos || headers[pos] == ' ' || headers[pos] == '\t') return 0;
        size_t colon = pos;
        while (colon < end && headers[colon] != ':') colon++;
        if (colon == pos || colon == end) return 0;
        for (size_t i = pos; i < colon; i++) {
            unsigned char c = (unsigned char)headers[i];
            if (!http_is_tchar(c)) return 0;
        }
        pos = end == len ? len : end + 2;
    }
    return 1;
}

static int header_count(const char *headers, size_t len, const char *name) {
    size_t name_len = strlen(name);
    size_t pos = 0;
    int count = 0;
    while (pos < len) {
        size_t end = find_crlf(headers, pos, len);
        if (end == SIZE_MAX) end = len;
        size_t colon = pos;
        while (colon < end && headers[colon] != ':') colon++;
        if (colon < end && ascii_ieq_n(headers + pos, colon - pos, name, name_len)) count++;
        pos = end == len ? len : end + 2;
    }
    return count;
}

/* Validate a request header block and find Host in one pass. Framing fields
 * are scanned separately because RFC 9110 permits repeated, comma-combined
 * Content-Length and Connection fields. */
typedef struct HttpHeaderScan {
    int valid;
    int host_count;
    const char *host;
    size_t host_len;
} HttpHeaderScan;

static unsigned char ascii_fold(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

static int name_is(const char *name, size_t n, const char *lower, size_t ln) {
    if (n != ln) return 0;
    for (size_t i = 0; i < n; i++)
        if (ascii_fold((unsigned char)name[i]) != (unsigned char)lower[i]) return 0;
    return 1;
}

static void http_scan_headers(const char *h, size_t len, HttpHeaderScan *sc) {
    memset(sc, 0, sizeof(*sc));
    size_t pos = 0;
    while (pos < len) {
        /* One line: name ":" value, ended by CRLF or the end of the block. */
        if (h[pos] == ' ' || h[pos] == '\t' || h[pos] == '\r' || h[pos] == '\n') return;
        size_t colon = pos;
        while (colon < len && http_is_tchar((unsigned char)h[colon])) colon++;
        if (colon == pos || colon >= len || h[colon] != ':') return;
        size_t end = colon + 1;
        while (end < len) {
            unsigned char c = (unsigned char)h[end];
            if (c == '\r') break;
            if (c == 0 || c == 127 || c == '\n' || (c < 32 && c != '\t')) return;
            end++;
        }
        size_t next = len;
        if (end < len) {
            if (end + 1 >= len || h[end + 1] != '\n') return;
            next = end + 2;
            if (next == len) return;               /* a trailing CRLF: empty last line */
        }
        size_t vstart = colon + 1, vend = end;
        while (vstart < vend && (h[vstart] == ' ' || h[vstart] == '\t')) vstart++;
        while (vend > vstart && (h[vend - 1] == ' ' || h[vend - 1] == '\t')) vend--;
        size_t nl = colon - pos;
        const char *name = h + pos;
        if (nl == 4 && name_is(name, nl, "host", 4)) {
            if (sc->host_count++ == 0) { sc->host = h + vstart; sc->host_len = vend - vstart; }
        }
        pos = next;
    }
    sc->valid = 1;
}

/* The implementation decodes chunked transfer coding only. Accept it when it
 * is the sole coding across all repeated field lines. */
static int parse_transfer_encoding(const char *headers, size_t len,
                                   int *present, int *chunked_only) {
    size_t pos = 0;
    int codings = 0;
    *present = 0;
    *chunked_only = 0;
    while (pos < len) {
        size_t line_end = find_crlf(headers, pos, len);
        if (line_end == SIZE_MAX) line_end = len;
        size_t colon = pos;
        while (colon < line_end && headers[colon] != ':') colon++;
        if (colon < line_end &&
            ascii_ieq_n(headers + pos, colon - pos,
                        "Transfer-Encoding", strlen("Transfer-Encoding"))) {
            *present = 1;
            size_t item = colon + 1;
            while (item <= line_end) {
                while (item < line_end &&
                       (headers[item] == ' ' || headers[item] == '\t')) item++;
                size_t end = item;
                while (end < line_end && headers[end] != ',') end++;
                size_t finish = end;
                while (finish > item &&
                       (headers[finish - 1] == ' ' || headers[finish - 1] == '\t'))
                    finish--;
                if (finish == item) return 0;
                codings++;
                if (!ascii_ieq_n(headers + item, finish - item,
                                 "chunked", strlen("chunked"))) return 1;
                if (end == line_end) break;
                item = end + 1;
            }
        }
        pos = line_end == len ? len : line_end + 2;
    }
    *chunked_only = codings == 1;
    return 1;
}

typedef struct HttpHead {
    uint8_t *data;
    size_t len;
    size_t marker;
    int64_t error;
} HttpHead;

typedef struct HttpMemoryTransport {
    const uint8_t *input;
    size_t input_len;
    size_t input_pos;
    uint8_t *output;
    size_t output_cap;
    size_t output_len;
    size_t write_calls;
} HttpMemoryTransport;

typedef struct HttpTransport {
    Iron_TcpSocket socket;
    Iron_TlsStream *tls;
    HttpMemoryTransport *memory;
} HttpTransport;

typedef struct HttpPendingInput {
    int64_t fd;
    uint8_t *data;
    size_t len;
    struct HttpPendingInput *next;
} HttpPendingInput;

static HttpPendingInput *http_pending_inputs;
#ifdef _WIN32
static SRWLOCK http_pending_lock = SRWLOCK_INIT;
static void http_pending_lock_acquire(void) {
    AcquireSRWLockExclusive(&http_pending_lock);
}
static void http_pending_lock_release(void) {
    ReleaseSRWLockExclusive(&http_pending_lock);
}
#else
static pthread_mutex_t http_pending_lock = PTHREAD_MUTEX_INITIALIZER;
static void http_pending_lock_acquire(void) {
    (void)pthread_mutex_lock(&http_pending_lock);
}
static void http_pending_lock_release(void) {
    (void)pthread_mutex_unlock(&http_pending_lock);
}
#endif

static void http_pending_store(int64_t fd, uint8_t *data, size_t len) {
    if (len == 0) { free(data); return; }
    HttpPendingInput *entry = (HttpPendingInput *)malloc(sizeof(*entry));
    if (!entry) { free(data); return; }
    entry->fd = fd;
    entry->data = data;
    entry->len = len;
    http_pending_lock_acquire();
    entry->next = http_pending_inputs;
    http_pending_inputs = entry;
    http_pending_lock_release();
}

static uint8_t *http_pending_take(int64_t fd, size_t *len) {
    uint8_t *data = NULL;
    *len = 0;
    http_pending_lock_acquire();
    HttpPendingInput **link = &http_pending_inputs;
    while (*link) {
        if ((*link)->fd == fd) {
            HttpPendingInput *entry = *link;
            *link = entry->next;
            data = entry->data;
            *len = entry->len;
            free(entry);
            break;
        }
        link = &(*link)->next;
    }
    http_pending_lock_release();
    return data;
}

static void http_pending_clear(int64_t fd) {
    size_t len = 0;
    uint8_t *data = http_pending_take(fd, &len);
    (void)len;
    free(data);
}

/* Plain sockets are non-blocking: try the call first and only consult the
 * clock (for the remaining budget) when it would block, which on a busy
 * keep-alive server is rare. */
static Iron_Result_Int_Error transport_read(HttpTransport transport,
                                             uint8_t *buffer, int64_t capacity,
                                             Iron_Deadline deadline) {
    if (transport.memory) {
        Iron_Result_Int_Error out;
        memset(&out, 0, sizeof(out));
        size_t remaining = transport.memory->input_len -
                           transport.memory->input_pos;
        size_t take = remaining < (size_t)capacity ? remaining :
                      (size_t)capacity;
        if (take)
            memcpy(buffer, transport.memory->input +
                   transport.memory->input_pos, take);
        transport.memory->input_pos += take;
        out.v0 = (int64_t)take;
        return out;
    }
    if (transport.tls) {
        return iron_tls_read(transport.tls, buffer, capacity, deadline);
    }
#ifndef _WIN32
    if (capacity > 0) {
        ssize_t n = recv((int)transport.socket.fd, buffer, (size_t)capacity, 0);
        if (n >= 0) {
            Iron_Result_Int_Error ok;
            ok.v0 = (int64_t)n;
            ok.v1.code = 0;
            return ok;
        }
    }
#endif
    return Iron_net_tcp_recv_bytes(transport.socket, buffer, capacity,
                                    Iron_deadline_remaining_ms(deadline));
}

static Iron_Result_Int_Error transport_write(HttpTransport transport,
                                              const uint8_t *buffer,
                                              int64_t length,
                                              Iron_Deadline deadline) {
    if (transport.memory) {
        Iron_Result_Int_Error out;
        memset(&out, 0, sizeof(out));
        if (length < 0 || (size_t)length > transport.memory->output_cap -
                                         transport.memory->output_len) {
            out.v1.code = IRON_ERR_HTTP_BODY_TOO_LARGE;
            return out;
        }
        memcpy(transport.memory->output + transport.memory->output_len,
               buffer, (size_t)length);
        transport.memory->output_len += (size_t)length;
        transport.memory->write_calls++;
        out.v0 = length;
        return out;
    }
    if (transport.tls) {
        return iron_tls_write(transport.tls, buffer, length, deadline);
    }
#ifndef _WIN32
    if (length > 0) {
        ssize_t n = send((int)transport.socket.fd, buffer, (size_t)length, MSG_NOSIGNAL_IF_ANY);
        if (n > 0) {
            Iron_Result_Int_Error ok;
            ok.v0 = (int64_t)n;
            ok.v1.code = 0;
            return ok;
        }
    }
#endif
    return Iron_net_tcp_send_bytes(transport.socket, buffer, length,
                                    Iron_deadline_remaining_ms(deadline));
}

static void transport_close(HttpTransport transport) {
    if (transport.memory) return;
    http_pending_clear(transport.socket.fd);
    if (transport.tls) iron_tls_stream_close(transport.tls);
    Iron_tcpsocket_close(transport.socket);
}

static HttpHead receive_head(HttpTransport transport, size_t max_header,
                             Iron_Deadline deadline) {
    HttpHead h;
    memset(&h, 0, sizeof(h));
    h.marker = SIZE_MAX;
    if (max_header < 16 || max_header > (size_t)INT64_MAX) {
        h.error = IRON_ERR_HTTP_INVALID_ARGUMENT;
        return h;
    }
    h.data = http_pending_take(transport.socket.fd, &h.len);
    /* Most heads fit in one small read: start with a buffer for that and
     * grow toward max_header only for the ones that do not. Pending bytes
     * from a pipelined read may already include the whole next message. */
    size_t cap = max_header < HTTP_IO_CHUNK ? max_header : HTTP_IO_CHUNK;
    if (cap < h.len) cap = h.len;
    uint8_t *grown = (uint8_t *)realloc(h.data, cap + 1);
    if (!grown) {
        free(h.data);
        h.data = NULL;
        h.error = IRON_ERR_NET_NO_MEMORY;
        return h;
    }
    h.data = grown;
    h.data[h.len] = 0;
    size_t scanned = 0;
    for (;;) {
        /* Resume the search for CRLFCRLF where the last one stopped. */
        size_t from = scanned > 3 ? scanned - 3 : 0;
        size_t found = find_header_end(h.data + from, h.len - from);
        if (found != SIZE_MAX) {
            h.marker = from + found;
            if (h.marker + 4 > max_header)
                h.error = IRON_ERR_HTTP_HEADERS_TOO_LARGE;
            return h;
        }
        scanned = h.len;
        if (h.len >= max_header) {
            h.error = IRON_ERR_HTTP_HEADERS_TOO_LARGE;
            return h;
        }
        if (h.len == cap) {
            size_t grown = cap * 2 < max_header ? cap * 2 : max_header;
            uint8_t *bigger = (uint8_t *)realloc(h.data, grown + 1);
            if (!bigger) {
                h.error = IRON_ERR_NET_NO_MEMORY;
                return h;
            }
            h.data = bigger;
            cap = grown;
        }
        size_t avail = cap - h.len;
        size_t chunk = avail < HTTP_IO_CHUNK ? avail : HTTP_IO_CHUNK;
        Iron_Result_Int_Error rr = transport_read(
            transport, h.data + h.len, (int64_t)chunk, deadline);
        if (rr.v1.code != 0) {
            h.error = rr.v1.code;
            return h;
        }
        if (rr.v0 == 0) {
            h.error = IRON_ERR_HTTP_TRUNCATED_MESSAGE;
            return h;
        }
        h.len += (size_t)rr.v0;
        h.data[h.len] = 0;
    }
}

static int64_t send_all(HttpTransport transport, const uint8_t *data,
                        size_t len, Iron_Deadline deadline) {
    size_t sent = 0;
    while (sent < len) {
        Iron_Result_Int_Error wr = transport_write(
            transport, data + sent, (int64_t)(len - sent), deadline);
        if (wr.v1.code != 0) return wr.v1.code;
        if (wr.v0 <= 0) return IRON_ERR_NET_CLOSED;
        sent += (size_t)wr.v0;
    }
    return 0;
}

typedef struct HttpReader {
    HttpTransport transport;
    const uint8_t *prefix;
    size_t prefix_len;
    size_t prefix_pos;
    uint8_t buf[HTTP_IO_CHUNK];
    size_t pos;
    size_t len;
    Iron_Deadline deadline;
    int64_t error;
    int eof;
} HttpReader;

static int reader_refill(HttpReader *r) {
    if (r->prefix_pos < r->prefix_len) return 1;
    if (r->pos < r->len) return 1;
    Iron_Result_Int_Error rr = transport_read(
        r->transport, r->buf, HTTP_IO_CHUNK, r->deadline);
    if (rr.v1.code != 0) {
        r->error = rr.v1.code;
        return 0;
    }
    if (rr.v0 == 0) {
        r->eof = 1;
        return 0;
    }
    r->pos = 0;
    r->len = (size_t)rr.v0;
    return 1;
}

static int reader_byte(HttpReader *r, uint8_t *out) {
    if (r->prefix_pos < r->prefix_len) {
        *out = r->prefix[r->prefix_pos++];
        return 1;
    }
    if (!reader_refill(r)) return 0;
    *out = r->buf[r->pos++];
    return 1;
}

static size_t reader_copy(HttpReader *r, uint8_t *out, size_t want) {
    size_t got = 0;
    while (got < want) {
        if (r->prefix_pos < r->prefix_len) {
            size_t avail = r->prefix_len - r->prefix_pos;
            size_t take = avail < want - got ? avail : want - got;
            memcpy(out + got, r->prefix + r->prefix_pos, take);
            r->prefix_pos += take;
            got += take;
            continue;
        }
        if (!reader_refill(r)) break;
        size_t avail = r->len - r->pos;
        size_t take = avail < want - got ? avail : want - got;
        memcpy(out + got, r->buf + r->pos, take);
        r->pos += take;
        got += take;
    }
    return got;
}

static void reader_save_pending(HttpReader *r) {
    size_t prefix_left = r->prefix_len - r->prefix_pos;
    size_t buffer_left = r->len - r->pos;
    size_t total = prefix_left + buffer_left;
    if (total == 0) return;
    uint8_t *data = (uint8_t *)malloc(total);
    if (!data) return;
    if (prefix_left)
        memcpy(data, r->prefix + r->prefix_pos, prefix_left);
    if (buffer_left)
        memcpy(data + prefix_left, r->buf + r->pos, buffer_left);
    http_pending_store(r->transport.socket.fd, data, total);
    r->prefix_pos = r->prefix_len;
    r->pos = r->len;
}

static int reader_line(HttpReader *r, char *line, size_t cap, size_t *len_out) {
    size_t n = 0;
    int saw_cr = 0;
    for (;;) {
        uint8_t b = 0;
        if (!reader_byte(r, &b)) return 0;
        if (saw_cr) {
            if (b != '\n') return -1;
            line[n] = 0;
            *len_out = n;
            return 1;
        }
        if (b == '\r') {
            saw_cr = 1;
            continue;
        }
        if (b == '\n' || n + 1 >= cap) return -1;
        line[n++] = (char)b;
    }
}

static int parse_hex_size(const char *line, size_t len, size_t *out) {
    size_t end = 0;
    while (end < len && line[end] != ';' && line[end] != ' ' && line[end] != '\t') end++;
    if (end == 0) return 0;
    size_t n = 0;
    for (size_t i = 0; i < end; i++) {
        unsigned char c = (unsigned char)line[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return 0;
        if (n > (SIZE_MAX - digit) / 16) return 0;
        n = n * 16 + digit;
    }
    size_t pos = end;
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;
    while (pos < len) {
        if (line[pos++] != ';') return 0;
        while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;
        size_t name = pos;
        while (pos < len && http_is_tchar((unsigned char)line[pos])) pos++;
        if (pos == name) return 0;
        while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;
        if (pos < len && line[pos] == '=') {
            pos++;
            while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;
            if (pos < len && line[pos] == '"') {
                pos++;
                int closed = 0;
                while (pos < len) {
                    unsigned char c = (unsigned char)line[pos++];
                    if (c == '"') { closed = 1; break; }
                    if (c == '\\') {
                        if (pos == len) return 0;
                        c = (unsigned char)line[pos++];
                    }
                    if ((c < 32 && c != '\t') || c == 127) return 0;
                }
                if (!closed) return 0;
            } else {
                size_t value = pos;
                while (pos < len && http_is_tchar((unsigned char)line[pos])) pos++;
                if (pos == value) return 0;
            }
            while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;
        }
    }
    *out = n;
    return 1;
}

static int read_chunked_body(HttpReader *reader, size_t max_body,
                             size_t max_trailer,
                             Iron_String *body_out, int64_t *error_out) {
    uint8_t *body = (uint8_t *)malloc(max_body + 1);
    if (!body) { *error_out = IRON_ERR_NET_NO_MEMORY; return 0; }
    size_t used = 0;
    for (;;) {
        char line[128];
        size_t line_len = 0;
        int lr = reader_line(reader, line, sizeof(line), &line_len);
        if (lr <= 0) {
            *error_out = reader->error ? reader->error : IRON_ERR_HTTP_TRUNCATED_MESSAGE;
            free(body);
            return 0;
        }
        size_t chunk = 0;
        if (!parse_hex_size(line, line_len, &chunk)) {
            *error_out = IRON_ERR_HTTP_MALFORMED_MESSAGE;
            free(body);
            return 0;
        }
        if (chunk == 0) {
            /* Consume and validate bounded optional trailer fields through
             * the empty line. Framing and routing fields are forbidden in
             * trailers so they cannot reinterpret the message. */
            size_t trailer_bytes = 0;
            for (;;) {
                lr = reader_line(reader, line, sizeof(line), &line_len);
                if (lr <= 0) {
                    *error_out = reader->error ? reader->error : IRON_ERR_HTTP_TRUNCATED_MESSAGE;
                    free(body);
                    return 0;
                }
                if (line_len == 0) break;
                if (line_len + 2 > max_trailer - trailer_bytes) {
                    *error_out = IRON_ERR_HTTP_HEADERS_TOO_LARGE;
                    free(body);
                    return 0;
                }
                trailer_bytes += line_len + 2;
                if (!raw_headers_valid(line, line_len) ||
                    headers_have_forbidden_framing(line, line_len)) {
                    *error_out = IRON_ERR_HTTP_MALFORMED_MESSAGE;
                    free(body);
                    return 0;
                }
            }
            body[used] = 0;
            *body_out = http_slice((const char *)body, used);
            free(body);
            return 1;
        }
        if (chunk > max_body - used) {
            *error_out = IRON_ERR_HTTP_BODY_TOO_LARGE;
            free(body);
            return 0;
        }
        if (reader_copy(reader, body + used, chunk) != chunk) {
            *error_out = reader->error ? reader->error : IRON_ERR_HTTP_TRUNCATED_MESSAGE;
            free(body);
            return 0;
        }
        used += chunk;
        uint8_t crlf[2];
        if (reader_copy(reader, crlf, 2) != 2 || crlf[0] != '\r' || crlf[1] != '\n') {
            *error_out = IRON_ERR_HTTP_MALFORMED_MESSAGE;
            free(body);
            return 0;
        }
    }
}

static int read_fixed_body(HttpReader *reader, size_t len, Iron_String *body_out,
                           int64_t *error_out) {
    uint8_t *body = (uint8_t *)malloc(len + 1);
    if (!body) { *error_out = IRON_ERR_NET_NO_MEMORY; return 0; }
    if (reader_copy(reader, body, len) != len) {
        *error_out = reader->error ? reader->error : IRON_ERR_HTTP_TRUNCATED_MESSAGE;
        free(body);
        return 0;
    }
    body[len] = 0;
    *body_out = http_slice((const char *)body, len);
    free(body);
    return 1;
}

static int read_close_body(HttpReader *reader, size_t max_body,
                           Iron_String *body_out, int64_t *error_out) {
    uint8_t *body = (uint8_t *)malloc(max_body + 1);
    if (!body) { *error_out = IRON_ERR_NET_NO_MEMORY; return 0; }
    size_t used = 0;
    uint8_t b;
    while (reader_byte(reader, &b)) {
        if (used == max_body) {
            *error_out = IRON_ERR_HTTP_BODY_TOO_LARGE;
            free(body);
            return 0;
        }
        body[used++] = b;
    }
    if (reader->error != 0) {
        *error_out = reader->error;
        free(body);
        return 0;
    }
    body[used] = 0;
    *body_out = http_slice((const char *)body, used);
    free(body);
    return 1;
}

static int valid_method(const char *method, size_t len) {
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)method[i];
        if (!http_is_tchar(c)) return 0;
    }
    return 1;
}

static int valid_target_octets(const char *target, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c <= 32 || c >= 127 || c == '#') return 0;
        if (c == '%' &&
            (i + 2 >= len || !http_is_hex((unsigned char)target[i + 1]) ||
             !http_is_hex((unsigned char)target[i + 2]))) return 0;
        if (c == '%') i += 2;
    }
    return 1;
}

static int uri_reg_name_char(unsigned char c) {
    return isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
           c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' ||
           c == ')' || c == '*' || c == '+' || c == ',' || c == ';' ||
           c == '=';
}

static int valid_authority_target(const char *target, size_t len) {
    size_t port_at = 0;
    if (target[0] == '[') {
        size_t close = 1;
        while (close < len && target[close] != ']') close++;
        if (close == 1 || close + 2 >= len || target[close + 1] != ':')
            return 0;
        size_t literal_len = close - 1;
        if ((target[1] == 'v' || target[1] == 'V') && literal_len >= 4) {
            size_t pos = 2;
            size_t hex_start = pos;
            while (pos < close && http_is_hex((unsigned char)target[pos])) pos++;
            if (pos == hex_start || pos >= close || target[pos++] != '.' ||
                pos == close) return 0;
            for (; pos < close; pos++) {
                unsigned char c = (unsigned char)target[pos];
                if (!(uri_reg_name_char(c) || c == ':')) return 0;
            }
        } else {
            if (literal_len >= 64) return 0;
            char literal[64];
            memcpy(literal, target + 1, literal_len);
            literal[literal_len] = 0;
            struct in6_addr address;
#ifdef _WIN32
            if (InetPtonA(AF_INET6, literal, &address) != 1) return 0;
#else
            if (inet_pton(AF_INET6, literal, &address) != 1) return 0;
#endif
        }
        port_at = close + 2;
    } else {
        size_t colon = 0;
        while (colon < len && target[colon] != ':') {
            unsigned char c = (unsigned char)target[colon];
            if (c == '%') colon += 3;
            else {
                if (!uri_reg_name_char(c)) return 0;
                colon++;
            }
        }
        if (colon == 0 || colon >= len) return 0;
        port_at = colon + 1;
    }
    if (port_at >= len) return 0;
    for (size_t i = port_at; i < len; i++)
        if (target[i] < '0' || target[i] > '9') return 0;
    return 1;
}

static int valid_request_target(const char *method, size_t method_len,
                                const char *target, size_t len) {
    if (len == 0 || !valid_target_octets(target, len)) return 0;
    if (len == 1 && target[0] == '*')
        return ascii_ieq_n(method, method_len, "OPTIONS", strlen("OPTIONS"));
    if (ascii_ieq_n(method, method_len, "CONNECT", strlen("CONNECT"))) {
        return valid_authority_target(target, len);
    }
    if (target[0] == '/') return 1;
    size_t scheme_end = 0;
    if (!isalpha((unsigned char)target[0])) return 0;
    while (scheme_end < len && target[scheme_end] != ':') {
        unsigned char c = (unsigned char)target[scheme_end];
        if (!(isalnum(c) || c == '+' || c == '-' || c == '.')) return 0;
        scheme_end++;
    }
    return scheme_end + 3 < len && target[scheme_end] == ':' &&
           target[scheme_end + 1] == '/' && target[scheme_end + 2] == '/' &&
           target[scheme_end + 3] != '/' && target[scheme_end + 3] != '?';
}

static void request_target_path_query(const char *method, size_t method_len,
                                      const char *target, size_t target_len,
                                      size_t *path_at, size_t *path_len,
                                      size_t *query_at, size_t *query_len) {
    size_t start = 0;
    if (target_len == 1 && target[0] == '*') {
        *path_at = 0; *path_len = 1; *query_at = 1; *query_len = 0;
        return;
    }
    if (ascii_ieq_n(method, method_len, "CONNECT", strlen("CONNECT"))) {
        *path_at = 0; *path_len = target_len;
        *query_at = target_len; *query_len = 0;
        return;
    }
    if (target[0] != '/') {
        size_t authority = 0;
        while (authority + 2 < target_len &&
               !(target[authority] == ':' && target[authority + 1] == '/' &&
                 target[authority + 2] == '/')) authority++;
        start = authority + 3;
        while (start < target_len && target[start] != '/' &&
               target[start] != '?') start++;
        if (start == target_len || target[start] == '?') {
            *path_at = target_len; *path_len = 0;
        }
    }
    size_t q = start;
    while (q < target_len && target[q] != '?') q++;
    *path_at = start;
    *path_len = q - start;
    *query_at = q < target_len ? q + 1 : target_len;
    *query_len = q < target_len ? target_len - q - 1 : 0;
}

typedef struct ParsedUrl {
    char host[HTTP_MAX_HOST + 1];
    char host_header[HTTP_MAX_HOST + 16];
    char target[HTTP_MAX_URL + 1];
    int64_t port;
    int secure;
    int origin_only;
    int64_t error;
} ParsedUrl;

static ParsedUrl parse_http_url(Iron_String url) {
    ParsedUrl out;
    memset(&out, 0, sizeof(out));
    const char *s = iron_string_cstr(&url);
    size_t n = iron_string_byte_len(&url);
    if (n == 0 || n > HTTP_MAX_URL || memchr(s, '\0', n) != NULL) {
        out.error = IRON_ERR_HTTP_BAD_URL;
        return out;
    }

    size_t pos = 0;
    if (n >= 7 && ascii_ieq_n(s, 7, "http://", 7)) pos = 7;
    else if (n >= 8 && ascii_ieq_n(s, 8, "https://", 8)) {
        pos = 8;
        out.secure = 1;
    } else {
        out.error = strstr(s, "://") != NULL
            ? IRON_ERR_HTTP_UNSUPPORTED_SCHEME : IRON_ERR_HTTP_BAD_URL;
        return out;
    }

    size_t authority_end = pos;
    while (authority_end < n && s[authority_end] != '/' &&
           s[authority_end] != '?' && s[authority_end] != '#') authority_end++;
    if (authority_end == pos) { out.error = IRON_ERR_HTTP_BAD_URL; return out; }
    for (size_t i = pos; i < authority_end; i++) {
        if (s[i] == '@' || (unsigned char)s[i] <= 32) {
            out.error = IRON_ERR_HTTP_BAD_URL;
            return out;
        }
    }

    size_t host_start = pos, host_end = authority_end, port_start = SIZE_MAX;
    int bracketed = 0;
    if (s[pos] == '[') {
        bracketed = 1;
        host_start = pos + 1;
        host_end = host_start;
        while (host_end < authority_end && s[host_end] != ']') host_end++;
        if (host_end == authority_end || host_end == host_start) {
            out.error = IRON_ERR_HTTP_BAD_URL;
            return out;
        }
        if (host_end + 1 < authority_end) {
            if (s[host_end + 1] != ':') { out.error = IRON_ERR_HTTP_BAD_URL; return out; }
            port_start = host_end + 2;
        }
    } else {
        for (size_t i = pos; i < authority_end; i++) {
            if (s[i] == ':') {
                if (port_start != SIZE_MAX) { out.error = IRON_ERR_HTTP_BAD_URL; return out; }
                host_end = i;
                port_start = i + 1;
            }
        }
    }
    size_t host_len = host_end - host_start;
    if (host_len == 0 || host_len > HTTP_MAX_HOST) { out.error = IRON_ERR_HTTP_BAD_URL; return out; }
    memcpy(out.host, s + host_start, host_len);
    out.host[host_len] = 0;
    out.port = out.secure ? 443 : 80;
    if (port_start != SIZE_MAX) {
        if (port_start >= authority_end) { out.error = IRON_ERR_HTTP_BAD_URL; return out; }
        int64_t port = 0;
        for (size_t i = port_start; i < authority_end; i++) {
            if (s[i] < '0' || s[i] > '9') { out.error = IRON_ERR_HTTP_BAD_URL; return out; }
            port = port * 10 + (s[i] - '0');
            if (port > 65535) { out.error = IRON_ERR_HTTP_BAD_URL; return out; }
        }
        if (port == 0) { out.error = IRON_ERR_HTTP_BAD_URL; return out; }
        out.port = port;
    }

    int default_port = (!out.secure && out.port == 80) ||
                       (out.secure && out.port == 443);
    if (bracketed) {
        if (default_port) snprintf(out.host_header, sizeof(out.host_header), "[%s]", out.host);
        else snprintf(out.host_header, sizeof(out.host_header), "[%s]:%lld", out.host, (long long)out.port);
    } else {
        if (default_port) snprintf(out.host_header, sizeof(out.host_header), "%s", out.host);
        else snprintf(out.host_header, sizeof(out.host_header), "%s:%lld", out.host, (long long)out.port);
    }

    size_t target_start = authority_end;
    out.origin_only = target_start == n ||
        (target_start + 1 == n && s[target_start] == '/');
    size_t fragment = target_start;
    while (fragment < n && s[fragment] != '#') fragment++;
    size_t target_len = fragment - target_start;
    if (target_len == 0) {
        strcpy(out.target, "/");
    } else if (s[target_start] == '?') {
        if (target_len + 1 > HTTP_MAX_URL) { out.error = IRON_ERR_HTTP_BAD_URL; return out; }
        out.target[0] = '/';
        memcpy(out.target + 1, s + target_start, target_len);
        out.target[target_len + 1] = 0;
    } else {
        if (s[target_start] != '/' || target_len > HTTP_MAX_URL) {
            out.error = IRON_ERR_HTTP_BAD_URL;
            return out;
        }
        memcpy(out.target, s + target_start, target_len);
        out.target[target_len] = 0;
    }
    for (size_t i = 0; out.target[i]; i++) {
        unsigned char c = (unsigned char)out.target[i];
        if (c <= 32 || c == 127) { out.error = IRON_ERR_HTTP_BAD_URL; return out; }
    }
    return out;
}

Iron_HttpServerResult Iron_http_listen(Iron_String host, int64_t port) {
    Iron_HttpServerResult out;
    memset(&out, 0, sizeof(out));
    out.server.fd = -1;
    out.error_message = http_str("");
    Iron_Result_TcpListener_Error lr = Iron_net_tcp_listen(host, port);
    out.server.fd = lr.v0.fd;
    out.error = lr.v1.code;
    if (out.error) out.error_message = http_str(http_error_message(out.error));
    return out;
}

Iron_HttpConnectionResult Iron_httpserver_accept(Iron_HttpServer server,
                                                  int64_t timeout) {
    Iron_HttpConnectionResult out;
    memset(&out, 0, sizeof(out));
    out.connection.fd = -1;
    out.error_message = http_str("");
    Iron_TcpListener listener = { server.fd };
    Iron_Result_TcpSocket_Error ar = Iron_tcplistener_accept(listener, timeout);
    out.connection.fd = ar.v0.fd;
    out.error = ar.v1.code;
    if (out.error) out.error_message = http_str(http_error_message(out.error));
    return out;
}

int64_t Iron_httpserver_port(Iron_HttpServer server) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
#ifdef _WIN32
    int len = (int)sizeof(ss);
    if (getsockname((SOCKET)server.fd, (struct sockaddr *)&ss, &len) != 0) return -1;
#else
    socklen_t len = (socklen_t)sizeof(ss);
    if (getsockname((int)server.fd, (struct sockaddr *)&ss, &len) != 0) return -1;
#endif
    if (ss.ss_family == AF_INET) return (int64_t)ntohs(((struct sockaddr_in *)&ss)->sin_port);
    if (ss.ss_family == AF_INET6) return (int64_t)ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    return -1;
}

void Iron_httpserver_close(Iron_HttpServer server) {
    Iron_TcpListener l = { server.fd };
    Iron_tcplistener_close(l);
}

void Iron_httpconnection_close(Iron_HttpConnection connection) {
    /* A handler often closes right after answering a request it rejected
     * (too large, malformed) with part of it still unread: a plain close
     * would reset the connection and the client could lose the answer. */
    http_pending_clear(connection.fd);
    iron_net_lingering_close(connection.fd);
}

Iron_HttpsServerResult Iron_http_listen_tls(Iron_String host, int64_t port,
                                             Iron_String certificate_file,
                                             Iron_String private_key_file) {
    Iron_HttpsServerResult out;
    memset(&out, 0, sizeof(out));
    out.server.fd = -1;
    out.error_message = http_str("");
    Iron_TlsContextResult tls = iron_tls_server_context_new(
        certificate_file, private_key_file);
    if (tls.error.code != 0) {
        out.error = tls.error.code;
        out.error_message = http_str(http_error_message(out.error));
        return out;
    }
    Iron_Result_TcpListener_Error listener = Iron_net_tcp_listen(host, port);
    if (listener.v1.code != 0) {
        iron_tls_server_context_free(tls.context);
        out.error = listener.v1.code;
        out.error_message = http_str(http_error_message(out.error));
        return out;
    }
    out.server.fd = listener.v0.fd;
    out.server.context = (int64_t)(intptr_t)tls.context;
    return out;
}

Iron_HttpsPendingConnectionResult Iron_httpsserver_accept_tcp(
    Iron_HttpsServer server, int64_t timeout) {
    Iron_HttpsPendingConnectionResult out;
    memset(&out, 0, sizeof(out));
    out.connection.fd = -1;
    out.error_message = http_str("");
    if (timeout < 0) {
        out.error = IRON_ERR_HTTP_INVALID_ARGUMENT;
        out.error_message = http_str(http_error_message(out.error));
        return out;
    }
    Iron_TlsServerContext *context =
        (Iron_TlsServerContext *)(intptr_t)server.context;
    if (server.fd < 0 || !iron_tls_server_context_retain(context)) {
        out.error = IRON_ERR_TLS_CONTEXT;
        out.error_message = http_str(http_error_message(out.error));
        return out;
    }
    Iron_TcpListener listener = { server.fd };
    Iron_Result_TcpSocket_Error accepted = Iron_tcplistener_accept(
        listener, timeout);
    if (accepted.v1.code != 0) {
        iron_tls_server_context_free(context);
        out.error = accepted.v1.code;
        out.error_message = http_str(http_error_message(out.error));
        return out;
    }
    out.connection.fd = accepted.v0.fd;
    out.connection.context = (int64_t)(intptr_t)context;
    return out;
}

Iron_HttpsConnectionResult Iron_httpspendingconnection_handshake(
    Iron_HttpsPendingConnection connection, int64_t timeout) {
    Iron_HttpsConnectionResult out;
    memset(&out, 0, sizeof(out));
    out.connection.fd = -1;
    out.error_message = http_str("");
    Iron_TcpSocket socket = { connection.fd };
    Iron_TlsServerContext *context =
        (Iron_TlsServerContext *)(intptr_t)connection.context;
    if (timeout < 0 || socket.fd < 0 || !context) {
        out.error = IRON_ERR_HTTP_INVALID_ARGUMENT;
        out.error_message = http_str(http_error_message(out.error));
        if (socket.fd >= 0) Iron_tcpsocket_close(socket);
        if (context) iron_tls_server_context_free(context);
        return out;
    }
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
    Iron_TlsStreamResult tls = iron_tls_server_accept(
        context, socket, deadline);
    iron_tls_server_context_free(context);
    if (tls.error.code != 0) {
        Iron_tcpsocket_close(socket);
        out.error = tls.error.code;
        out.error_message = http_str(http_error_message(out.error));
        return out;
    }
    out.connection.fd = socket.fd;
    out.connection.tls = (int64_t)(intptr_t)tls.stream;
    return out;
}

void Iron_httpspendingconnection_close(Iron_HttpsPendingConnection connection) {
    http_pending_clear(connection.fd);
    Iron_TcpSocket socket = { connection.fd };
    if (socket.fd >= 0) Iron_tcpsocket_close(socket);
    iron_tls_server_context_free(
        (Iron_TlsServerContext *)(intptr_t)connection.context);
}

Iron_HttpsConnectionResult Iron_httpsserver_accept(Iron_HttpsServer server,
                                                    int64_t timeout) {
    Iron_HttpsConnectionResult out;
    memset(&out, 0, sizeof(out));
    out.connection.fd = -1;
    out.error_message = http_str("");
    if (timeout < 0) {
        out.error = IRON_ERR_HTTP_INVALID_ARGUMENT;
        out.error_message = http_str(http_error_message(out.error));
        return out;
    }
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
    Iron_HttpsPendingConnectionResult accepted = Iron_httpsserver_accept_tcp(
        server, Iron_deadline_remaining_ms(deadline));
    if (accepted.error != 0) {
        out.error = accepted.error;
        out.error_message = accepted.error_message;
        return out;
    }
    return Iron_httpspendingconnection_handshake(
        accepted.connection, Iron_deadline_remaining_ms(deadline));
}

int64_t Iron_httpsserver_port(Iron_HttpsServer server) {
    Iron_HttpServer plain = { server.fd };
    return Iron_httpserver_port(plain);
}

void Iron_httpsserver_close(Iron_HttpsServer server) {
    Iron_TcpListener listener = { server.fd };
    Iron_tcplistener_close(listener);
    iron_tls_server_context_free(
        (Iron_TlsServerContext *)(intptr_t)server.context);
}

void Iron_httpsconnection_close(Iron_HttpsConnection connection) {
    http_pending_clear(connection.fd);
    iron_tls_stream_close((Iron_TlsStream *)(intptr_t)connection.tls);
    Iron_TcpSocket socket = { connection.fd };
    Iron_tcpsocket_close(socket);
}

Iron_String Iron_http_header(Iron_String headers, Iron_String name) {
    const char *value = NULL;
    size_t value_len = 0;
    if (!header_value_span(iron_string_cstr(&headers), iron_string_byte_len(&headers),
                           iron_string_cstr(&name), &value, &value_len)) return http_str("");
    return http_slice(value, value_len);
}

static Iron_HttpRequest read_request_transport(HttpTransport transport,
                                                int64_t max_header_bytes,
                                                int64_t max_body_bytes,
                                                int64_t timeout) {
    Iron_HttpRequest out = http_request_empty();
    if (max_header_bytes < 16 || max_body_bytes < 0 || timeout < 0 ||
        (uint64_t)max_header_bytes > (uint64_t)(SIZE_MAX - 1) ||
        (uint64_t)max_body_bytes > (uint64_t)(SIZE_MAX - 1)) {
        http_request_error(&out, IRON_ERR_HTTP_INVALID_ARGUMENT);
        return out;
    }
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
    HttpHead h = receive_head(transport, (size_t)max_header_bytes, deadline);
    if (h.error) {
        http_request_error(&out, h.error);
        free(h.data);
        return out;
    }
    const char *data = (const char *)h.data;
    /* The start line ends at the first CRLF, which is the one at the
     * marker itself when the message has no header lines. */
    size_t line_end = find_crlf(data, 0, h.marker + 2);
    if (line_end == SIZE_MAX) {
        http_request_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    size_t sp1 = 0;
    while (sp1 < line_end && data[sp1] != ' ') sp1++;
    size_t sp2 = sp1 + 1;
    while (sp2 < line_end && data[sp2] != ' ') sp2++;
    int version_11 = sp2 < line_end && line_end - sp2 - 1 == 8 &&
        memcmp(data + sp2 + 1, "HTTP/1.1", 8) == 0;
    int version_10 = sp2 < line_end && line_end - sp2 - 1 == 8 &&
        memcmp(data + sp2 + 1, "HTTP/1.0", 8) == 0;
    if (sp1 == 0 || sp1 >= line_end || sp2 <= sp1 + 1 || sp2 >= line_end ||
        !valid_method(data, sp1) || (!version_11 && !version_10)) {
        http_request_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    size_t target_len = sp2 - sp1 - 1;
    const char *target = data + sp1 + 1;
    if (!valid_request_target(data, sp1, target, target_len)) {
        http_request_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    out.method = http_slice(data, sp1);
    out.target = http_slice(target, target_len);
    out.version = http_slice(data + sp2 + 1, line_end - sp2 - 1);
    size_t path_at = 0, path_len = 0, query_at = 0, query_len = 0;
    request_target_path_query(data, sp1, target, target_len,
                              &path_at, &path_len, &query_at, &query_len);
    out.path = http_slice(target + path_at, path_len);
    out.query = http_slice(target + query_at, query_len);

    size_t headers_start = line_end + 2;
    size_t headers_len = h.marker >= headers_start ? h.marker - headers_start : 0;
    HttpHeaderScan sc;
    http_scan_headers(data + headers_start, headers_len, &sc);
    if (!sc.valid ||
        (version_11 ? sc.host_count != 1 : sc.host_count > 1) ||
        (sc.host_count > 0 && sc.host_len == 0)) {
        http_request_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    out.headers = http_slice(data + headers_start, headers_len);
    int requests_close = header_has_token(
        data + headers_start, headers_len, "Connection", "close");
    int requests_keep_alive = header_has_token(
        data + headers_start, headers_len, "Connection", "keep-alive");
    out.keep_alive = version_11 ? !requests_close
                                : requests_keep_alive && !requests_close;
    size_t content_len = 0;
    int content_present = 0;
    if (!parse_content_length(data + headers_start, headers_len,
                              &content_len, &content_present)) {
        http_request_error(&out, IRON_ERR_HTTP_BAD_CONTENT_LENGTH);
        free(h.data);
        return out;
    }
    int has_te = 0;
    int chunked = 0;
    if (!parse_transfer_encoding(data + headers_start, headers_len,
                                 &has_te, &chunked)) {
        http_request_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    if (has_te && content_present) {
        http_request_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    if (has_te && !chunked) {
        http_request_error(&out, IRON_ERR_HTTP_UNSUPPORTED_TRANSFER);
        free(h.data);
        return out;
    }
    if (content_len > (size_t)max_body_bytes) {
        http_request_error(&out, IRON_ERR_HTTP_BODY_TOO_LARGE);
        free(h.data);
        return out;
    }
    const char *expect_value = NULL;
    size_t expect_len = 0;
    int expect_count = header_count(data + headers_start, headers_len, "Expect");
    int expect_continue = expect_count == 1 &&
        header_value_span(data + headers_start, headers_len, "Expect",
                          &expect_value, &expect_len) &&
        ascii_ieq_n(expect_value, expect_len,
                    "100-continue", strlen("100-continue"));
    if (expect_count > 0 && !expect_continue) {
        http_request_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    if (expect_continue && (chunked || (content_present && content_len > 0))) {
        int64_t send_error = send_all(
            transport, (const uint8_t *)"HTTP/1.1 100 Continue\r\n\r\n",
            strlen("HTTP/1.1 100 Continue\r\n\r\n"), deadline);
        if (send_error != 0) {
            http_request_error(&out, send_error);
            free(h.data);
            return out;
        }
    }
    HttpReader reader;
    memset(&reader, 0, sizeof(reader));
    reader.transport = transport;
    reader.prefix = h.data + h.marker + 4;
    reader.prefix_len = h.len - (h.marker + 4);
    reader.deadline = deadline;
    int64_t read_error = 0;
    if (chunked &&
        !read_chunked_body(&reader, (size_t)max_body_bytes,
                           (size_t)max_header_bytes, &out.body, &read_error)) {
        http_request_error(&out, read_error);
    } else if (content_present && content_len > 0 &&
               !read_fixed_body(&reader, content_len, &out.body, &read_error)) {
        http_request_error(&out, read_error);
    }
    if (out.error == 0) reader_save_pending(&reader);
    free(h.data);
    return out;
}

Iron_HttpRequest Iron_httpconnection_read_request(Iron_HttpConnection connection,
                                                   int64_t max_header_bytes,
                                                   int64_t max_body_bytes,
                                                   int64_t timeout) {
    HttpTransport transport = { { connection.fd }, NULL, NULL };
    return read_request_transport(transport, max_header_bytes, max_body_bytes,
                                  timeout);
}

Iron_HttpRequest Iron_httpsconnection_read_request(Iron_HttpsConnection connection,
                                                    int64_t max_header_bytes,
                                                    int64_t max_body_bytes,
                                                    int64_t timeout) {
    HttpTransport transport = {
        { connection.fd }, (Iron_TlsStream *)(intptr_t)connection.tls, NULL
    };
    return read_request_transport(transport, max_header_bytes, max_body_bytes,
                                  timeout);
}

static const char *status_reason(int64_t status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Status";
    }
}

static Iron_HttpResponse http_response_owned(int64_t status,
                                              Iron_String headers,
                                              Iron_String body) {
    Iron_HttpResponse out = http_response_empty();
    out.status = status;
    out.reason = http_str(status_reason(status));
    out.headers = headers;
    out.body = body;
    if (status < 100 || status > 999 ||
        !raw_headers_valid(iron_string_cstr(&headers), iron_string_byte_len(&headers))) {
        http_response_error(&out, IRON_ERR_HTTP_INVALID_ARGUMENT);
    }
    return out;
}

/* Strings are immutable and reference counted: a response shares the
 * caller's header and body strings (one more reference) instead of copying
 * the body. */
static Iron_String http_share(Iron_String s) {
    iron_string_retain(&s);
    return s;
}

Iron_HttpResponse Iron_http_response(int64_t status, Iron_String headers,
                                      Iron_String body) {
    return http_response_owned(status, http_share(headers), http_share(body));
}

Iron_HttpResponse Iron_http_json_response(int64_t status, Iron_String body) {
    return http_response_owned(
        status, http_str("Content-Type: application/json; charset=utf-8"),
        http_share(body));
}

Iron_HttpResponse Iron_http_html_response(int64_t status, Iron_String body) {
    return http_response_owned(
        status, http_str("Content-Type: text/html; charset=utf-8"),
        http_share(body));
}

Iron_HttpResponse Iron_http_text_response(int64_t status, Iron_String body) {
    return http_response_owned(
        status, http_str("Content-Type: text/plain; charset=utf-8"),
        http_share(body));
}

Iron_HttpResponse Iron_http_file_response(int64_t status, Iron_String path,
                                           Iron_String content_type,
                                           int64_t max_body_bytes) {
    Iron_HttpResponse out = http_response_empty();
    if (max_body_bytes < 0) {
        http_response_error(&out, IRON_ERR_HTTP_INVALID_ARGUMENT);
        return out;
    }
    FILE *f = fopen(iron_string_cstr(&path), "rb");
    if (!f || fseek(f, 0, SEEK_END) != 0) {
        if (f) fclose(f);
        http_response_error(&out, IRON_ERR_HTTP_FILE);
        return out;
    }
    long end = ftell(f);
    if (end < 0 || (uint64_t)end > (uint64_t)max_body_bytes || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        http_response_error(&out, end >= 0 ? IRON_ERR_HTTP_BODY_TOO_LARGE : IRON_ERR_HTTP_FILE);
        return out;
    }
    size_t len = (size_t)end;
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        fclose(f);
        http_response_error(&out, IRON_ERR_NET_NO_MEMORY);
        return out;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) {
        free(buf);
        http_response_error(&out, IRON_ERR_HTTP_FILE);
        return out;
    }
    buf[len] = 0;
    size_t ct_len = iron_string_byte_len(&content_type);
    const char *ct = iron_string_cstr(&content_type);
    size_t header_len = strlen("Content-Type: ") + ct_len;
    char *header = (char *)malloc(header_len + 1);
    if (!header) {
        free(buf);
        http_response_error(&out, IRON_ERR_NET_NO_MEMORY);
        return out;
    }
    memcpy(header, "Content-Type: ", strlen("Content-Type: "));
    memcpy(header + strlen("Content-Type: "), ct, ct_len);
    header[header_len] = 0;
    Iron_String header_s = http_slice(header, header_len);
    Iron_String body_s = http_slice(buf, len);
    free(header);
    free(buf);
    return http_response_owned(status, header_s, body_s);
}

/* Format the generated response fields without snprintf, which showed in
 * profiles. Informational and 204 responses omit Content-Length. */
static int http_status_head(char *out, size_t cap, int64_t status,
                            const char *reason, size_t reason_len,
                            size_t body_len, bool keep_alive) {
    int include_length = !((status >= 100 && status < 200) || status == 204);
    char num[24];
    int nl = 0;
    size_t v = body_len;
    do { num[nl++] = (char)('0' + v % 10); v /= 10; } while (v && nl < 22);
    const char *conn = keep_alive ? "keep-alive" : "close";
    size_t conn_len = keep_alive ? 10 : 5;
    size_t need = 9 + 3 + 1 + reason_len +
                  (include_length ? 18 + (size_t)nl : 0) +
                  14 + conn_len + 2;
    if (need >= cap || status < 100 || status > 999) return -1;
    char *p = out;
    memcpy(p, "HTTP/1.1 ", 9); p += 9;
    *p++ = (char)('0' + status / 100);
    *p++ = (char)('0' + status / 10 % 10);
    *p++ = (char)('0' + status % 10);
    *p++ = ' ';
    memcpy(p, reason, reason_len); p += reason_len;
    if (include_length) {
        memcpy(p, "\r\nContent-Length: ", 18); p += 18;
        while (nl) *p++ = num[--nl];
    }
    memcpy(p, "\r\nConnection: ", 14); p += 14;
    memcpy(p, conn, conn_len); p += conn_len;
    *p++ = '\r';
    *p++ = '\n';
    return (int)(p - out);
}

static int64_t send_response_transport(HttpTransport transport,
                                       Iron_HttpResponse response,
                                       bool keep_alive,
                                       int64_t timeout) {
    if (response.error != 0) return response.error;
    if (timeout < 0) return IRON_ERR_HTTP_INVALID_ARGUMENT;
    const char *headers = iron_string_cstr(&response.headers);
    size_t headers_len = iron_string_byte_len(&response.headers);
    if (response.status < 100 || response.status > 999 ||
        !raw_headers_valid(headers, headers_len) ||
        headers_have_forbidden_framing(headers, headers_len)) {
        return IRON_ERR_HTTP_INVALID_ARGUMENT;
    }
    const char *reason = iron_string_cstr(&response.reason);
    size_t reason_len = iron_string_byte_len(&response.reason);
    for (size_t i = 0; i < reason_len; i++) {
        if ((unsigned char)reason[i] < 32 || reason[i] == 127) return IRON_ERR_HTTP_INVALID_ARGUMENT;
    }
    const char *body = iron_string_cstr(&response.body);
    size_t body_len = iron_string_byte_len(&response.body);
    int forbids_body = (response.status >= 100 && response.status < 200) ||
                       response.status == 204 || response.status == 304;
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
    char fixed[256];
    int fixed_len = http_status_head(fixed, sizeof(fixed), response.status,
                                     reason, reason_len, body_len, keep_alive);
    if (fixed_len < 0) return IRON_ERR_HTTP_INVALID_ARGUMENT;
    /* One write for the whole message: separate small writes for the status
     * line, headers and body meet Nagle's algorithm and the peer's delayed
     * ACK on a keep-alive connection and stall each response by ~40 ms. */
    int headers_need_crlf = headers_len &&
        (headers_len < 2 || headers[headers_len - 2] != '\r' || headers[headers_len - 1] != '\n');
    size_t sent_body_len = forbids_body ? 0 : body_len;
    size_t total = (size_t)fixed_len;
    if (headers_len > SIZE_MAX - total) return IRON_ERR_HTTP_INVALID_ARGUMENT;
    total += headers_len;
    if (headers_need_crlf) {
        if (total > SIZE_MAX - 2) return IRON_ERR_HTTP_INVALID_ARGUMENT;
        total += 2;
    }
    if (total > SIZE_MAX - 2) return IRON_ERR_HTTP_INVALID_ARGUMENT;
    total += 2;
    if (sent_body_len > SIZE_MAX - total) return IRON_ERR_HTTP_INVALID_ARGUMENT;
    total += sent_body_len;
    uint8_t stack_buf[16384];
    uint8_t *msg = total <= sizeof(stack_buf) ? stack_buf : (uint8_t *)malloc(total);
    if (!msg) return IRON_ERR_NET_NO_MEMORY;
    size_t at = 0;
    memcpy(msg + at, fixed, (size_t)fixed_len);
    at += (size_t)fixed_len;
    if (headers_len) {
        memcpy(msg + at, headers, headers_len);
        at += headers_len;
        if (headers_need_crlf) {
            memcpy(msg + at, "\r\n", 2);
            at += 2;
        }
    }
    memcpy(msg + at, "\r\n", 2);
    at += 2;
    if (sent_body_len) memcpy(msg + at, body, sent_body_len);
    int64_t err = send_all(transport, msg, total, deadline);
    if (msg != stack_buf) free(msg);
    return err;
}

int64_t Iron_httpconnection_send_response(Iron_HttpConnection connection,
                                           Iron_HttpResponse response,
                                           int64_t timeout) {
    HttpTransport transport = { { connection.fd }, NULL, NULL };
    return send_response_transport(transport, response, false, timeout);
}

int64_t Iron_httpconnection_send_response_keep_alive(
    Iron_HttpConnection connection, Iron_HttpResponse response,
    bool keep_alive, int64_t timeout) {
    HttpTransport transport = { { connection.fd }, NULL, NULL };
    return send_response_transport(transport, response, keep_alive, timeout);
}

int64_t Iron_httpsconnection_send_response(Iron_HttpsConnection connection,
                                            Iron_HttpResponse response,
                                            int64_t timeout) {
    HttpTransport transport = {
        { connection.fd }, (Iron_TlsStream *)(intptr_t)connection.tls, NULL
    };
    return send_response_transport(transport, response, false, timeout);
}

int64_t Iron_httpsconnection_send_response_keep_alive(
    Iron_HttpsConnection connection, Iron_HttpResponse response,
    bool keep_alive, int64_t timeout) {
    HttpTransport transport = {
        { connection.fd }, (Iron_TlsStream *)(intptr_t)connection.tls, NULL
    };
    return send_response_transport(transport, response, keep_alive, timeout);
}

static Iron_HttpResponse read_one_client_response(HttpTransport transport,
                                                   size_t max_body,
                                                   Iron_Deadline deadline,
                                                   int suppress_body) {
    Iron_HttpResponse out = http_response_empty();
    HttpHead h = receive_head(transport, HTTP_DEFAULT_MAX_HEADER, deadline);
    if (h.error) {
        http_response_error(&out, h.error);
        free(h.data);
        return out;
    }
    const char *data = (const char *)h.data;
    /* The start line ends at the first CRLF, which is the one at the
     * marker itself when the message has no header lines. */
    size_t line_end = find_crlf(data, 0, h.marker + 2);
    int response_version_11 = line_end != SIZE_MAX && line_end >= 8 &&
        memcmp(data, "HTTP/1.1", 8) == 0;
    int response_version_10 = line_end != SIZE_MAX && line_end >= 8 &&
        memcmp(data, "HTTP/1.0", 8) == 0;
    int supported_version = response_version_11 || response_version_10;
    if (line_end == SIZE_MAX || line_end < 13 || !supported_version ||
        data[8] != ' ' || data[12] != ' ') {
        http_response_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    size_t status_end = 9;
    while (status_end < line_end && data[status_end] != ' ') status_end++;
    if (status_end != 12 || !isdigit((unsigned char)data[9]) ||
        !isdigit((unsigned char)data[10]) || !isdigit((unsigned char)data[11])) {
        http_response_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    out.status = (data[9] - '0') * 100 + (data[10] - '0') * 10 + (data[11] - '0');
    if (out.status < 100 || out.status > 599) {
        http_response_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    out.reason = status_end < line_end
        ? http_slice(data + status_end + 1, line_end - status_end - 1)
        : http_str("");
    for (size_t i = status_end + 1; i < line_end; i++) {
        unsigned char c = (unsigned char)data[i];
        if ((c < 32 && c != '\t') || c == 127) {
            http_response_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
            free(h.data);
            return out;
        }
    }
    size_t headers_start = line_end + 2;
    size_t headers_len = h.marker >= headers_start ? h.marker - headers_start : 0;
    if (!raw_headers_valid(data + headers_start, headers_len)) {
        http_response_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    out.headers = http_slice(data + headers_start, headers_len);

    size_t content_len = 0;
    int content_present = 0;
    if (!parse_content_length(data + headers_start, headers_len,
                              &content_len, &content_present)) {
        http_response_error(&out, IRON_ERR_HTTP_BAD_CONTENT_LENGTH);
        free(h.data);
        return out;
    }
    int has_te = 0;
    int chunked = 0;
    if (!parse_transfer_encoding(data + headers_start, headers_len,
                                 &has_te, &chunked)) {
        http_response_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    if (has_te && !chunked) {
        http_response_error(&out, IRON_ERR_HTTP_UNSUPPORTED_TRANSFER);
        free(h.data);
        return out;
    }
    if (chunked && content_present) {
        http_response_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    if (((out.status >= 100 && out.status < 200) || out.status == 204) &&
        (content_present || has_te)) {
        http_response_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    if (content_present && content_len > max_body) {
        http_response_error(&out, IRON_ERR_HTTP_BODY_TOO_LARGE);
        free(h.data);
        return out;
    }
    HttpReader reader;
    memset(&reader, 0, sizeof(reader));
    reader.transport = transport;
    reader.prefix = h.data + h.marker + 4;
    reader.prefix_len = h.len - (h.marker + 4);
    reader.deadline = deadline;
    int response_closes = header_has_token(
        data + headers_start, headers_len, "Connection", "close");
    int response_keeps_alive = header_has_token(
        data + headers_start, headers_len, "Connection", "keep-alive");
    int persistent = response_version_11 ? !response_closes
        : response_keeps_alive && !response_closes;
    int framed_for_reuse = suppress_body || chunked || content_present ||
        (out.status >= 100 && out.status < 200) || out.status == 204 ||
        out.status == 304;
    int64_t read_error = 0;
    int ok;
    if (suppress_body || (out.status >= 100 && out.status < 200) ||
        out.status == 204 || out.status == 304) ok = 1;
    else if (chunked) ok = read_chunked_body(&reader, max_body,
                                        HTTP_DEFAULT_MAX_HEADER,
                                        &out.body, &read_error);
    else if (content_present) ok = read_fixed_body(&reader, content_len, &out.body, &read_error);
    else ok = read_close_body(&reader, max_body, &out.body, &read_error);
    if (!ok) http_response_error(&out, read_error);
    else {
        out.keep_alive = persistent && framed_for_reuse;
        reader_save_pending(&reader);
    }
    free(h.data);
    return out;
}

static Iron_HttpResponse read_client_response(HttpTransport transport,
                                               size_t max_body,
                                               Iron_Deadline deadline,
                                               int suppress_body) {
    for (int informational = 0; informational < 16; informational++) {
        Iron_HttpResponse out = read_one_client_response(
            transport, max_body, deadline, suppress_body);
        if (out.error != 0 || out.status < 100 || out.status >= 200 ||
            out.status == 101) return out;
        Iron_httpresponse_dispose(out);
    }
    Iron_HttpResponse out = http_response_empty();
    http_response_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
    return out;
}

static Iron_HttpResponse http_request_tls_options(
    Iron_String method, Iron_String url, Iron_String headers, Iron_String body,
    int64_t max_body_bytes, int64_t timeout, Iron_String ca_file,
    bool insecure) {
    Iron_HttpResponse out = http_response_empty();
    const char *method_c = iron_string_cstr(&method);
    size_t method_len = iron_string_byte_len(&method);
    const char *headers_c = iron_string_cstr(&headers);
    size_t headers_len = iron_string_byte_len(&headers);
    size_t body_len = iron_string_byte_len(&body);
    if (max_body_bytes < 0 || timeout < 0 ||
        (uint64_t)max_body_bytes > (uint64_t)(SIZE_MAX - 1) ||
        !valid_method(method_c, method_len) ||
        !raw_headers_valid(headers_c, headers_len) ||
        headers_have_forbidden_framing(headers_c, headers_len)) {
        http_response_error(&out, IRON_ERR_HTTP_INVALID_ARGUMENT);
        return out;
    }
    ParsedUrl parsed = parse_http_url(url);
    if (parsed.error) {
        http_response_error(&out, parsed.error);
        return out;
    }
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
    Iron_String host = http_str(parsed.host);
    Iron_Result_TcpSocket_Error dr = Iron_net_tcp_dial(
        host, parsed.port, Iron_deadline_remaining_ms(deadline));
    if (dr.v1.code != 0) {
        iron_string_release(&host);
        http_response_error(&out, dr.v1.code);
        return out;
    }
    http_set_nodelay(dr.v0.fd);
    HttpTransport transport = { dr.v0, NULL, NULL };
    if (parsed.secure) {
        Iron_TlsStreamResult tls = iron_tls_client_connect(
            dr.v0, host, ca_file, insecure, deadline);
        iron_string_release(&host);
        if (tls.error.code != 0) {
            Iron_tcpsocket_close(dr.v0);
            http_response_error(&out, tls.error.code);
            return out;
        }
        transport.tls = tls.stream;
    } else {
        iron_string_release(&host);
    }
    size_t prefix_cap = method_len + strlen(parsed.target) + strlen(parsed.host_header) +
                        headers_len + 256;
    char *prefix = (char *)malloc(prefix_cap);
    if (!prefix) {
        transport_close(transport);
        http_response_error(&out, IRON_ERR_NET_NO_MEMORY);
        return out;
    }
    int n = snprintf(prefix, prefix_cap,
        "%.*s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %zu\r\n",
        (int)method_len, method_c, parsed.target, parsed.host_header, body_len);
    if (n < 0 || (size_t)n >= prefix_cap) {
        free(prefix);
        transport_close(transport);
        http_response_error(&out, IRON_ERR_HTTP_INVALID_ARGUMENT);
        return out;
    }
    char *msg = NULL;
    size_t msg_len = 0;
    int64_t err = http_build_request(&msg, &msg_len, prefix, (size_t)n,
                                     headers_c, headers_len,
                                     iron_string_cstr(&body), body_len);
    free(prefix);
    if (!err) err = send_all(transport, (const uint8_t *)msg, msg_len, deadline);
    free(msg);
    if (err) {
        transport_close(transport);
        http_response_error(&out, err);
        return out;
    }
    out = read_client_response(
        transport, (size_t)max_body_bytes, deadline,
        ascii_ieq_n(method_c, method_len, "HEAD", 4));
    transport_close(transport);
    return out;
}

Iron_HttpResponse Iron_http_request(Iron_String method, Iron_String url,
                                     Iron_String headers, Iron_String body,
                                     int64_t max_body_bytes, int64_t timeout) {
    return http_request_tls_options(method, url, headers, body, max_body_bytes,
                                    timeout, http_str(""), false);
}

Iron_HttpResponse Iron_http_request_with_ca(
    Iron_String method, Iron_String url, Iron_String headers, Iron_String body,
    int64_t max_body_bytes, Iron_String ca_file, int64_t timeout) {
    return http_request_tls_options(method, url, headers, body, max_body_bytes,
                                    timeout, ca_file, false);
}

Iron_HttpResponse Iron_http_request_insecure(
    Iron_String method, Iron_String url, Iron_String headers, Iron_String body,
    int64_t max_body_bytes, int64_t timeout) {
    return http_request_tls_options(method, url, headers, body, max_body_bytes,
                                    timeout, http_str(""), true);
}

Iron_HttpResponse Iron_http_get(Iron_String url, int64_t timeout) {
    return Iron_http_request(http_str("GET"), url, http_str(""), http_str(""),
                             HTTP_DEFAULT_MAX_BODY, timeout);
}

Iron_HttpResponse Iron_http_post_json(Iron_String url, Iron_String body,
                                       int64_t timeout) {
    return Iron_http_request(http_str("POST"), url,
        iron_string_from_literal(
            "Content-Type: application/json; charset=utf-8",
            strlen("Content-Type: application/json; charset=utf-8")), body,
        HTTP_DEFAULT_MAX_BODY, timeout);
}

Iron_HttpResponse Iron_http_get_with_ca(Iron_String url, Iron_String ca_file,
                                        int64_t timeout) {
    return Iron_http_request_with_ca(
        http_str("GET"), url, http_str(""), http_str(""),
        HTTP_DEFAULT_MAX_BODY, ca_file, timeout);
}

Iron_HttpResponse Iron_http_get_insecure(Iron_String url, int64_t timeout) {
    return Iron_http_request_insecure(
        http_str("GET"), url, http_str(""), http_str(""),
        HTTP_DEFAULT_MAX_BODY, timeout);
}

typedef struct HttpClientSlot {
    HttpTransport transport;
    int connected;
    int busy;
    uint64_t last_used_ms;
} HttpClientSlot;

typedef struct HttpClientState {
    ParsedUrl origin;
    char *ca_file;
    size_t ca_file_length;
    int insecure;
    int closing;
    int64_t idle_timeout;
    int64_t slot_count;
    HttpClientSlot *slots;
#ifdef _WIN32
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE available;
#else
    pthread_mutex_t lock;
    pthread_cond_t available;
#endif
} HttpClientState;

static int http_client_lock_init(HttpClientState *client) {
#ifdef _WIN32
    InitializeCriticalSection(&client->lock);
    InitializeConditionVariable(&client->available);
    return 1;
#else
    if (pthread_mutex_init(&client->lock, NULL) != 0) return 0;
    if (pthread_cond_init(&client->available, NULL) != 0) {
        pthread_mutex_destroy(&client->lock);
        return 0;
    }
    return 1;
#endif
}

static void http_client_lock(HttpClientState *client) {
#ifdef _WIN32
    EnterCriticalSection(&client->lock);
#else
    pthread_mutex_lock(&client->lock);
#endif
}

static void http_client_unlock(HttpClientState *client) {
#ifdef _WIN32
    LeaveCriticalSection(&client->lock);
#else
    pthread_mutex_unlock(&client->lock);
#endif
}

static void http_client_signal(HttpClientState *client) {
#ifdef _WIN32
    WakeAllConditionVariable(&client->available);
#else
    pthread_cond_broadcast(&client->available);
#endif
}

static int http_client_wait(HttpClientState *client, int milliseconds) {
#ifdef _WIN32
    return SleepConditionVariableCS(&client->available, &client->lock,
                                    (DWORD)milliseconds) != 0;
#else
    struct timespec until;
    if (clock_gettime(CLOCK_REALTIME, &until) != 0) return 0;
    until.tv_sec += milliseconds / 1000;
    until.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&client->available, &client->lock,
                                  &until) == 0;
#endif
}

static void http_client_lock_destroy(HttpClientState *client) {
#ifdef _WIN32
    DeleteCriticalSection(&client->lock);
#else
    pthread_cond_destroy(&client->available);
    pthread_mutex_destroy(&client->lock);
#endif
}

static void http_client_slot_close(HttpClientSlot *slot) {
    if (!slot->connected) return;
    transport_close(slot->transport);
    memset(&slot->transport, 0, sizeof(slot->transport));
    slot->transport.socket.fd = -1;
    slot->connected = 0;
    slot->last_used_ms = 0;
}

static HttpClientSlot *http_client_acquire(HttpClientState *client,
                                           Iron_Deadline deadline,
                                           int64_t *error) {
    http_client_lock(client);
    for (;;) {
        if (client->closing) {
            *error = IRON_ERR_NET_CLOSED;
            http_client_unlock(client);
            return NULL;
        }
        uint64_t now = Iron_monotonic_now_ms();
        for (int64_t i = 0; i < client->slot_count; i++) {
            HttpClientSlot *slot = &client->slots[i];
            if (!slot->busy && slot->connected &&
                (client->idle_timeout == 0 ||
                 now - slot->last_used_ms >= (uint64_t)client->idle_timeout)) {
                http_client_slot_close(slot);
            }
            if (!slot->busy) {
                slot->busy = 1;
                http_client_unlock(client);
                return slot;
            }
        }
        int remaining = Iron_deadline_remaining_ms(deadline);
        if (remaining <= 0 || !http_client_wait(client, remaining)) {
            *error = IRON_ERR_NET_TIMEOUT;
            http_client_unlock(client);
            return NULL;
        }
    }
}

static void http_client_release(HttpClientState *client, HttpClientSlot *slot,
                                int reusable) {
    http_client_lock(client);
    if (!reusable || client->closing) http_client_slot_close(slot);
    else slot->last_used_ms = Iron_monotonic_now_ms();
    slot->busy = 0;
    http_client_signal(client);
    http_client_unlock(client);
}

static int64_t http_client_connect_slot(HttpClientState *client,
                                        HttpClientSlot *slot,
                                        Iron_Deadline deadline) {
    Iron_String host = http_str(client->origin.host);
    Iron_Result_TcpSocket_Error dial = Iron_net_tcp_dial(
        host, client->origin.port, Iron_deadline_remaining_ms(deadline));
    if (dial.v1.code != 0) {
        iron_string_release(&host);
        return dial.v1.code;
    }
    http_set_nodelay(dial.v0.fd);
    slot->transport.socket = dial.v0;
    slot->transport.tls = NULL;
    if (client->origin.secure) {
        Iron_String ca = http_slice(client->ca_file, client->ca_file_length);
        Iron_TlsStreamResult tls = iron_tls_client_connect(
            dial.v0, host, ca, client->insecure != 0, deadline);
        iron_string_release(&ca);
        iron_string_release(&host);
        if (tls.error.code != 0) {
            Iron_tcpsocket_close(dial.v0);
            slot->transport.socket.fd = -1;
            return tls.error.code;
        }
        slot->transport.tls = tls.stream;
    } else {
        iron_string_release(&host);
    }
    slot->connected = 1;
    return 0;
}

static int64_t http_client_send_request(
    HttpClientState *client, HttpClientSlot *slot,
    const char *method, size_t method_length,
    const char *target, size_t target_length,
    const char *headers, size_t headers_length,
    const char *body, size_t body_length, Iron_Deadline deadline) {
    size_t capacity = method_length + target_length +
        strlen(client->origin.host_header) + 256;
    char *fixed = (char *)malloc(capacity);
    if (!fixed) return IRON_ERR_NET_NO_MEMORY;
    int length = snprintf(fixed, capacity,
        "%.*s %.*s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n"
        "Content-Length: %zu\r\n",
        (int)method_length, method, (int)target_length, target,
        client->origin.host_header, body_length);
    if (length < 0 || (size_t)length >= capacity) {
        free(fixed);
        return IRON_ERR_HTTP_INVALID_ARGUMENT;
    }
    char *msg = NULL;
    size_t msg_len = 0;
    int64_t error = http_build_request(&msg, &msg_len, fixed, (size_t)length,
                                       headers, headers_length, body, body_length);
    free(fixed);
    if (!error) error = send_all(slot->transport, (const uint8_t *)msg, msg_len, deadline);
    free(msg);
    return error;
}

static Iron_HttpClientResult http_client_result_error(int64_t error) {
    Iron_HttpClientResult out;
    memset(&out, 0, sizeof(out));
    out.error = error;
    out.error_message = http_str(http_error_message(error));
    return out;
}

Iron_HttpClientResult Iron_httpclient_open(
    Iron_String origin, Iron_String ca_file, bool insecure,
    int64_t max_connections, int64_t idle_timeout) {
    if (max_connections <= 0 || max_connections > 64 || idle_timeout < 0 ||
        (insecure && iron_string_byte_len(&ca_file) != 0))
        return http_client_result_error(IRON_ERR_HTTP_INVALID_ARGUMENT);
    ParsedUrl parsed = parse_http_url(origin);
    if (parsed.error) return http_client_result_error(parsed.error);
    size_t ca_length = iron_string_byte_len(&ca_file);
    if (!parsed.origin_only ||
        (!parsed.secure && (insecure || ca_length != 0)))
        return http_client_result_error(IRON_ERR_HTTP_INVALID_ARGUMENT);
    if (parsed.secure && !iron_tls_is_available())
        return http_client_result_error(IRON_ERR_TLS_UNAVAILABLE);
    const char *ca_bytes = iron_string_cstr(&ca_file);
    if (memchr(ca_bytes, '\0', ca_length) != NULL)
        return http_client_result_error(IRON_ERR_HTTP_INVALID_ARGUMENT);
    HttpClientState *client = (HttpClientState *)calloc(1, sizeof(*client));
    if (!client) return http_client_result_error(IRON_ERR_NET_NO_MEMORY);
    client->slots = (HttpClientSlot *)calloc(
        (size_t)max_connections, sizeof(*client->slots));
    client->ca_file = (char *)malloc(ca_length + 1);
    if (!client->slots || !client->ca_file || !http_client_lock_init(client)) {
        free(client->slots);
        free(client->ca_file);
        free(client);
        return http_client_result_error(IRON_ERR_NET_NO_MEMORY);
    }
    memcpy(client->ca_file, ca_bytes, ca_length);
    client->ca_file[ca_length] = '\0';
    client->ca_file_length = ca_length;
    client->origin = parsed;
    client->insecure = insecure;
    client->idle_timeout = idle_timeout;
    client->slot_count = max_connections;
    for (int64_t i = 0; i < max_connections; i++)
        client->slots[i].transport.socket.fd = -1;
    Iron_HttpClientResult out = http_client_result_error(0);
    out.client.handle = (int64_t)(intptr_t)client;
    return out;
}

Iron_HttpResponse Iron_httpclient_request(
    Iron_HttpClient handle, Iron_String method_value, Iron_String target_value,
    Iron_String headers_value, Iron_String body_value,
    int64_t max_body_bytes, int64_t timeout) {
    HttpClientState *client = (HttpClientState *)(intptr_t)handle.handle;
    Iron_HttpResponse out = http_response_empty();
    const char *method = iron_string_cstr(&method_value);
    size_t method_length = iron_string_byte_len(&method_value);
    const char *target = iron_string_cstr(&target_value);
    size_t target_length = iron_string_byte_len(&target_value);
    const char *headers = iron_string_cstr(&headers_value);
    size_t headers_length = iron_string_byte_len(&headers_value);
    const char *body = iron_string_cstr(&body_value);
    size_t body_length = iron_string_byte_len(&body_value);
    if (!client || timeout < 0 || max_body_bytes < 0 ||
        (uint64_t)max_body_bytes > (uint64_t)(SIZE_MAX - 1) ||
        !valid_method(method, method_length) ||
        !valid_request_target(method, method_length, target, target_length) ||
        !raw_headers_valid(headers, headers_length) ||
        headers_have_forbidden_framing(headers, headers_length)) {
        http_response_error(&out, IRON_ERR_HTTP_INVALID_ARGUMENT);
        return out;
    }
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
    int64_t acquire_error = 0;
    HttpClientSlot *slot = http_client_acquire(client, deadline, &acquire_error);
    if (!slot) {
        http_response_error(&out, acquire_error);
        return out;
    }
    int retry_safe = body_length == 0 &&
        (ascii_ieq_n(method, method_length, "GET", 3) ||
         ascii_ieq_n(method, method_length, "HEAD", 4));
    for (int attempt = 0; attempt < 2; attempt++) {
        int was_reused = slot->connected;
        int64_t error = slot->connected ? 0
            : http_client_connect_slot(client, slot, deadline);
        if (!error) error = http_client_send_request(
            client, slot, method, method_length, target, target_length,
            headers, headers_length, body, body_length, deadline);
        if (error) {
            http_client_slot_close(slot);
            if (attempt == 0 && retry_safe && was_reused &&
                Iron_deadline_remaining_ms(deadline) > 0) continue;
            http_response_error(&out, error);
            http_client_release(client, slot, 0);
            return out;
        }
        out = read_client_response(
            slot->transport, (size_t)max_body_bytes, deadline,
            ascii_ieq_n(method, method_length, "HEAD", 4));
        if (out.error != 0) {
            int64_t error_code = out.error;
            http_client_slot_close(slot);
            int stale_transport =
                (error_code >= IRON_ERR_NET_UNKNOWN &&
                 error_code < IRON_ERR_HTTP_BAD_URL) ||
                error_code == IRON_ERR_TLS_CLOSED ||
                error_code == IRON_ERR_TLS_IO ||
                error_code == IRON_ERR_HTTP_TRUNCATED_MESSAGE;
            if (attempt == 0 && retry_safe && was_reused && stale_transport &&
                Iron_deadline_remaining_ms(deadline) > 0) {
                http_response_dispose(&out);
                out = http_response_empty();
                continue;
            }
            http_client_release(client, slot, 0);
            return out;
        }
        http_client_release(client, slot, out.keep_alive);
        return out;
    }
    http_response_error(&out, IRON_ERR_NET_CLOSED);
    http_client_release(client, slot, 0);
    return out;
}

uint64_t iron_http_bench_parse_requests(
    const uint8_t *data, size_t len, size_t message_count,
    size_t repetitions, int64_t *error_out) {
    const int64_t memory_fd = INT64_MIN + 9112;
    uint64_t checksum = 0;
    *error_out = 0;
    http_pending_clear(memory_fd);
    for (size_t repetition = 0; repetition < repetitions; repetition++) {
        HttpMemoryTransport memory;
        memset(&memory, 0, sizeof(memory));
        memory.input = data;
        memory.input_len = len;
        HttpTransport transport = {{memory_fd}, NULL, &memory};
        for (size_t message = 0; message < message_count; message++) {
            Iron_HttpRequest request = read_request_transport(
                transport, HTTP_DEFAULT_MAX_HEADER, HTTP_DEFAULT_MAX_BODY,
                1000);
            if (request.error != 0) {
                *error_out = request.error;
                Iron_httprequest_dispose(request);
                http_pending_clear(memory_fd);
                return checksum;
            }
            checksum += iron_string_byte_len(&request.method) * 3u +
                        iron_string_byte_len(&request.target) * 5u +
                        iron_string_byte_len(&request.headers) * 7u +
                        iron_string_byte_len(&request.body) * 11u +
                        (uint64_t)request.keep_alive;
            Iron_httprequest_dispose(request);
        }
        http_pending_clear(memory_fd);
    }
    return checksum;
}

uint64_t iron_http_bench_serialize_response(
    int64_t status, const char *headers, size_t headers_len,
    const uint8_t *body, size_t body_len, size_t repetitions,
    uint64_t *bytes_out, int64_t *error_out) {
    *bytes_out = 0;
    *error_out = 0;
    if (body_len > SIZE_MAX - headers_len - 512) {
        *error_out = IRON_ERR_HTTP_INVALID_ARGUMENT;
        return 0;
    }
    size_t capacity = body_len + headers_len + 512;
    uint8_t *output = (uint8_t *)malloc(capacity);
    if (!output) {
        *error_out = IRON_ERR_NET_NO_MEMORY;
        return 0;
    }
    Iron_String header_string = http_slice(headers, headers_len);
    Iron_String body_string = http_slice((const char *)body, body_len);
    Iron_HttpResponse response = Iron_http_response(
        status, header_string, body_string);
    iron_string_release(&header_string);
    iron_string_release(&body_string);
    if (response.error != 0) {
        *error_out = response.error;
        Iron_httpresponse_dispose(response);
        free(output);
        return 0;
    }
    uint64_t checksum = 0;
    for (size_t repetition = 0; repetition < repetitions; repetition++) {
        HttpMemoryTransport memory;
        memset(&memory, 0, sizeof(memory));
        memory.output = output;
        memory.output_cap = capacity;
        HttpTransport transport = {{INT64_MIN + 9110}, NULL, &memory};
        int64_t error = send_response_transport(
            transport, response, true, 1000);
        if (error != 0) {
            *error_out = error;
            break;
        }
        if (memory.write_calls != 1) {
            *error_out = IRON_ERR_HTTP_MALFORMED_MESSAGE;
            break;
        }
        *bytes_out += memory.output_len;
        checksum += memory.output_len ? output[memory.output_len - 1] : 0;
        checksum += memory.output_len;
    }
    Iron_httpresponse_dispose(response);
    free(output);
    return checksum;
}

void Iron_httpclient_close(Iron_HttpClient handle) {
    HttpClientState *client = (HttpClientState *)(intptr_t)handle.handle;
    if (!client) return;
    http_client_lock(client);
    client->closing = 1;
    for (;;) {
        int busy = 0;
        for (int64_t i = 0; i < client->slot_count; i++)
            if (client->slots[i].busy) busy = 1;
        if (!busy) break;
        (void)http_client_wait(client, 1000);
    }
    for (int64_t i = 0; i < client->slot_count; i++)
        http_client_slot_close(&client->slots[i]);
    http_client_unlock(client);
    http_client_lock_destroy(client);
    free(client->slots);
    free(client->ca_file);
    free(client);
}

/* ── HttpPoller: readiness-driven keep-alive serving ───────────────────────
 *
 * A server that hands each connection to a pool worker for its whole life
 * can serve only as many keep-alive clients as it has workers: the rest wait
 * in the queue while idle connections hold their workers in read_request.
 * The poller keeps idle connections out of the workers. It owns accepting
 * (non-blocking, TCP_NODELAY) and watches new and parked connections with
 * epoll (Linux), kqueue (macOS and the BSDs) or poll (elsewhere); next()
 * returns one connection whose request bytes have arrived. The worker reads
 * that request, answers it and parks the connection again, or closes it.
 *
 * Slot states: WAITING (accepted, no bytes yet), PARKED (between requests),
 * ACTIVE (handed out by next(), owned by a worker). Only WAITING and PARKED
 * connections are watched and expire after idle_timeout milliseconds; an
 * ACTIVE connection is the worker's to park, discard or close. Interest is
 * one-shot, so an fd is returned once per park.
 *
 * Thread safety: any thread may park or discard, and any number of threads
 * may call next() at once: a server can run one accept loop that hands
 * connections to tasks, or several loops that each serve what they take. The state lives until the last
 * Iron reference drops (HttpPollerCore's drop calls free), so a worker that
 * parks after close() only has its connection closed. */

#if defined(__linux__)
  #define HTTP_POLLER_EPOLL 1
  #include <sys/epoll.h>
  #include <sys/eventfd.h>
  #include <netinet/tcp.h>
  #include <unistd.h>
  #include <fcntl.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  #define HTTP_POLLER_KQUEUE 1
  #include <sys/event.h>
  #include <netinet/tcp.h>
  #include <unistd.h>
  #include <fcntl.h>
#else
  #define HTTP_POLLER_POLL 1
  #ifndef _WIN32
    #include <poll.h>
    #include <netinet/tcp.h>
    #include <unistd.h>
    #include <fcntl.h>
  #endif
#endif

enum {
    HTTP_POLL_WAITING = 1,
    HTTP_POLL_PARKED = 2,
    HTTP_POLL_ACTIVE = 3,
    HTTP_POLL_EVENTS = 256,
    HTTP_POLL_ACCEPT_BURST = 256
};

typedef struct HttpPollSlot {
    int64_t fd;
    uint64_t since_ms;
    int state;
    int registered;
} HttpPollSlot;

typedef struct HttpPollerState {
    int64_t listen_fd;
    int64_t idle_timeout;
    int closed;
#if defined(HTTP_POLLER_EPOLL)
    int ep;
    int wake_fd;
#elif defined(HTTP_POLLER_KQUEUE)
    int kq;
#endif
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    HttpPollSlot *slots;
    size_t count, cap;
#ifndef _WIN32
    int32_t *pos_by_fd;     /* fd -> index in slots, -1 when absent */
    size_t pos_cap;
#endif
    int64_t *ready;         /* ring of fds handed out by next() */
    size_t ready_head, ready_count, ready_cap;
    uint64_t last_sweep_ms;
    int64_t accepted, expired;
    int callers;            /* threads inside next() */
} HttpPollerState;

static void http_poller_lock(HttpPollerState *st) {
#ifdef _WIN32
    EnterCriticalSection(&st->lock);
#else
    pthread_mutex_lock(&st->lock);
#endif
}

static void http_poller_unlock(HttpPollerState *st) {
#ifdef _WIN32
    LeaveCriticalSection(&st->lock);
#else
    pthread_mutex_unlock(&st->lock);
#endif
}

static void http_poller_close_fd(int64_t fd) {
    Iron_TcpSocket s = { fd };
    Iron_tcpsocket_close(s);
}

static long http_poller_find(HttpPollerState *st, int64_t fd) {
#ifdef _WIN32
    for (size_t i = 0; i < st->count; i++)
        if (st->slots[i].fd == fd) return (long)i;
    return -1;
#else
    if (fd < 0 || (size_t)fd >= st->pos_cap) return -1;
    return st->pos_by_fd[fd];
#endif
}

static HttpPollSlot *http_poller_add(HttpPollerState *st, int64_t fd, int state,
                                     uint64_t now) {
    long existing = http_poller_find(st, fd);
    if (existing >= 0) {
        /* The fd number was closed by its owner and reused by the kernel:
         * whatever registration it had went with the old descriptor. */
        HttpPollSlot *slot = &st->slots[existing];
        slot->state = state;
        slot->since_ms = now;
        slot->registered = 0;
        return slot;
    }
    if (st->count == st->cap) {
        size_t cap = st->cap ? st->cap * 2 : 64;
        HttpPollSlot *grown = (HttpPollSlot *)realloc(st->slots, cap * sizeof(*grown));
        if (!grown) return NULL;
        st->slots = grown;
        st->cap = cap;
    }
#ifndef _WIN32
    if (fd < 0) return NULL;
    if ((size_t)fd >= st->pos_cap) {
        size_t cap = st->pos_cap ? st->pos_cap : 256;
        while (cap <= (size_t)fd) cap *= 2;
        int32_t *grown = (int32_t *)realloc(st->pos_by_fd, cap * sizeof(*grown));
        if (!grown) return NULL;
        for (size_t i = st->pos_cap; i < cap; i++) grown[i] = -1;
        st->pos_by_fd = grown;
        st->pos_cap = cap;
    }
    st->pos_by_fd[fd] = (int32_t)st->count;
#endif
    HttpPollSlot *slot = &st->slots[st->count++];
    slot->fd = fd;
    slot->since_ms = now;
    slot->state = state;
    slot->registered = 0;
    return slot;
}

static void http_poller_remove_at(HttpPollerState *st, size_t i) {
#ifndef _WIN32
    st->pos_by_fd[st->slots[i].fd] = -1;
#endif
    st->count--;
    if (i != st->count) {
        st->slots[i] = st->slots[st->count];
#ifndef _WIN32
        st->pos_by_fd[st->slots[i].fd] = (int32_t)i;
#endif
    }
}

static int http_poller_push_ready(HttpPollerState *st, int64_t fd) {
    if (st->ready_count == st->ready_cap) {
        size_t cap = st->ready_cap ? st->ready_cap * 2 : 64;
        int64_t *grown = (int64_t *)malloc(cap * sizeof(*grown));
        if (!grown) return 0;
        for (size_t i = 0; i < st->ready_count; i++)
            grown[i] = st->ready[(st->ready_head + i) % st->ready_cap];
        free(st->ready);
        st->ready = grown;
        st->ready_cap = cap;
        st->ready_head = 0;
    }
    st->ready[(st->ready_head + st->ready_count) % st->ready_cap] = fd;
    st->ready_count++;
    return 1;
}

/* Watch the slot's fd for one readable event. 0 on failure. */
static int http_poller_arm(HttpPollerState *st, HttpPollSlot *slot) {
#if defined(HTTP_POLLER_EPOLL)
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
    ev.data.fd = (int)slot->fd;
    if (slot->registered) {
        if (epoll_ctl(st->ep, EPOLL_CTL_MOD, (int)slot->fd, &ev) == 0) return 1;
        if (errno != ENOENT) return 0;
    }
    if (epoll_ctl(st->ep, EPOLL_CTL_ADD, (int)slot->fd, &ev) != 0) {
        if (errno != EEXIST ||
            epoll_ctl(st->ep, EPOLL_CTL_MOD, (int)slot->fd, &ev) != 0) return 0;
    }
    slot->registered = 1;
    return 1;
#elif defined(HTTP_POLLER_KQUEUE)
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)slot->fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_ONESHOT,
           0, 0, NULL);
    if (kevent(st->kq, &ev, 1, NULL, 0, NULL) != 0) return 0;
    slot->registered = 1;
    return 1;
#else
    (void)st;
    slot->registered = 1;   /* the poll set is rebuilt from the slots */
    return 1;
#endif
}

static void http_poller_forget(HttpPollerState *st, HttpPollSlot *slot) {
#if defined(HTTP_POLLER_EPOLL)
    if (slot->registered) epoll_ctl(st->ep, EPOLL_CTL_DEL, (int)slot->fd, NULL);
#else
    (void)st;
#endif
    slot->registered = 0;
}

static void http_poller_wake(HttpPollerState *st) {
#if defined(HTTP_POLLER_EPOLL)
    uint64_t one = 1;
    ssize_t w = write(st->wake_fd, &one, sizeof(one));
    (void)w;
#elif defined(HTTP_POLLER_KQUEUE)
    struct kevent ev;
    EV_SET(&ev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent(st->kq, &ev, 1, NULL, 0, NULL);
#else
    (void)st;   /* poll waits in short slices and sees `closed` */
#endif
}

static void http_poller_set_socket_options(int64_t fd) {
    http_set_nodelay(fd);
}

static int64_t http_poller_accept_one(int64_t listen_fd) {
#if defined(_WIN32)
    SOCKET fd = accept((SOCKET)listen_fd, NULL, NULL);
    if (fd == INVALID_SOCKET) return -1;
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
    return (int64_t)fd;
#else
    int fd = accept((int)listen_fd, NULL, NULL);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#endif
}

/* Accept what the listen queue holds (bounded per wakeup) and watch each. */
static void http_poller_accept_burst(HttpPollerState *st, uint64_t now) {
    for (int i = 0; i < HTTP_POLL_ACCEPT_BURST; i++) {
        int64_t fd = http_poller_accept_one(st->listen_fd);
        if (fd < 0) return;
        http_poller_set_socket_options(fd);
        HttpPollSlot *slot = http_poller_add(st, fd, HTTP_POLL_WAITING, now);
        if (!slot || !http_poller_arm(st, slot)) {
            long at = http_poller_find(st, fd);
            if (at >= 0) http_poller_remove_at(st, (size_t)at);
            http_poller_close_fd(fd);
            continue;
        }
        st->accepted++;
    }
}

/* True when a peer that signalled hang-up has nothing left to read. */
static int http_poller_peer_gone(int64_t fd) {
    char b;
#ifdef _WIN32
    int n = recv((SOCKET)fd, &b, 1, MSG_PEEK);
    return n == 0;
#else
    ssize_t n = recv((int)fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
#endif
}

/* A watched connection became readable (or hung up). */
static void http_poller_on_ready(HttpPollerState *st, int64_t fd, int hangup) {
    long at = http_poller_find(st, fd);
    if (at < 0) return;
    HttpPollSlot *slot = &st->slots[at];
    if (slot->state != HTTP_POLL_WAITING && slot->state != HTTP_POLL_PARKED) return;
    if (hangup && http_poller_peer_gone(fd)) {
        http_poller_forget(st, slot);
        http_poller_remove_at(st, (size_t)at);
        http_poller_close_fd(fd);
        return;
    }
    slot->state = HTTP_POLL_ACTIVE;
    if (!http_poller_push_ready(st, fd)) {
        http_poller_forget(st, slot);
        http_poller_remove_at(st, (size_t)at);
        http_poller_close_fd(fd);
    }
}

static void http_poller_sweep(HttpPollerState *st, uint64_t now) {
    st->last_sweep_ms = now;
    if (st->idle_timeout <= 0) return;
    size_t i = st->count;
    while (i > 0) {
        i--;
        HttpPollSlot *slot = &st->slots[i];
        if ((slot->state == HTTP_POLL_WAITING || slot->state == HTTP_POLL_PARKED) &&
            now - slot->since_ms >= (uint64_t)st->idle_timeout) {
            int64_t fd = slot->fd;
            http_poller_forget(st, slot);
            http_poller_remove_at(st, i);
            http_poller_close_fd(fd);
            st->expired++;
        }
    }
}

int64_t Iron_httppollernative_open(int64_t listen_fd, int64_t idle_timeout) {
    if (listen_fd < 0 || idle_timeout < 0) return -IRON_ERR_HTTP_INVALID_ARGUMENT;
    HttpPollerState *st = (HttpPollerState *)calloc(1, sizeof(*st));
    if (!st) return -IRON_ERR_NET_NO_MEMORY;
    st->listen_fd = listen_fd;
    st->idle_timeout = idle_timeout;
    st->last_sweep_ms = Iron_monotonic_now_ms();
#if defined(HTTP_POLLER_EPOLL)
    st->ep = epoll_create1(EPOLL_CLOEXEC);
    st->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (st->ep < 0 || st->wake_fd < 0) goto fail;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = (int)listen_fd;
    if (epoll_ctl(st->ep, EPOLL_CTL_ADD, (int)listen_fd, &ev) != 0) goto fail;
    ev.data.fd = st->wake_fd;
    if (epoll_ctl(st->ep, EPOLL_CTL_ADD, st->wake_fd, &ev) != 0) goto fail;
#elif defined(HTTP_POLLER_KQUEUE)
    st->kq = kqueue();
    if (st->kq < 0) goto fail;
    fcntl(st->kq, F_SETFD, FD_CLOEXEC);
    struct kevent evs[2];
    EV_SET(&evs[0], (uintptr_t)listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    EV_SET(&evs[1], 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(st->kq, evs, 2, NULL, 0, NULL) != 0) goto fail;
#endif
#ifdef _WIN32
    InitializeCriticalSection(&st->lock);
#else
    if (pthread_mutex_init(&st->lock, NULL) != 0) goto fail;
#endif
    return (int64_t)(intptr_t)st;
#if !defined(HTTP_POLLER_POLL) || !defined(_WIN32)
fail:
#if defined(HTTP_POLLER_EPOLL)
    if (st->ep >= 0) close(st->ep);
    if (st->wake_fd >= 0) close(st->wake_fd);
#elif defined(HTTP_POLLER_KQUEUE)
    if (st->kq >= 0) close(st->kq);
#endif
    free(st);
    return -IRON_ERR_NET_UNKNOWN;
#endif
}

/* Wait for readiness for at most `wait_ms` with the lock released, then
 * process the events with the lock held again. */
static void http_poller_wait(HttpPollerState *st, int wait_ms) {
    uint64_t now;
#if defined(HTTP_POLLER_EPOLL)
    struct epoll_event evs[HTTP_POLL_EVENTS];
    /* With several threads in next(), each takes one event: the kernel hands
     * every ready connection to exactly one waiter, and none sits in the
     * ready queue while its taker is busy serving another. */
    int max_events = st->callers > 1 ? 1 : HTTP_POLL_EVENTS;
    http_poller_unlock(st);
    int n = epoll_wait(st->ep, evs, max_events, wait_ms);
    http_poller_lock(st);
    if (n <= 0 || st->closed) return;
    now = Iron_monotonic_now_ms();
    for (int i = 0; i < n; i++) {
        int fd = evs[i].data.fd;
        if (fd == (int)st->listen_fd) {
            http_poller_accept_burst(st, now);
        } else if (fd == st->wake_fd) {
            uint64_t v;
            ssize_t r = read(st->wake_fd, &v, sizeof(v));
            (void)r;
        } else {
            int hangup = (evs[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0;
            http_poller_on_ready(st, fd, hangup);
        }
    }
#elif defined(HTTP_POLLER_KQUEUE)
    struct kevent evs[HTTP_POLL_EVENTS];
    struct timespec ts;
    ts.tv_sec = wait_ms / 1000;
    ts.tv_nsec = (long)(wait_ms % 1000) * 1000000L;
    int max_events = st->callers > 1 ? 1 : HTTP_POLL_EVENTS;
    http_poller_unlock(st);
    int n = kevent(st->kq, NULL, 0, evs, max_events, wait_ms < 0 ? NULL : &ts);
    http_poller_lock(st);
    if (n <= 0 || st->closed) return;
    now = Iron_monotonic_now_ms();
    for (int i = 0; i < n; i++) {
        if (evs[i].filter == EVFILT_USER) continue;
        int64_t fd = (int64_t)evs[i].ident;
        if (fd == st->listen_fd) {
            http_poller_accept_burst(st, now);
        } else {
            int hangup = (evs[i].flags & (EV_EOF | EV_ERROR)) != 0;
            http_poller_on_ready(st, fd, hangup);
        }
    }
#else
    /* poll(): the set is rebuilt from the watched slots each round and the
     * wait is sliced so close() is noticed without a wake-up primitive. */
    size_t watched = 0;
    for (size_t i = 0; i < st->count; i++)
        if (st->slots[i].state == HTTP_POLL_WAITING || st->slots[i].state == HTTP_POLL_PARKED)
            watched++;
#ifdef _WIN32
    WSAPOLLFD *pfds = (WSAPOLLFD *)malloc((watched + 1) * sizeof(*pfds));
#else
    struct pollfd *pfds = (struct pollfd *)malloc((watched + 1) * sizeof(*pfds));
#endif
    if (!pfds) return;
    size_t np = 0;
    pfds[np].fd = st->listen_fd;
    pfds[np].events = POLLIN;
    pfds[np].revents = 0;
    np++;
    for (size_t i = 0; i < st->count; i++) {
        if (st->slots[i].state != HTTP_POLL_WAITING && st->slots[i].state != HTTP_POLL_PARKED) continue;
        pfds[np].fd = st->slots[i].fd;
        pfds[np].events = POLLIN;
        pfds[np].revents = 0;
        np++;
    }
    int slice = wait_ms < 0 || wait_ms > 50 ? 50 : wait_ms;
    http_poller_unlock(st);
#ifdef _WIN32
    int n = WSAPoll(pfds, (ULONG)np, slice);
#else
    int n = poll(pfds, (nfds_t)np, slice);
#endif
    http_poller_lock(st);
    if (n > 0 && !st->closed) {
        now = Iron_monotonic_now_ms();
        for (size_t i = 1; i < np; i++) {
            if (!pfds[i].revents) continue;
            int hangup = (pfds[i].revents & (POLLHUP | POLLERR)) != 0;
            http_poller_on_ready(st, (int64_t)pfds[i].fd, hangup);
        }
        if (pfds[0].revents) http_poller_accept_burst(st, now);
    }
    free(pfds);
#endif
    (void)now;
}

int64_t Iron_httppollernative_next(int64_t h, int64_t timeout) {
    HttpPollerState *st = (HttpPollerState *)(intptr_t)h;
    if (!st || timeout < 0) return -IRON_ERR_HTTP_INVALID_ARGUMENT;
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
    http_poller_lock(st);
    st->callers++;
    for (;;) {
        if (st->closed) {
            st->callers--;
            http_poller_unlock(st);
            return -IRON_ERR_NET_CLOSED;
        }
        if (st->ready_count) {
            int64_t fd = st->ready[st->ready_head];
            st->ready_head = (st->ready_head + 1) % st->ready_cap;
            st->ready_count--;
            st->callers--;
            http_poller_unlock(st);
            return fd;
        }
        uint64_t now = Iron_monotonic_now_ms();
        uint64_t sweep_every = st->idle_timeout > 0 && st->idle_timeout < 2000
            ? (uint64_t)st->idle_timeout / 2 + 1 : 1000;
        if (now - st->last_sweep_ms >= sweep_every) http_poller_sweep(st, now);
        int remaining = Iron_deadline_remaining_ms(deadline);
        if (remaining <= 0 && timeout > 0) {
            st->callers--;
            http_poller_unlock(st);
            return -IRON_ERR_NET_TIMEOUT;
        }
        int wait = remaining;
        if ((uint64_t)wait > sweep_every) wait = (int)sweep_every;
        http_poller_wait(st, wait);
        if (timeout == 0 && !st->ready_count && !st->closed) {
            st->callers--;
            http_poller_unlock(st);
            return -IRON_ERR_NET_TIMEOUT;
        }
    }
}

void Iron_httppollernative_park(int64_t h, int64_t fd) {
    HttpPollerState *st = (HttpPollerState *)(intptr_t)h;
    if (fd < 0) return;
    if (!st) {
        http_poller_close_fd(fd);
        return;
    }
    http_poller_lock(st);
    if (st->closed) {
        long at = http_poller_find(st, fd);
        if (at >= 0) http_poller_remove_at(st, (size_t)at);
        http_poller_unlock(st);
        http_poller_close_fd(fd);
        return;
    }
    uint64_t now = Iron_monotonic_now_ms();
    long at = http_poller_find(st, fd);
    HttpPollSlot *slot;
    if (at >= 0) {
        slot = &st->slots[at];
        slot->state = HTTP_POLL_PARKED;
        slot->since_ms = now;
    } else {
        http_poller_set_socket_options(fd);
        slot = http_poller_add(st, fd, HTTP_POLL_PARKED, now);
    }
    if (!slot || !http_poller_arm(st, slot)) {
        at = http_poller_find(st, fd);
        if (at >= 0) http_poller_remove_at(st, (size_t)at);
        http_poller_unlock(st);
        http_poller_close_fd(fd);
        return;
    }
    http_poller_unlock(st);
}

/* Close a connection whose peer may still be sending (a request rejected
 * before it was read: too large, malformed). Closing with unread input
 * makes the kernel reset the connection, and the reset can destroy the
 * answer before the client reads it; so stop sending, read and drop what
 * arrives for a short while, then close. */
static void http_poller_close_draining(int64_t fd) {
    /* Only a connection with input still waiting needs the lingering close
     * (it waits for the peer); the usual discard closes at once. */
    char b;
#ifdef _WIN32
    u_long avail = 0;
    bool pending = ioctlsocket((SOCKET)fd, FIONREAD, &avail) == 0 && avail > 0;
    (void)b;
#else
    bool pending = recv((int)fd, &b, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
#endif
    if (pending) iron_net_lingering_close(fd);
    else http_poller_close_fd(fd);
}

void Iron_httppollernative_discard(int64_t h, int64_t fd) {
    HttpPollerState *st = (HttpPollerState *)(intptr_t)h;
    if (fd < 0) return;
    if (st) {
        http_poller_lock(st);
        long at = http_poller_find(st, fd);
        if (at >= 0) {
            http_poller_forget(st, &st->slots[at]);
            http_poller_remove_at(st, (size_t)at);
        }
        http_poller_unlock(st);
    }
    http_poller_close_draining(fd);
}

void Iron_httppollernative_close(int64_t h) {
    HttpPollerState *st = (HttpPollerState *)(intptr_t)h;
    if (!st) return;
    http_poller_lock(st);
    if (!st->closed) {
        __atomic_store_n(&st->closed, 1, __ATOMIC_RELEASE);
        size_t i = st->count;
        while (i > 0) {
            i--;
            HttpPollSlot *slot = &st->slots[i];
            if (slot->state == HTTP_POLL_ACTIVE) continue;
            int64_t fd = slot->fd;
            http_poller_forget(st, slot);
            http_poller_remove_at(st, i);
            http_poller_close_fd(fd);
        }
        /* Handed out by the poll but never taken by next(). */
        while (st->ready_count) {
            int64_t fd = st->ready[st->ready_head];
            st->ready_head = (st->ready_head + 1) % st->ready_cap;
            st->ready_count--;
            long at = http_poller_find(st, fd);
            if (at >= 0) http_poller_remove_at(st, (size_t)at);
            http_poller_close_fd(fd);
        }
        http_poller_wake(st);
    }
    http_poller_unlock(st);
}

void Iron_httppollernative_free(int64_t h) {
    HttpPollerState *st = (HttpPollerState *)(intptr_t)h;
    if (!st) return;
    Iron_httppollernative_close(h);
    /* Connections still ACTIVE belong to their workers; only forget them. */
#if defined(HTTP_POLLER_EPOLL)
    close(st->ep);
    close(st->wake_fd);
#elif defined(HTTP_POLLER_KQUEUE)
    close(st->kq);
#endif
#ifdef _WIN32
    DeleteCriticalSection(&st->lock);
#else
    pthread_mutex_destroy(&st->lock);
    free(st->pos_by_fd);
#endif
    free(st->slots);
    free(st->ready);
    free(st);
}

/* 0 parked, 1 waiting for a first request, 2 handed out (active),
 * 3 accepted in total, 4 closed for idleness in total. */
int64_t Iron_httppollernative_count(int64_t h, int64_t what) {
    HttpPollerState *st = (HttpPollerState *)(intptr_t)h;
    if (!st) return 0;
    int64_t n = 0;
    http_poller_lock(st);
    if (what == 3) n = st->accepted;
    else if (what == 4) n = st->expired;
    else {
        int want = what == 0 ? HTTP_POLL_PARKED : what == 1 ? HTTP_POLL_WAITING : HTTP_POLL_ACTIVE;
        for (size_t i = 0; i < st->count; i++)
            if (st->slots[i].state == want) n++;
    }
    http_poller_unlock(st);
    return n;
}

/* Whether close() has run: a lock-free read, cheap enough to ask on every
 * request (to answer with Connection: close while draining). */
bool Iron_httppollernative_closed(int64_t h) {
    HttpPollerState *st = (HttpPollerState *)(intptr_t)h;
    if (!st) return true;
    return __atomic_load_n(&st->closed, __ATOMIC_ACQUIRE) != 0;
}

Iron_String Iron_httppollernative_error_message(int64_t code) {
    return http_str(http_error_message(code));
}
