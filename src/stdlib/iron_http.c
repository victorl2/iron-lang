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
  #include <pthread.h>
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
    iron_string_release(&result.error_message);
}

void Iron_httpconnectionresult_release(Iron_HttpConnectionResult result) {
    iron_string_release(&result.error_message);
}

void Iron_httpsserverresult_release(Iron_HttpsServerResult result) {
    iron_string_release(&result.error_message);
}

void Iron_httpsconnectionresult_release(Iron_HttpsConnectionResult result) {
    iron_string_release(&result.error_message);
}

void Iron_httpspendingconnectionresult_release(
    Iron_HttpsPendingConnectionResult result) {
    iron_string_release(&result.error_message);
}

void Iron_httpclientresult_release(Iron_HttpClientResult result) {
    iron_string_release(&result.error_message);
}

void Iron_httprequest_release(Iron_HttpRequest request) {
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

static int header_is_exact_token(const char *headers, size_t len, const char *name,
                                 const char *token) {
    const char *v = NULL;
    size_t vn = 0;
    if (!header_value_span(headers, len, name, &v, &vn)) return 0;
    return ascii_ieq_n(v, vn, token, strlen(token));
}

static int header_has_token(const char *headers, size_t len, const char *name,
                            const char *wanted) {
    const char *value = NULL;
    size_t value_len = 0;
    if (!header_value_span(headers, len, name, &value, &value_len)) return 0;
    size_t wanted_len = strlen(wanted);
    size_t pos = 0;
    while (pos < value_len) {
        while (pos < value_len &&
               (value[pos] == ' ' || value[pos] == '\t' || value[pos] == ','))
            pos++;
        size_t end = pos;
        while (end < value_len && value[end] != ',') end++;
        size_t finish = end;
        while (finish > pos &&
               (value[finish - 1] == ' ' || value[finish - 1] == '\t'))
            finish--;
        if (ascii_ieq_n(value + pos, finish - pos, wanted, wanted_len)) return 1;
        pos = end + (end < value_len);
    }
    return 0;
}

static int parse_content_length(const char *headers, size_t len,
                                size_t *out, int *present) {
    const char *v = NULL;
    size_t vn = 0;
    *out = 0;
    *present = 0;
    if (!header_value_span(headers, len, "Content-Length", &v, &vn)) return 1;
    *present = 1;
    if (vn == 0) return 0;
    size_t n = 0;
    for (size_t i = 0; i < vn; i++) {
        if (v[i] < '0' || v[i] > '9') return 0;
        unsigned digit = (unsigned)(v[i] - '0');
        if (n > (SIZE_MAX - digit) / 10) return 0;
        n = n * 10 + digit;
    }
    *out = n;
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
            if (!(isalnum(c) || c == '!' || c == '#' || c == '$' || c == '%' ||
                  c == '&' || c == '\'' || c == '*' || c == '+' || c == '-' ||
                  c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~')) return 0;
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

typedef struct HttpHead {
    uint8_t *data;
    size_t len;
    size_t marker;
    int64_t error;
} HttpHead;

typedef struct HttpTransport {
    Iron_TcpSocket socket;
    Iron_TlsStream *tls;
} HttpTransport;

static Iron_Result_Int_Error transport_read(HttpTransport transport,
                                             uint8_t *buffer, int64_t capacity,
                                             Iron_Deadline deadline) {
    if (transport.tls) {
        return iron_tls_read(transport.tls, buffer, capacity, deadline);
    }
    return Iron_net_tcp_recv_bytes(transport.socket, buffer, capacity,
                                    Iron_deadline_remaining_ms(deadline));
}

static Iron_Result_Int_Error transport_write(HttpTransport transport,
                                              const uint8_t *buffer,
                                              int64_t length,
                                              Iron_Deadline deadline) {
    if (transport.tls) {
        return iron_tls_write(transport.tls, buffer, length, deadline);
    }
    return Iron_net_tcp_send_bytes(transport.socket, buffer, length,
                                    Iron_deadline_remaining_ms(deadline));
}

static void transport_close(HttpTransport transport) {
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
    h.data = (uint8_t *)malloc(max_header + 1);
    if (!h.data) {
        h.error = IRON_ERR_NET_NO_MEMORY;
        return h;
    }
    for (;;) {
        h.marker = find_header_end(h.data, h.len);
        if (h.marker != SIZE_MAX) return h;
        if (h.len == max_header) {
            h.error = IRON_ERR_HTTP_HEADERS_TOO_LARGE;
            return h;
        }
        size_t avail = max_header - h.len;
        size_t chunk = avail < HTTP_IO_CHUNK ? avail : HTTP_IO_CHUNK;
        int remaining = Iron_deadline_remaining_ms(deadline);
        (void)remaining;
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
    if (len == 0 || len > 32) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)method[i];
        if (!(isalnum(c) || c == '!' || c == '#' || c == '$' || c == '%' ||
              c == '&' || c == '\'' || c == '*' || c == '+' || c == '-' ||
              c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~')) return 0;
    }
    return 1;
}

static int valid_request_target(const char *target, size_t len) {
    if (len == 0 || target[0] != '/') return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c <= 32 || c == 127) return 0;
    }
    return 1;
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
    Iron_TcpSocket s = { connection.fd };
    Iron_tcpsocket_close(s);
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
    size_t line_end = find_crlf(data, 0, h.marker);
    if (line_end == SIZE_MAX) {
        http_request_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    size_t sp1 = 0;
    while (sp1 < line_end && data[sp1] != ' ') sp1++;
    size_t sp2 = sp1 + 1;
    while (sp2 < line_end && data[sp2] != ' ') sp2++;
    int version_11 = sp2 < line_end &&
        ascii_ieq_n(data + sp2 + 1, line_end - sp2 - 1, "HTTP/1.1", 8);
    int version_10 = sp2 < line_end &&
        ascii_ieq_n(data + sp2 + 1, line_end - sp2 - 1, "HTTP/1.0", 8);
    if (sp1 == 0 || sp1 >= line_end || sp2 <= sp1 + 1 || sp2 >= line_end ||
        !valid_method(data, sp1) || (!version_11 && !version_10)) {
        http_request_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    size_t target_len = sp2 - sp1 - 1;
    const char *target = data + sp1 + 1;
    if (!valid_request_target(target, target_len)) {
        http_request_error(&out, IRON_ERR_HTTP_MALFORMED_MESSAGE);
        free(h.data);
        return out;
    }
    out.method = http_slice(data, sp1);
    out.target = http_slice(target, target_len);
    out.version = http_slice(data + sp2 + 1, line_end - sp2 - 1);
    size_t query_at = 0;
    while (query_at < target_len && target[query_at] != '?') query_at++;
    out.path = http_slice(target, query_at);
    out.query = query_at < target_len
        ? http_slice(target + query_at + 1, target_len - query_at - 1)
        : http_str("");

    size_t headers_start = line_end + 2;
    size_t headers_len = h.marker >= headers_start ? h.marker - headers_start : 0;
    const char *host_value = NULL;
    size_t host_value_len = 0;
    size_t host_count = header_count(data + headers_start, headers_len, "Host");
    if (!raw_headers_valid(data + headers_start, headers_len) ||
        (version_11 ? host_count != 1 : host_count > 1) ||
        header_count(data + headers_start, headers_len, "Connection") > 1 ||
        header_count(data + headers_start, headers_len, "Content-Length") > 1 ||
        header_count(data + headers_start, headers_len, "Transfer-Encoding") > 1 ||
        (host_count > 0 &&
         (!header_value_span(data + headers_start, headers_len, "Host",
                             &host_value, &host_value_len) ||
          host_value_len == 0))) {
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
    const char *te = NULL;
    size_t te_len = 0;
    int has_te = header_value_span(data + headers_start, headers_len,
                                   "Transfer-Encoding", &te, &te_len);
    int chunked = header_is_exact_token(data + headers_start, headers_len,
                                        "Transfer-Encoding", "chunked");
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
    free(h.data);
    return out;
}

Iron_HttpRequest Iron_httpconnection_read_request(Iron_HttpConnection connection,
                                                   int64_t max_header_bytes,
                                                   int64_t max_body_bytes,
                                                   int64_t timeout) {
    HttpTransport transport = { { connection.fd }, NULL };
    return read_request_transport(transport, max_header_bytes, max_body_bytes,
                                  timeout);
}

Iron_HttpRequest Iron_httpsconnection_read_request(Iron_HttpsConnection connection,
                                                    int64_t max_header_bytes,
                                                    int64_t max_body_bytes,
                                                    int64_t timeout) {
    HttpTransport transport = {
        { connection.fd }, (Iron_TlsStream *)(intptr_t)connection.tls
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

Iron_HttpResponse Iron_http_response(int64_t status, Iron_String headers,
                                      Iron_String body) {
    return http_response_owned(
        status,
        http_slice(iron_string_cstr(&headers), iron_string_byte_len(&headers)),
        http_slice(iron_string_cstr(&body), iron_string_byte_len(&body)));
}

Iron_HttpResponse Iron_http_json_response(int64_t status, Iron_String body) {
    return http_response_owned(
        status, http_str("Content-Type: application/json; charset=utf-8"),
        http_slice(iron_string_cstr(&body), iron_string_byte_len(&body)));
}

Iron_HttpResponse Iron_http_html_response(int64_t status, Iron_String body) {
    return http_response_owned(
        status, http_str("Content-Type: text/html; charset=utf-8"),
        http_slice(iron_string_cstr(&body), iron_string_byte_len(&body)));
}

Iron_HttpResponse Iron_http_text_response(int64_t status, Iron_String body) {
    return http_response_owned(
        status, http_str("Content-Type: text/plain; charset=utf-8"),
        http_slice(iron_string_cstr(&body), iron_string_byte_len(&body)));
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
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
    char fixed[256];
    int fixed_len = snprintf(fixed, sizeof(fixed),
        "HTTP/1.1 %lld %.*s\r\nContent-Length: %zu\r\nConnection: %s\r\n",
        (long long)response.status, (int)reason_len, reason, body_len,
        keep_alive ? "keep-alive" : "close");
    if (fixed_len < 0 || (size_t)fixed_len >= sizeof(fixed)) return IRON_ERR_HTTP_INVALID_ARGUMENT;
    int64_t err = send_all(transport, (const uint8_t *)fixed, (size_t)fixed_len, deadline);
    if (err) return err;
    if (headers_len) {
        err = send_all(transport, (const uint8_t *)headers, headers_len, deadline);
        if (err) return err;
        if (headers_len < 2 || headers[headers_len - 2] != '\r' || headers[headers_len - 1] != '\n') {
            err = send_all(transport, (const uint8_t *)"\r\n", 2, deadline);
            if (err) return err;
        }
    }
    err = send_all(transport, (const uint8_t *)"\r\n", 2, deadline);
    if (err) return err;
    if (body_len) err = send_all(transport, (const uint8_t *)body, body_len, deadline);
    return err;
}

int64_t Iron_httpconnection_send_response(Iron_HttpConnection connection,
                                           Iron_HttpResponse response,
                                           int64_t timeout) {
    HttpTransport transport = { { connection.fd }, NULL };
    return send_response_transport(transport, response, false, timeout);
}

int64_t Iron_httpconnection_send_response_keep_alive(
    Iron_HttpConnection connection, Iron_HttpResponse response,
    bool keep_alive, int64_t timeout) {
    HttpTransport transport = { { connection.fd }, NULL };
    return send_response_transport(transport, response, keep_alive, timeout);
}

int64_t Iron_httpsconnection_send_response(Iron_HttpsConnection connection,
                                            Iron_HttpResponse response,
                                            int64_t timeout) {
    HttpTransport transport = {
        { connection.fd }, (Iron_TlsStream *)(intptr_t)connection.tls
    };
    return send_response_transport(transport, response, false, timeout);
}

int64_t Iron_httpsconnection_send_response_keep_alive(
    Iron_HttpsConnection connection, Iron_HttpResponse response,
    bool keep_alive, int64_t timeout) {
    HttpTransport transport = {
        { connection.fd }, (Iron_TlsStream *)(intptr_t)connection.tls
    };
    return send_response_transport(transport, response, keep_alive, timeout);
}

static Iron_HttpResponse read_client_response(HttpTransport transport,
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
    size_t line_end = find_crlf(data, 0, h.marker);
    int response_version_11 = line_end != SIZE_MAX && line_end >= 8 &&
        ascii_ieq_n(data, 8, "HTTP/1.1", 8);
    int response_version_10 = line_end != SIZE_MAX && line_end >= 8 &&
        ascii_ieq_n(data, 8, "HTTP/1.0", 8);
    int supported_version = response_version_11 || response_version_10;
    if (line_end == SIZE_MAX || line_end < 12 || !supported_version || data[8] != ' ') {
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
    out.reason = status_end < line_end
        ? http_slice(data + status_end + 1, line_end - status_end - 1)
        : http_str("");
    size_t headers_start = line_end + 2;
    size_t headers_len = h.marker >= headers_start ? h.marker - headers_start : 0;
    if (!raw_headers_valid(data + headers_start, headers_len) ||
        header_count(data + headers_start, headers_len, "Connection") > 1 ||
        header_count(data + headers_start, headers_len, "Content-Length") > 1 ||
        header_count(data + headers_start, headers_len, "Transfer-Encoding") > 1) {
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
    int chunked = header_is_exact_token(data + headers_start, headers_len,
                                        "Transfer-Encoding", "chunked");
    const char *te = NULL;
    size_t te_len = 0;
    int has_te = header_value_span(data + headers_start, headers_len,
                                   "Transfer-Encoding", &te, &te_len);
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
    if (suppress_body) ok = 1;
    else if (chunked) ok = read_chunked_body(&reader, max_body,
                                        HTTP_DEFAULT_MAX_HEADER,
                                        &out.body, &read_error);
    else if (content_present) ok = read_fixed_body(&reader, content_len, &out.body, &read_error);
    else if ((out.status >= 100 && out.status < 200) || out.status == 204 || out.status == 304) ok = 1;
    else ok = read_close_body(&reader, max_body, &out.body, &read_error);
    if (!ok) http_response_error(&out, read_error);
    else out.keep_alive = persistent && framed_for_reuse;
    free(h.data);
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
        http_response_error(&out, dr.v1.code);
        return out;
    }
    HttpTransport transport = { dr.v0, NULL };
    if (parsed.secure) {
        Iron_TlsStreamResult tls = iron_tls_client_connect(
            dr.v0, host, ca_file, insecure, deadline);
        if (tls.error.code != 0) {
            Iron_tcpsocket_close(dr.v0);
            http_response_error(&out, tls.error.code);
            return out;
        }
        transport.tls = tls.stream;
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
    int64_t err = send_all(transport, (const uint8_t *)prefix, (size_t)n, deadline);
    free(prefix);
    if (!err && headers_len) {
        err = send_all(transport, (const uint8_t *)headers_c, headers_len, deadline);
        if (!err && (headers_len < 2 || headers_c[headers_len - 2] != '\r' ||
                     headers_c[headers_len - 1] != '\n')) {
            err = send_all(transport, (const uint8_t *)"\r\n", 2, deadline);
        }
    }
    if (!err) err = send_all(transport, (const uint8_t *)"\r\n", 2, deadline);
    if (!err && body_len) {
        err = send_all(transport, (const uint8_t *)iron_string_cstr(&body), body_len, deadline);
    }
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
    if (dial.v1.code != 0) return dial.v1.code;
    slot->transport.socket = dial.v0;
    slot->transport.tls = NULL;
    if (client->origin.secure) {
        Iron_String ca = http_slice(client->ca_file, client->ca_file_length);
        Iron_TlsStreamResult tls = iron_tls_client_connect(
            dial.v0, host, ca, client->insecure != 0, deadline);
        if (tls.error.code != 0) {
            Iron_tcpsocket_close(dial.v0);
            slot->transport.socket.fd = -1;
            return tls.error.code;
        }
        slot->transport.tls = tls.stream;
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
    int64_t error = send_all(slot->transport, (const uint8_t *)fixed,
                             (size_t)length, deadline);
    free(fixed);
    if (!error && headers_length) {
        error = send_all(slot->transport, (const uint8_t *)headers,
                         headers_length, deadline);
        if (!error && (headers_length < 2 ||
            headers[headers_length - 2] != '\r' ||
            headers[headers_length - 1] != '\n')) {
            error = send_all(slot->transport, (const uint8_t *)"\r\n", 2,
                             deadline);
        }
    }
    if (!error)
        error = send_all(slot->transport, (const uint8_t *)"\r\n", 2,
                         deadline);
    if (!error && body_length)
        error = send_all(slot->transport, (const uint8_t *)body, body_length,
                         deadline);
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
        !valid_request_target(target, target_length) ||
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
                Iron_deadline_remaining_ms(deadline) > 0) continue;
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
