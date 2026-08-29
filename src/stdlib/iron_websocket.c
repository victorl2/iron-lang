#include "iron_websocket.h"

#include "iron_net.h"
#include "iron_tls.h"
#include "runtime/iron_errors.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

enum {
    WS_MAX_HEADER = 16 * 1024,
    WS_MAX_URL = 8192,
    WS_MAX_HOST = 1024
};

typedef struct WsTransport {
    Iron_TcpSocket socket;
    Iron_TlsStream *tls;
} WsTransport;

typedef struct WsSession {
    WsTransport transport;
    size_t max_message;
    int is_client;
    int close_sent;
    int close_received;
    int open;
    uint8_t *fragment;
    size_t fragment_len;
    size_t fragment_cap;
    uint8_t fragment_opcode;
    uint8_t *prefetch;
    size_t prefetch_pos;
    size_t prefetch_len;
#ifdef _WIN32
    CRITICAL_SECTION send_lock;
#else
    pthread_mutex_t send_lock;
#endif
} WsSession;

typedef struct WsUrl {
    char host[WS_MAX_HOST + 1];
    char host_header[WS_MAX_HOST + 16];
    char target[WS_MAX_URL + 1];
    int64_t port;
    int secure;
    int64_t error;
} WsUrl;

static Iron_String ws_string(const char *text, size_t length) {
    return iron_string_from_cstr(text ? text : "", text ? length : 0);
}

static Iron_String ws_cstr(const char *text) {
    return ws_string(text, text ? strlen(text) : 0);
}

static const char *ws_error_message(int64_t error) {
    switch (error) {
        case 0: return "";
        case IRON_ERR_WS_BAD_URL: return "invalid WebSocket URL";
        case IRON_ERR_WS_HANDSHAKE: return "WebSocket opening handshake failed";
        case IRON_ERR_WS_PROTOCOL: return "WebSocket protocol error";
        case IRON_ERR_WS_MESSAGE_TOO_LARGE: return "WebSocket message exceeds limit";
        case IRON_ERR_WS_INVALID_UTF8: return "WebSocket text is not valid UTF-8";
        case IRON_ERR_WS_CLOSED: return "WebSocket is closed";
        case IRON_ERR_WS_INVALID_ARGUMENT: return "invalid WebSocket argument";
        case IRON_ERR_WS_NO_MEMORY: return "WebSocket allocation failed";
        case IRON_ERR_TLS_UNAVAILABLE: return "TLS backend is unavailable";
        case IRON_ERR_TLS_TRUST_STORE: return "could not load TLS trust roots";
        case IRON_ERR_TLS_HANDSHAKE: return "TLS handshake failed";
        case IRON_ERR_TLS_VERIFY: return "TLS certificate or hostname verification failed";
        case IRON_ERR_NET_TIMEOUT: return "network timeout";
        case IRON_ERR_NET_CONN_REFUSED: return "connection refused";
        case IRON_ERR_NET_CONN_RESET: return "connection reset";
        default: return "WebSocket transport error";
    }
}

static Iron_WebSocketResult ws_result_error(int64_t error) {
    Iron_WebSocketResult result;
    memset(&result, 0, sizeof(result));
    result.error = error;
    result.error_message = ws_cstr(ws_error_message(error));
    result.protocol = ws_cstr("");
    return result;
}

static Iron_WebSocketMessage ws_message_empty(void) {
    Iron_WebSocketMessage message;
    memset(&message, 0, sizeof(message));
    message.data = ws_cstr("");
    message.error_message = ws_cstr("");
    return message;
}

static Iron_WebSocketMessage ws_message_error(int64_t error) {
    Iron_WebSocketMessage message = ws_message_empty();
    message.error = error;
    message.error_message = ws_cstr(ws_error_message(error));
    return message;
}

void Iron_websocketresult_release(Iron_WebSocketResult result) {
    iron_string_release(&result.error_message);
    iron_string_release(&result.protocol);
}

void Iron_websocketmessage_release(Iron_WebSocketMessage message) {
    iron_string_release(&message.data);
    iron_string_release(&message.error_message);
}

static int ascii_equal(const char *left, size_t left_len,
                       const char *right, size_t right_len) {
    if (left_len != right_len) return 0;
    for (size_t i = 0; i < left_len; i++) {
        if (tolower((unsigned char)left[i]) !=
            tolower((unsigned char)right[i])) return 0;
    }
    return 1;
}

static int header_span(const char *headers, size_t length, const char *name,
                       const char **value, size_t *value_length) {
    size_t name_length = strlen(name);
    size_t pos = 0;
    while (pos < length) {
        size_t end = pos;
        while (end + 1 < length &&
               !(headers[end] == '\r' && headers[end + 1] == '\n')) end++;
        size_t line_end = end + 1 < length ? end : length;
        size_t colon = pos;
        while (colon < line_end && headers[colon] != ':') colon++;
        if (colon < line_end && ascii_equal(headers + pos, colon - pos,
                                             name, name_length)) {
            size_t start = colon + 1;
            while (start < line_end &&
                   (headers[start] == ' ' || headers[start] == '\t')) start++;
            size_t finish = line_end;
            while (finish > start &&
                   (headers[finish - 1] == ' ' || headers[finish - 1] == '\t')) finish--;
            *value = headers + start;
            *value_length = finish - start;
            return 1;
        }
        pos = line_end < length ? line_end + 2 : length;
    }
    return 0;
}

static size_t header_count(const char *headers, size_t length, const char *name) {
    size_t name_length = strlen(name), count = 0, pos = 0;
    while (pos < length) {
        size_t end = pos;
        while (end + 1 < length &&
               !(headers[end] == '\r' && headers[end + 1] == '\n')) end++;
        size_t line_end = end + 1 < length ? end : length;
        size_t colon = pos;
        while (colon < line_end && headers[colon] != ':') colon++;
        if (colon < line_end && ascii_equal(headers + pos, colon - pos,
                                             name, name_length)) count++;
        pos = line_end < length ? line_end + 2 : length;
    }
    return count;
}

static int protocol_char(unsigned char byte) {
    return isalnum(byte) || byte == '!' || byte == '#' || byte == '$' ||
           byte == '%' || byte == '&' || byte == '\'' || byte == '*' ||
           byte == '+' || byte == '-' || byte == '.' || byte == '^' ||
           byte == '_' || byte == '`' || byte == '|' || byte == '~';
}

static int protocol_token_valid(const char *bytes, size_t length) {
    if (!bytes || length == 0 || length > WS_MAX_HEADER) return 0;
    for (size_t i = 0; i < length; i++) {
        if (!protocol_char((unsigned char)bytes[i])) return 0;
    }
    return 1;
}

/* Build the single Sec-WebSocket-Protocol request field while preserving the
 * caller's list order. */
static int protocol_list_build(Iron_List_Iron_String protocols,
                               char *header, size_t capacity,
                               size_t *header_length) {
    *header_length = 0;
    if (protocols.count < 0 || protocols.capacity < protocols.count ||
        (protocols.count > 0 && !protocols.items)) return 0;
    size_t total = 0;
    for (int64_t i = 0; i < protocols.count; i++) {
        const char *token = iron_string_cstr(&protocols.items[i]);
        size_t length = iron_string_byte_len(&protocols.items[i]);
        size_t separator = i ? 2u : 0u;
        if (!protocol_token_valid(token, length) ||
            total > WS_MAX_HEADER - separator ||
            length > WS_MAX_HEADER - total - separator) return 0;
        for (int64_t prior = 0; prior < i; prior++) {
            size_t prior_length = iron_string_byte_len(&protocols.items[prior]);
            if (prior_length == length &&
                memcmp(iron_string_cstr(&protocols.items[prior]), token,
                       length) == 0) return 0;
        }
        total += length + (i ? 2u : 0u);
    }
    if (total + 1 > capacity) return 0;
    size_t used = 0;
    for (int64_t i = 0; i < protocols.count; i++) {
        if (i) { header[used++] = ','; header[used++] = ' '; }
        size_t length = iron_string_byte_len(&protocols.items[i]);
        memcpy(header + used, iron_string_cstr(&protocols.items[i]), length);
        used += length;
    }
    header[used] = '\0';
    *header_length = used;
    return 1;
}

static int protocol_list_contains(Iron_List_Iron_String protocols,
                                  const char *token, size_t length) {
    for (int64_t i = 0; i < protocols.count; i++) {
        size_t candidate_length = iron_string_byte_len(&protocols.items[i]);
        if (candidate_length == length &&
            memcmp(iron_string_cstr(&protocols.items[i]), token, length) == 0)
            return 1;
    }
    return 0;
}

/* Validate a client's comma-separated offer and, when selected is non-empty,
 * prove the exact case-sensitive token was offered. */
static int request_protocol_allows(const char *headers, size_t headers_length,
                                   const char *selected,
                                   size_t selected_length) {
    size_t count = header_count(headers, headers_length,
                                "Sec-WebSocket-Protocol");
    if (count == 0) return selected_length == 0;
    if (count != 1) return 0;
    const char *value = NULL;
    size_t value_length = 0;
    if (!header_span(headers, headers_length, "Sec-WebSocket-Protocol",
                     &value, &value_length)) return 0;
    size_t pos = 0;
    int found = 0;
    while (pos < value_length) {
        while (pos < value_length &&
               (value[pos] == ' ' || value[pos] == '\t')) pos++;
        size_t end = pos;
        while (end < value_length && value[end] != ',') end++;
        size_t finish = end;
        while (finish > pos &&
               (value[finish - 1] == ' ' || value[finish - 1] == '\t'))
            finish--;
        if (!protocol_token_valid(value + pos, finish - pos)) return 0;
        if (selected_length == finish - pos &&
            memcmp(value + pos, selected, selected_length) == 0) found = 1;
        if (end == value_length) break;
        pos = end + 1;
        if (pos == value_length) return 0;
    }
    return selected_length == 0 || found;
}

static int header_has_token(const char *headers, size_t length,
                            const char *name, const char *wanted) {
    const char *value = NULL;
    size_t value_length = 0;
    if (!header_span(headers, length, name, &value, &value_length)) return 0;
    size_t wanted_length = strlen(wanted);
    size_t pos = 0;
    while (pos < value_length) {
        while (pos < value_length &&
               (value[pos] == ' ' || value[pos] == '\t' || value[pos] == ',')) pos++;
        size_t end = pos;
        while (end < value_length && value[end] != ',') end++;
        size_t trimmed = end;
        while (trimmed > pos &&
               (value[trimmed - 1] == ' ' || value[trimmed - 1] == '\t')) trimmed--;
        if (ascii_equal(value + pos, trimmed - pos, wanted, wanted_length)) return 1;
        pos = end + (end < value_length);
    }
    return 0;
}

static int utf8_valid(const uint8_t *bytes, size_t length) {
    size_t i = 0;
    while (i < length) {
        uint8_t first = bytes[i++];
        if (first <= 0x7f) continue;
        uint32_t codepoint;
        int continuation;
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1f; continuation = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0f; continuation = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07; continuation = 3;
        } else return 0;
        if (i + (size_t)continuation > length) return 0;
        for (int j = 0; j < continuation; j++) {
            uint8_t next = bytes[i++];
            if ((next & 0xc0) != 0x80) return 0;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) return 0;
    }
    return 1;
}

static int secure_random(uint8_t *bytes, size_t length) {
#ifdef _WIN32
    if (length > ULONG_MAX) return 0;
    return BCryptGenRandom(NULL, bytes, (ULONG)length,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    int fd;
    do { fd = open("/dev/urandom", O_RDONLY); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return 0;
    size_t used = 0;
    while (used < length) {
        ssize_t count = read(fd, bytes + used, length - used);
        if (count > 0) used += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else { close(fd); return 0; }
    }
    close(fd);
    return 1;
#endif
}

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *input, size_t length, char *output) {
    size_t in = 0, out = 0;
    while (in + 3 <= length) {
        uint32_t value = ((uint32_t)input[in] << 16) |
                         ((uint32_t)input[in + 1] << 8) | input[in + 2];
        output[out++] = base64_table[(value >> 18) & 63];
        output[out++] = base64_table[(value >> 12) & 63];
        output[out++] = base64_table[(value >> 6) & 63];
        output[out++] = base64_table[value & 63];
        in += 3;
    }
    if (in < length) {
        uint32_t value = (uint32_t)input[in] << 16;
        output[out++] = base64_table[(value >> 18) & 63];
        if (in + 1 < length) {
            value |= (uint32_t)input[in + 1] << 8;
            output[out++] = base64_table[(value >> 12) & 63];
            output[out++] = base64_table[(value >> 6) & 63];
            output[out++] = '=';
        } else {
            output[out++] = base64_table[(value >> 12) & 63];
            output[out++] = '=';
            output[out++] = '=';
        }
    }
    output[out] = '\0';
    return out;
}

static int base64_value(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z') return byte - 'A';
    if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
    if (byte >= '0' && byte <= '9') return byte - '0' + 52;
    if (byte == '+') return 62;
    if (byte == '/') return 63;
    return -1;
}

static int websocket_key_valid(const char *key, size_t length) {
    if (length != 24 || key[22] != '=' || key[23] != '=') return 0;
    for (size_t i = 0; i < 22; i++) {
        if (base64_value((unsigned char)key[i]) < 0) return 0;
    }
    /* A 16-byte nonce leaves four padding bits in the final Base64 digit. */
    return (base64_value((unsigned char)key[21]) & 0x0f) == 0;
}

typedef struct Sha1State {
    uint32_t hash[5];
    uint64_t length;
    uint8_t block[64];
    size_t used;
} Sha1State;

static uint32_t rotate_left(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_block(Sha1State *state, const uint8_t *block) {
    uint32_t words[80];
    for (int i = 0; i < 16; i++) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++)
        words[i] = rotate_left(words[i - 3] ^ words[i - 8] ^
                               words[i - 14] ^ words[i - 16], 1);
    uint32_t a = state->hash[0], b = state->hash[1], c = state->hash[2];
    uint32_t d = state->hash[3], e = state->hash[4];
    for (int i = 0; i < 80; i++) {
        uint32_t function, constant;
        if (i < 20) { function = (b & c) | ((~b) & d); constant = 0x5a827999; }
        else if (i < 40) { function = b ^ c ^ d; constant = 0x6ed9eba1; }
        else if (i < 60) { function = (b & c) | (b & d) | (c & d); constant = 0x8f1bbcdc; }
        else { function = b ^ c ^ d; constant = 0xca62c1d6; }
        uint32_t temporary = rotate_left(a, 5) + function + e +
                             constant + words[i];
        e = d; d = c; c = rotate_left(b, 30); b = a; a = temporary;
    }
    state->hash[0] += a; state->hash[1] += b; state->hash[2] += c;
    state->hash[3] += d; state->hash[4] += e;
}

static void sha1_update(Sha1State *state, const uint8_t *bytes, size_t length) {
    state->length += (uint64_t)length * 8;
    while (length > 0) {
        size_t count = 64 - state->used;
        if (count > length) count = length;
        memcpy(state->block + state->used, bytes, count);
        state->used += count; bytes += count; length -= count;
        if (state->used == 64) { sha1_block(state, state->block); state->used = 0; }
    }
}

static void sha1_digest(const uint8_t *bytes, size_t length, uint8_t output[20]) {
    Sha1State state = { { 0x67452301, 0xefcdab89, 0x98badcfe,
                          0x10325476, 0xc3d2e1f0 }, 0, {0}, 0 };
    sha1_update(&state, bytes, length);
    uint64_t bit_length = state.length;
    uint8_t marker = 0x80;
    sha1_update(&state, &marker, 1);
    uint8_t zero = 0;
    while (state.used != 56) sha1_update(&state, &zero, 1);
    uint8_t encoded_length[8];
    for (int i = 0; i < 8; i++)
        encoded_length[7 - i] = (uint8_t)(bit_length >> (i * 8));
    sha1_update(&state, encoded_length, 8);
    for (int i = 0; i < 5; i++) {
        output[i * 4] = (uint8_t)(state.hash[i] >> 24);
        output[i * 4 + 1] = (uint8_t)(state.hash[i] >> 16);
        output[i * 4 + 2] = (uint8_t)(state.hash[i] >> 8);
        output[i * 4 + 3] = (uint8_t)state.hash[i];
    }
}

static void websocket_accept(const char *key, size_t key_length, char output[29]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t input[24 + sizeof(guid) - 1];
    memcpy(input, key, key_length);
    memcpy(input + key_length, guid, sizeof(guid) - 1);
    uint8_t digest[20];
    sha1_digest(input, key_length + sizeof(guid) - 1, digest);
    base64_encode(digest, sizeof(digest), output);
}

static Iron_Result_Int_Error transport_read(WsTransport transport,
                                             uint8_t *buffer, int64_t capacity,
                                             Iron_Deadline deadline) {
    if (transport.tls)
        return iron_tls_read(transport.tls, buffer, capacity, deadline);
    return Iron_net_tcp_recv_bytes(transport.socket, buffer, capacity,
                                    Iron_deadline_remaining_ms(deadline));
}

static Iron_Result_Int_Error transport_write(WsTransport transport,
                                              const uint8_t *buffer,
                                              int64_t length,
                                              Iron_Deadline deadline) {
    if (transport.tls)
        return iron_tls_write(transport.tls, buffer, length, deadline);
    return Iron_net_tcp_send_bytes(transport.socket, buffer, length,
                                    Iron_deadline_remaining_ms(deadline));
}

static void transport_close(WsTransport transport) {
    if (transport.tls) iron_tls_stream_close(transport.tls);
    Iron_tcpsocket_close(transport.socket);
}

static int64_t read_exact(WsSession *session, uint8_t *buffer, size_t length,
                          Iron_Deadline deadline) {
    size_t used = 0;
    if (session->prefetch_pos < session->prefetch_len) {
        size_t available = session->prefetch_len - session->prefetch_pos;
        size_t count = available < length ? available : length;
        memcpy(buffer, session->prefetch + session->prefetch_pos, count);
        session->prefetch_pos += count;
        used += count;
        if (session->prefetch_pos == session->prefetch_len) {
            free(session->prefetch);
            session->prefetch = NULL;
            session->prefetch_pos = session->prefetch_len = 0;
        }
    }
    while (used < length) {
        size_t remaining = length - used;
        int64_t request = remaining > INT_MAX ? INT_MAX : (int64_t)remaining;
        Iron_Result_Int_Error read = transport_read(
            session->transport, buffer + used, request, deadline);
        if (read.v1.code != 0) return read.v1.code;
        if (read.v0 <= 0) return IRON_ERR_WS_CLOSED;
        used += (size_t)read.v0;
    }
    return 0;
}

static int64_t write_all(WsTransport transport, const uint8_t *bytes,
                         size_t length, Iron_Deadline deadline) {
    size_t sent = 0;
    while (sent < length) {
        size_t remaining = length - sent;
        int64_t request = remaining > INT_MAX ? INT_MAX : (int64_t)remaining;
        Iron_Result_Int_Error write = transport_write(
            transport, bytes + sent, request, deadline);
        if (write.v1.code != 0) return write.v1.code;
        if (write.v0 <= 0) return IRON_ERR_WS_CLOSED;
        sent += (size_t)write.v0;
    }
    return 0;
}

static void lock_send(WsSession *session) {
#ifdef _WIN32
    EnterCriticalSection(&session->send_lock);
#else
    pthread_mutex_lock(&session->send_lock);
#endif
}

static void unlock_send(WsSession *session) {
#ifdef _WIN32
    LeaveCriticalSection(&session->send_lock);
#else
    pthread_mutex_unlock(&session->send_lock);
#endif
}

static WsSession *session_new(WsTransport transport, int is_client,
                              size_t max_message) {
    WsSession *session = (WsSession *)calloc(1, sizeof(*session));
    if (!session) return NULL;
    session->transport = transport;
    session->is_client = is_client;
    session->max_message = max_message;
    session->open = 1;
#ifdef _WIN32
    InitializeCriticalSection(&session->send_lock);
#else
    if (pthread_mutex_init(&session->send_lock, NULL) != 0) {
        free(session);
        return NULL;
    }
#endif
    return session;
}

static void session_free(WsSession *session) {
    if (!session) return;
    free(session->fragment);
    free(session->prefetch);
#ifdef _WIN32
    DeleteCriticalSection(&session->send_lock);
#else
    pthread_mutex_destroy(&session->send_lock);
#endif
    free(session);
}

static int64_t send_frame(WsSession *session, uint8_t opcode,
                          const uint8_t *payload, size_t length,
                          Iron_Deadline deadline) {
    if (!session || !session->open) return IRON_ERR_WS_CLOSED;
    if ((opcode & 0x08) && length > 125) return IRON_ERR_WS_INVALID_ARGUMENT;
    if (!(opcode & 0x08) && length > session->max_message)
        return IRON_ERR_WS_MESSAGE_TOO_LARGE;
    uint8_t header[14];
    size_t header_length = 2;
    header[0] = (uint8_t)(0x80 | opcode);
    uint8_t masked = session->is_client ? 0x80 : 0;
    if (length < 126) header[1] = (uint8_t)(masked | length);
    else if (length <= 0xffff) {
        header[1] = (uint8_t)(masked | 126);
        header[2] = (uint8_t)(length >> 8);
        header[3] = (uint8_t)length;
        header_length = 4;
    } else {
        header[1] = (uint8_t)(masked | 127);
        uint64_t encoded = (uint64_t)length;
        for (int i = 0; i < 8; i++) header[2 + i] = (uint8_t)(encoded >> (56 - i * 8));
        header_length = 10;
    }
    uint8_t mask[4];
    if (session->is_client) {
        if (!secure_random(mask, sizeof(mask))) return IRON_ERR_WS_PROTOCOL;
        memcpy(header + header_length, mask, sizeof(mask));
        header_length += sizeof(mask);
    }
    lock_send(session);
    int64_t error = write_all(session->transport, header, header_length, deadline);
    if (!error && length > 0) {
        if (!session->is_client) {
            error = write_all(session->transport, payload, length, deadline);
        } else {
            uint8_t buffer[4096];
            size_t offset = 0;
            while (!error && offset < length) {
                size_t count = length - offset;
                if (count > sizeof(buffer)) count = sizeof(buffer);
                for (size_t i = 0; i < count; i++)
                    buffer[i] = payload[offset + i] ^ mask[(offset + i) & 3];
                error = write_all(session->transport, buffer, count, deadline);
                offset += count;
            }
        }
    }
    unlock_send(session);
    return error;
}

static int close_code_valid(uint16_t code) {
    if (code >= 3000 && code <= 4999) return 1;
    switch (code) {
        case 1000: case 1001: case 1002: case 1003: case 1007:
        case 1008: case 1009: case 1010: case 1011: case 1012:
        case 1013: case 1014: return 1;
        default: return 0;
    }
}

static int fragment_append(WsSession *session, const uint8_t *bytes,
                           size_t length) {
    if (length > session->max_message - session->fragment_len) return 0;
    size_t required = session->fragment_len + length;
    if (required > session->fragment_cap) {
        size_t capacity = session->fragment_cap ? session->fragment_cap : 256;
        while (capacity < required) {
            if (capacity > session->max_message / 2) { capacity = session->max_message; break; }
            capacity *= 2;
        }
        uint8_t *grown = (uint8_t *)realloc(session->fragment, capacity);
        if (!grown) return -1;
        session->fragment = grown;
        session->fragment_cap = capacity;
    }
    if (length) memcpy(session->fragment + session->fragment_len, bytes, length);
    session->fragment_len = required;
    return 1;
}

static void best_effort_protocol_close(WsSession *session, uint16_t code,
                                       Iron_Deadline deadline) {
    if (!session->close_sent && session->open) {
        uint8_t payload[2] = { (uint8_t)(code >> 8), (uint8_t)code };
        if (send_frame(session, 8, payload, sizeof(payload), deadline) == 0)
            session->close_sent = 1;
    }
}

Iron_WebSocketMessage Iron_websocket_receive(Iron_WebSocket socket,
                                              int64_t timeout) {
    WsSession *session = (WsSession *)(intptr_t)socket.handle;
    if (!session || timeout < 0) return ws_message_error(IRON_ERR_WS_INVALID_ARGUMENT);
    if (!session->open) return ws_message_error(IRON_ERR_WS_CLOSED);
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
next_frame: ;
    uint8_t head[2];
    int64_t error = read_exact(session, head, sizeof(head), deadline);
    if (error) return ws_message_error(error);
    int fin = (head[0] & 0x80) != 0;
    uint8_t opcode = head[0] & 0x0f;
    int masked = (head[1] & 0x80) != 0;
    uint64_t length = head[1] & 0x7f;
    int control = (opcode & 0x08) != 0;
    if ((head[0] & 0x70) != 0 || (masked != !session->is_client) ||
        !(opcode == 0 || opcode == 1 || opcode == 2 || opcode == 8 ||
          opcode == 9 || opcode == 10) ||
        (control && (!fin || length > 125)) ||
        (opcode == 0 && session->fragment_opcode == 0) ||
        ((opcode == 1 || opcode == 2) && session->fragment_opcode != 0)) {
        best_effort_protocol_close(session, 1002, deadline);
        return ws_message_error(IRON_ERR_WS_PROTOCOL);
    }
    if (length == 126) {
        uint8_t extended[2];
        error = read_exact(session, extended, sizeof(extended), deadline);
        if (error) return ws_message_error(error);
        length = ((uint64_t)extended[0] << 8) | extended[1];
        if (length < 126) {
            best_effort_protocol_close(session, 1002, deadline);
            return ws_message_error(IRON_ERR_WS_PROTOCOL);
        }
    } else if (length == 127) {
        uint8_t extended[8];
        error = read_exact(session, extended, sizeof(extended), deadline);
        if (error) return ws_message_error(error);
        if ((extended[0] & 0x80) != 0) {
            best_effort_protocol_close(session, 1002, deadline);
            return ws_message_error(IRON_ERR_WS_PROTOCOL);
        }
        length = 0;
        for (int i = 0; i < 8; i++) length = (length << 8) | extended[i];
        if (length <= 0xffff) {
            best_effort_protocol_close(session, 1002, deadline);
            return ws_message_error(IRON_ERR_WS_PROTOCOL);
        }
    }
    if (control && length > 125) {
        best_effort_protocol_close(session, 1002, deadline);
        return ws_message_error(IRON_ERR_WS_PROTOCOL);
    }
    if (length > SIZE_MAX || (!control && length > session->max_message) ||
        (!control && length > session->max_message - session->fragment_len)) {
        best_effort_protocol_close(session, 1009, deadline);
        return ws_message_error(IRON_ERR_WS_MESSAGE_TOO_LARGE);
    }
    uint8_t mask[4] = {0};
    if (masked) {
        error = read_exact(session, mask, sizeof(mask), deadline);
        if (error) return ws_message_error(error);
    }
    uint8_t *payload = NULL;
    if (length > 0) {
        payload = (uint8_t *)malloc((size_t)length);
        if (!payload) return ws_message_error(IRON_ERR_WS_NO_MEMORY);
        error = read_exact(session, payload, (size_t)length, deadline);
        if (error) { free(payload); return ws_message_error(error); }
        if (masked) {
            for (size_t i = 0; i < (size_t)length; i++) payload[i] ^= mask[i & 3];
        }
    }

    Iron_WebSocketMessage message = ws_message_empty();
    if (control) {
        message.kind = opcode;
        if (opcode == 8) {
            if (length == 1) {
                free(payload);
                best_effort_protocol_close(session, 1002, deadline);
                return ws_message_error(IRON_ERR_WS_PROTOCOL);
            }
            uint16_t code = 1005;
            const uint8_t *reason = payload;
            size_t reason_length = (size_t)length;
            if (length >= 2) {
                code = (uint16_t)(((uint16_t)payload[0] << 8) | payload[1]);
                reason = payload + 2;
                reason_length -= 2;
                if (!close_code_valid(code)) {
                    free(payload);
                    best_effort_protocol_close(session, 1002, deadline);
                    return ws_message_error(IRON_ERR_WS_PROTOCOL);
                }
            }
            if (!utf8_valid(reason, reason_length)) {
                free(payload);
                best_effort_protocol_close(session, 1007, deadline);
                return ws_message_error(IRON_ERR_WS_INVALID_UTF8);
            }
            session->close_received = 1;
            message.close_code = code;
            message.data = ws_string((const char *)reason, reason_length);
            if (!session->close_sent) {
                if (send_frame(session, 8, payload, (size_t)length, deadline) == 0)
                    session->close_sent = 1;
            }
        } else {
            message.data = ws_string((const char *)payload, (size_t)length);
            if (opcode == 9) {
                error = send_frame(session, 10, payload, (size_t)length, deadline);
                if (error) { free(payload); return ws_message_error(error); }
            }
        }
        free(payload);
        return message;
    }

    if (opcode == 1 || opcode == 2) {
        if (fin) {
            if (opcode == 1 && !utf8_valid(payload, (size_t)length)) {
                free(payload);
                best_effort_protocol_close(session, 1007, deadline);
                return ws_message_error(IRON_ERR_WS_INVALID_UTF8);
            }
            message.kind = opcode;
            message.data = ws_string((const char *)payload, (size_t)length);
            free(payload);
            return message;
        }
        session->fragment_opcode = opcode;
    }
    int append = fragment_append(session, payload, (size_t)length);
    free(payload);
    if (append == 0) {
        best_effort_protocol_close(session, 1009, deadline);
        return ws_message_error(IRON_ERR_WS_MESSAGE_TOO_LARGE);
    }
    if (append < 0) return ws_message_error(IRON_ERR_WS_NO_MEMORY);
    if (!fin) goto next_frame;

    uint8_t message_opcode = session->fragment_opcode;
    if (message_opcode == 1 &&
        !utf8_valid(session->fragment, session->fragment_len)) {
        session->fragment_len = 0;
        session->fragment_opcode = 0;
        best_effort_protocol_close(session, 1007, deadline);
        return ws_message_error(IRON_ERR_WS_INVALID_UTF8);
    }
    message.kind = message_opcode;
    message.data = ws_string((const char *)session->fragment, session->fragment_len);
    session->fragment_len = 0;
    session->fragment_opcode = 0;
    return message;
}

static int custom_headers_valid(Iron_String headers) {
    const char *bytes = iron_string_cstr(&headers);
    size_t length = iron_string_byte_len(&headers);
    if (length > WS_MAX_HEADER) return 0;
    size_t pos = 0;
    while (pos < length) {
        size_t end = pos;
        while (end + 1 < length && !(bytes[end] == '\r' && bytes[end + 1] == '\n')) {
            unsigned char byte = (unsigned char)bytes[end];
            if ((byte < 32 && byte != '\t') || byte == 127) return 0;
            end++;
        }
        size_t line_end = end + 1 < length ? end : length;
        size_t colon = pos;
        while (colon < line_end && bytes[colon] != ':') colon++;
        if (colon == pos || colon == line_end) return 0;
        if (ascii_equal(bytes + pos, colon - pos, "Host", 4) ||
            ascii_equal(bytes + pos, colon - pos, "Connection", 10) ||
            ascii_equal(bytes + pos, colon - pos, "Upgrade", 7) ||
            ascii_equal(bytes + pos, colon - pos, "Sec-WebSocket-Key", 17) ||
            ascii_equal(bytes + pos, colon - pos, "Sec-WebSocket-Version", 21) ||
            ascii_equal(bytes + pos, colon - pos, "Sec-WebSocket-Extensions", 24) ||
            ascii_equal(bytes + pos, colon - pos, "Sec-WebSocket-Protocol", 22)) return 0;
        pos = line_end < length ? line_end + 2 : length;
    }
    return 1;
}

static int response_headers_valid(const char *headers, size_t length) {
    size_t pos = 0;
    while (pos < length) {
        size_t end = pos;
        while (end + 1 < length &&
               !(headers[end] == '\r' && headers[end + 1] == '\n')) {
            unsigned char byte = (unsigned char)headers[end];
            if ((byte < 32 && byte != '\t') || byte == 127) return 0;
            end++;
        }
        size_t line_end = end + 1 < length ? end : length;
        size_t colon = pos;
        while (colon < line_end && headers[colon] != ':') {
            unsigned char byte = (unsigned char)headers[colon];
            if (!(isalnum(byte) || byte == '!' || byte == '#' || byte == '$' ||
                  byte == '%' || byte == '&' || byte == '\'' || byte == '*' ||
                  byte == '+' || byte == '-' || byte == '.' || byte == '^' ||
                  byte == '_' || byte == '`' || byte == '|' || byte == '~')) return 0;
            colon++;
        }
        if (colon == pos || colon == line_end) return 0;
        pos = line_end < length ? line_end + 2 : length;
    }
    return 1;
}

static WsUrl parse_ws_url(Iron_String url) {
    WsUrl parsed;
    memset(&parsed, 0, sizeof(parsed));
    const char *bytes = iron_string_cstr(&url);
    size_t length = iron_string_byte_len(&url);
    if (length == 0 || length > WS_MAX_URL ||
        memchr(bytes, '\0', length) != NULL) {
        parsed.error = IRON_ERR_WS_BAD_URL;
        return parsed;
    }
    size_t pos;
    if (length >= 5 && ascii_equal(bytes, 5, "ws://", 5)) pos = 5;
    else if (length >= 6 && ascii_equal(bytes, 6, "wss://", 6)) {
        pos = 6; parsed.secure = 1;
    } else { parsed.error = IRON_ERR_WS_BAD_URL; return parsed; }
    size_t authority_end = pos;
    while (authority_end < length && bytes[authority_end] != '/' &&
           bytes[authority_end] != '?' && bytes[authority_end] != '#') authority_end++;
    if (authority_end == pos ||
        (authority_end < length && bytes[authority_end] == '#')) {
        parsed.error = IRON_ERR_WS_BAD_URL; return parsed;
    }
    size_t host_start = pos, host_end = authority_end, port_start = SIZE_MAX;
    int bracketed = 0;
    if (bytes[pos] == '[') {
        bracketed = 1; host_start = ++pos; host_end = pos;
        while (host_end < authority_end && bytes[host_end] != ']') host_end++;
        if (host_end == authority_end || host_end == host_start) {
            parsed.error = IRON_ERR_WS_BAD_URL; return parsed;
        }
        if (host_end + 1 < authority_end) {
            if (bytes[host_end + 1] != ':') { parsed.error = IRON_ERR_WS_BAD_URL; return parsed; }
            port_start = host_end + 2;
        }
    } else {
        int colon_seen = 0;
        for (size_t i = pos; i < authority_end; i++) {
            if (bytes[i] == '@' || (unsigned char)bytes[i] <= 32) {
                parsed.error = IRON_ERR_WS_BAD_URL; return parsed;
            }
            if (bytes[i] == ':') {
                if (colon_seen) { parsed.error = IRON_ERR_WS_BAD_URL; return parsed; }
                colon_seen = 1; host_end = i; port_start = i + 1;
            }
        }
    }
    size_t host_length = host_end - host_start;
    if (host_length == 0 || host_length > WS_MAX_HOST) {
        parsed.error = IRON_ERR_WS_BAD_URL; return parsed;
    }
    memcpy(parsed.host, bytes + host_start, host_length);
    parsed.host[host_length] = '\0';
    parsed.port = parsed.secure ? 443 : 80;
    if (port_start != SIZE_MAX) {
        if (port_start >= authority_end) { parsed.error = IRON_ERR_WS_BAD_URL; return parsed; }
        int64_t port = 0;
        for (size_t i = port_start; i < authority_end; i++) {
            if (!isdigit((unsigned char)bytes[i])) { parsed.error = IRON_ERR_WS_BAD_URL; return parsed; }
            port = port * 10 + bytes[i] - '0';
            if (port > 65535) { parsed.error = IRON_ERR_WS_BAD_URL; return parsed; }
        }
        if (port == 0) { parsed.error = IRON_ERR_WS_BAD_URL; return parsed; }
        parsed.port = port;
    }
    int default_port = (!parsed.secure && parsed.port == 80) ||
                       (parsed.secure && parsed.port == 443);
    if (bracketed) snprintf(parsed.host_header, sizeof(parsed.host_header),
        default_port ? "[%s]" : "[%s]:%lld", parsed.host, (long long)parsed.port);
    else snprintf(parsed.host_header, sizeof(parsed.host_header),
        default_port ? "%s" : "%s:%lld", parsed.host, (long long)parsed.port);
    size_t target_length = length - authority_end;
    if (target_length == 0) strcpy(parsed.target, "/");
    else {
        if (memchr(bytes + authority_end, '#', target_length) != NULL) {
            parsed.error = IRON_ERR_WS_BAD_URL; return parsed;
        }
        if (bytes[authority_end] == '?') {
            parsed.target[0] = '/';
            memcpy(parsed.target + 1, bytes + authority_end, target_length);
            parsed.target[target_length + 1] = '\0';
        } else {
            memcpy(parsed.target, bytes + authority_end, target_length);
            parsed.target[target_length] = '\0';
        }
    }
    return parsed;
}

static int64_t receive_http_head(WsTransport transport, uint8_t **bytes_out,
                                 size_t *length_out, size_t *marker_out,
                                 Iron_Deadline deadline) {
    size_t capacity = 1024, used = 0;
    uint8_t *bytes = (uint8_t *)malloc(capacity);
    if (!bytes) return IRON_ERR_WS_NO_MEMORY;
    for (;;) {
        for (size_t i = 0; i + 3 < used; i++) {
            if (memcmp(bytes + i, "\r\n\r\n", 4) == 0) {
                *bytes_out = bytes; *length_out = used; *marker_out = i;
                return 0;
            }
        }
        if (used == WS_MAX_HEADER) { free(bytes); return IRON_ERR_WS_HANDSHAKE; }
        if (used == capacity) {
            size_t next = capacity * 2;
            if (next > WS_MAX_HEADER) next = WS_MAX_HEADER;
            uint8_t *grown = (uint8_t *)realloc(bytes, next);
            if (!grown) { free(bytes); return IRON_ERR_WS_NO_MEMORY; }
            bytes = grown; capacity = next;
        }
        Iron_Result_Int_Error read = transport_read(
            transport, bytes + used, (int64_t)(capacity - used), deadline);
        if (read.v1.code != 0) { free(bytes); return read.v1.code; }
        if (read.v0 <= 0) { free(bytes); return IRON_ERR_WS_HANDSHAKE; }
        used += (size_t)read.v0;
    }
}

static Iron_WebSocketResult websocket_connect_options(
    Iron_String url, Iron_String headers, Iron_String ca_file, int insecure,
    Iron_List_Iron_String protocols, int64_t max_message_bytes,
    int64_t timeout) {
    char protocol_header[WS_MAX_HEADER + 1];
    size_t protocol_header_length = 0;
    if (max_message_bytes <= 0 || max_message_bytes > UINT32_MAX || timeout < 0 ||
        !custom_headers_valid(headers) ||
        !protocol_list_build(protocols, protocol_header,
                             sizeof(protocol_header),
                             &protocol_header_length))
        return ws_result_error(IRON_ERR_WS_INVALID_ARGUMENT);
    WsUrl parsed = parse_ws_url(url);
    if (parsed.error) return ws_result_error(parsed.error);
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
    Iron_String host = ws_cstr(parsed.host);
    Iron_Result_TcpSocket_Error dial = Iron_net_tcp_dial(
        host, parsed.port, Iron_deadline_remaining_ms(deadline));
    if (dial.v1.code != 0) return ws_result_error(dial.v1.code);
    WsTransport transport = { dial.v0, NULL };
    if (parsed.secure) {
        Iron_TlsStreamResult tls = iron_tls_client_connect(
            dial.v0, host, ca_file, insecure != 0, deadline);
        if (tls.error.code != 0) {
            Iron_tcpsocket_close(dial.v0);
            return ws_result_error(tls.error.code);
        }
        transport.tls = tls.stream;
    }
    uint8_t nonce[16];
    if (!secure_random(nonce, sizeof(nonce))) {
        transport_close(transport);
        return ws_result_error(IRON_ERR_WS_HANDSHAKE);
    }
    char key[25], expected_accept[29];
    base64_encode(nonce, sizeof(nonce), key);
    websocket_accept(key, strlen(key), expected_accept);
    const char *custom = iron_string_cstr(&headers);
    size_t custom_length = iron_string_byte_len(&headers);
    size_t capacity = strlen(parsed.target) + strlen(parsed.host_header) +
                      custom_length + protocol_header_length + 544;
    char *request = (char *)malloc(capacity);
    if (!request) { transport_close(transport); return ws_result_error(IRON_ERR_WS_NO_MEMORY); }
    int request_length = snprintf(request, capacity,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n",
        parsed.target, parsed.host_header, key);
    if (request_length < 0 || (size_t)request_length >= capacity) {
        free(request); transport_close(transport);
        return ws_result_error(IRON_ERR_WS_HANDSHAKE);
    }
    size_t used = (size_t)request_length;
    if (protocol_header_length) {
        static const char protocol_prefix[] = "Sec-WebSocket-Protocol: ";
        memcpy(request + used, protocol_prefix, sizeof(protocol_prefix) - 1);
        used += sizeof(protocol_prefix) - 1;
        memcpy(request + used, protocol_header, protocol_header_length);
        used += protocol_header_length;
        memcpy(request + used, "\r\n", 2);
        used += 2;
    }
    if (custom_length) {
        memcpy(request + used, custom, custom_length); used += custom_length;
        if (custom_length < 2 || custom[custom_length - 2] != '\r' ||
            custom[custom_length - 1] != '\n') {
            memcpy(request + used, "\r\n", 2); used += 2;
        }
    }
    memcpy(request + used, "\r\n", 2); used += 2;
    int64_t error = write_all(transport, (const uint8_t *)request, used, deadline);
    free(request);
    if (error) { transport_close(transport); return ws_result_error(error); }

    uint8_t *response = NULL;
    size_t response_length = 0, marker = 0;
    error = receive_http_head(transport, &response, &response_length, &marker, deadline);
    if (error) { transport_close(transport); return ws_result_error(error); }
    size_t line_end = 0;
    while (line_end + 1 < marker &&
           !(response[line_end] == '\r' && response[line_end + 1] == '\n')) line_end++;
    const char *header_bytes = (const char *)response + line_end + 2;
    size_t header_length = marker - (line_end + 2);
    const char *accept = NULL;
    size_t accept_length = 0;
    const char *selected_protocol = NULL;
    size_t selected_protocol_length = 0;
    size_t protocol_response_count = header_count(
        header_bytes, header_length, "Sec-WebSocket-Protocol");
    int protocol_response_valid = protocol_response_count == 0 ||
        (protocol_response_count == 1 &&
         header_span(header_bytes, header_length, "Sec-WebSocket-Protocol",
                     &selected_protocol, &selected_protocol_length) &&
         protocol_token_valid(selected_protocol, selected_protocol_length) &&
         protocol_list_contains(protocols, selected_protocol,
                                selected_protocol_length));
    int valid = line_end >= 13 &&
        (memcmp(response, "HTTP/1.1 101", 12) == 0) &&
        response[12] == ' ' &&
        response_headers_valid(header_bytes, header_length) &&
        header_has_token(header_bytes, header_length, "Upgrade", "websocket") &&
        header_has_token(header_bytes, header_length, "Connection", "Upgrade") &&
        header_span(header_bytes, header_length, "Sec-WebSocket-Accept",
                    &accept, &accept_length) &&
        header_count(header_bytes, header_length, "Sec-WebSocket-Accept") == 1 &&
        header_count(header_bytes, header_length, "Sec-WebSocket-Extensions") == 0 &&
        protocol_response_valid &&
        header_count(header_bytes, header_length, "Content-Length") == 0 &&
        header_count(header_bytes, header_length, "Transfer-Encoding") == 0 &&
        accept_length == strlen(expected_accept) &&
        memcmp(accept, expected_accept, accept_length) == 0;
    if (!valid) {
        free(response); transport_close(transport);
        return ws_result_error(IRON_ERR_WS_HANDSHAKE);
    }
    WsSession *session = session_new(transport, 1, (size_t)max_message_bytes);
    if (!session) {
        free(response); transport_close(transport);
        return ws_result_error(IRON_ERR_WS_NO_MEMORY);
    }
    size_t extra_start = marker + 4;
    if (response_length > extra_start) {
        session->prefetch_len = response_length - extra_start;
        session->prefetch = (uint8_t *)malloc(session->prefetch_len);
        if (!session->prefetch) {
            free(response); transport_close(transport); session_free(session);
            return ws_result_error(IRON_ERR_WS_NO_MEMORY);
        }
        memcpy(session->prefetch, response + extra_start, session->prefetch_len);
    }
    Iron_String negotiated_protocol = selected_protocol
        ? ws_string(selected_protocol, selected_protocol_length)
        : ws_cstr("");
    free(response);
    Iron_WebSocketResult result = ws_result_error(0);
    result.socket.handle = (int64_t)(intptr_t)session;
    result.protocol = negotiated_protocol;
    return result;
}

Iron_WebSocketResult Iron_websocket_connect(Iron_String url,
                                             Iron_String headers,
                                             int64_t max_message_bytes,
                                             int64_t timeout) {
    Iron_List_Iron_String protocols = { NULL, 0, 0 };
    return websocket_connect_options(url, headers, ws_cstr(""), 0, protocols,
                                     max_message_bytes, timeout);
}

Iron_WebSocketResult Iron_websocket_connect_with_ca(Iron_String url,
                                                     Iron_String headers,
                                                     Iron_String ca_file,
                                                     int64_t max_message_bytes,
                                                     int64_t timeout) {
    Iron_List_Iron_String protocols = { NULL, 0, 0 };
    return websocket_connect_options(url, headers, ca_file, 0, protocols,
                                     max_message_bytes, timeout);
}

Iron_WebSocketResult Iron_websocket_connect_insecure(Iron_String url,
                                                      Iron_String headers,
                                                      int64_t max_message_bytes,
                                                      int64_t timeout) {
    Iron_List_Iron_String protocols = { NULL, 0, 0 };
    return websocket_connect_options(url, headers, ws_cstr(""), 1, protocols,
                                     max_message_bytes, timeout);
}

Iron_WebSocketResult Iron_websocket_connect_with_protocols(
    Iron_String url, Iron_String headers, Iron_List_Iron_String protocols,
    int64_t max_message_bytes, int64_t timeout) {
    return websocket_connect_options(url, headers, ws_cstr(""), 0, protocols,
                                     max_message_bytes, timeout);
}

Iron_WebSocketResult Iron_websocket_connect_with_ca_and_protocols(
    Iron_String url, Iron_String headers, Iron_String ca_file,
    Iron_List_Iron_String protocols, int64_t max_message_bytes,
    int64_t timeout) {
    return websocket_connect_options(url, headers, ca_file, 0, protocols,
                                     max_message_bytes, timeout);
}

Iron_WebSocketResult Iron_websocket_connect_insecure_with_protocols(
    Iron_String url, Iron_String headers, Iron_List_Iron_String protocols,
    int64_t max_message_bytes, int64_t timeout) {
    return websocket_connect_options(url, headers, ws_cstr(""), 1, protocols,
                                     max_message_bytes, timeout);
}

static Iron_WebSocketResult upgrade_server(WsTransport transport,
                                            Iron_HttpRequest request,
                                            Iron_String protocol,
                                            int64_t max_message_bytes,
                                            int64_t timeout) {
    if (max_message_bytes <= 0 || max_message_bytes > UINT32_MAX || timeout < 0)
        return ws_result_error(IRON_ERR_WS_INVALID_ARGUMENT);
    const char *method = iron_string_cstr(&request.method);
    size_t method_length = iron_string_byte_len(&request.method);
    const char *version = iron_string_cstr(&request.version);
    size_t version_length = iron_string_byte_len(&request.version);
    size_t body_length = iron_string_byte_len(&request.body);
    const char *headers = iron_string_cstr(&request.headers);
    size_t headers_length = iron_string_byte_len(&request.headers);
    const char *selected_protocol = iron_string_cstr(&protocol);
    size_t selected_protocol_length = iron_string_byte_len(&protocol);
    const char *key = NULL, *ws_version = NULL;
    size_t key_length = 0, ws_version_length = 0;
    if (request.error != 0 || body_length != 0 ||
        !ascii_equal(method, method_length, "GET", 3) ||
        !ascii_equal(version, version_length, "HTTP/1.1", 8) ||
        !header_has_token(headers, headers_length, "Upgrade", "websocket") ||
        !header_has_token(headers, headers_length, "Connection", "Upgrade") ||
        !header_span(headers, headers_length, "Sec-WebSocket-Version",
                     &ws_version, &ws_version_length) ||
        header_count(headers, headers_length, "Sec-WebSocket-Version") != 1 ||
        !ascii_equal(ws_version, ws_version_length, "13", 2) ||
        !header_span(headers, headers_length, "Sec-WebSocket-Key",
                     &key, &key_length) ||
        header_count(headers, headers_length, "Sec-WebSocket-Key") != 1 ||
        !websocket_key_valid(key, key_length) ||
        (selected_protocol_length > 0 &&
         !protocol_token_valid(selected_protocol, selected_protocol_length)) ||
        !request_protocol_allows(headers, headers_length, selected_protocol,
                                 selected_protocol_length))
        return ws_result_error(IRON_ERR_WS_HANDSHAKE);
    char accept[29];
    websocket_accept(key, key_length, accept);
    size_t response_capacity = selected_protocol_length + 320;
    char *response = (char *)malloc(response_capacity);
    if (!response) return ws_result_error(IRON_ERR_WS_NO_MEMORY);
    int length = snprintf(response, response_capacity,
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n%s%s%s",
        accept,
        selected_protocol_length ? "Sec-WebSocket-Protocol: " : "",
        selected_protocol_length ? selected_protocol : "",
        selected_protocol_length ? "\r\n\r\n" : "\r\n");
    if (length < 0 || (size_t)length >= response_capacity) {
        free(response);
        return ws_result_error(IRON_ERR_WS_HANDSHAKE);
    }
    WsSession *session = session_new(transport, 0, (size_t)max_message_bytes);
    if (!session) {
        free(response);
        return ws_result_error(IRON_ERR_WS_NO_MEMORY);
    }
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
    int64_t error = write_all(transport, (const uint8_t *)response,
                              (size_t)length, deadline);
    free(response);
    if (error) {
        session_free(session);
        return ws_result_error(error);
    }
    Iron_WebSocketResult result = ws_result_error(0);
    result.socket.handle = (int64_t)(intptr_t)session;
    result.protocol = ws_string(selected_protocol, selected_protocol_length);
    return result;
}

Iron_WebSocketResult Iron_httpconnection_upgrade_websocket(
    Iron_HttpConnection connection, Iron_HttpRequest request,
    int64_t max_message_bytes, int64_t timeout) {
    WsTransport transport = { { connection.fd }, NULL };
    return upgrade_server(transport, request, ws_cstr(""),
                          max_message_bytes, timeout);
}

Iron_WebSocketResult Iron_httpsconnection_upgrade_websocket(
    Iron_HttpsConnection connection, Iron_HttpRequest request,
    int64_t max_message_bytes, int64_t timeout) {
    WsTransport transport = { { connection.fd },
        (Iron_TlsStream *)(intptr_t)connection.tls };
    return upgrade_server(transport, request, ws_cstr(""),
                          max_message_bytes, timeout);
}

Iron_WebSocketResult Iron_httpconnection_upgrade_websocket_protocol(
    Iron_HttpConnection connection, Iron_HttpRequest request,
    Iron_String protocol, int64_t max_message_bytes, int64_t timeout) {
    WsTransport transport = { { connection.fd }, NULL };
    return upgrade_server(transport, request, protocol,
                          max_message_bytes, timeout);
}

Iron_WebSocketResult Iron_httpsconnection_upgrade_websocket_protocol(
    Iron_HttpsConnection connection, Iron_HttpRequest request,
    Iron_String protocol, int64_t max_message_bytes, int64_t timeout) {
    WsTransport transport = { { connection.fd },
        (Iron_TlsStream *)(intptr_t)connection.tls };
    return upgrade_server(transport, request, protocol,
                          max_message_bytes, timeout);
}

int64_t Iron_websocket_send_text(Iron_WebSocket socket, Iron_String data,
                                  int64_t timeout) {
    WsSession *session = (WsSession *)(intptr_t)socket.handle;
    const uint8_t *bytes = (const uint8_t *)iron_string_cstr(&data);
    size_t length = iron_string_byte_len(&data);
    if (!session || timeout < 0) return IRON_ERR_WS_INVALID_ARGUMENT;
    if (!utf8_valid(bytes, length)) return IRON_ERR_WS_INVALID_UTF8;
    return send_frame(session, 1, bytes, length,
                      Iron_deadline_from_timeout_ms(timeout));
}

int64_t Iron_websocket_send_bytes(Iron_WebSocket socket, Iron_String data,
                                   int64_t timeout) {
    WsSession *session = (WsSession *)(intptr_t)socket.handle;
    if (!session || timeout < 0) return IRON_ERR_WS_INVALID_ARGUMENT;
    return send_frame(session, 2,
        (const uint8_t *)iron_string_cstr(&data), iron_string_byte_len(&data),
        Iron_deadline_from_timeout_ms(timeout));
}

int64_t Iron_websocket_ping(Iron_WebSocket socket, Iron_String data,
                            int64_t timeout) {
    WsSession *session = (WsSession *)(intptr_t)socket.handle;
    size_t length = iron_string_byte_len(&data);
    if (!session || timeout < 0 || length > 125) return IRON_ERR_WS_INVALID_ARGUMENT;
    return send_frame(session, 9,
        (const uint8_t *)iron_string_cstr(&data), length,
        Iron_deadline_from_timeout_ms(timeout));
}

int64_t Iron_websocket_close(Iron_WebSocket socket, int64_t code,
                              Iron_String reason, int64_t timeout) {
    WsSession *session = (WsSession *)(intptr_t)socket.handle;
    const uint8_t *reason_bytes = (const uint8_t *)iron_string_cstr(&reason);
    size_t reason_length = iron_string_byte_len(&reason);
    if (!session || timeout < 0 || !close_code_valid((uint16_t)code) ||
        code < 0 || code > UINT16_MAX || reason_length > 123)
        return IRON_ERR_WS_INVALID_ARGUMENT;
    if (!utf8_valid(reason_bytes, reason_length)) return IRON_ERR_WS_INVALID_UTF8;
    Iron_Deadline deadline = Iron_deadline_from_timeout_ms(timeout);
    int64_t error = 0;
    if (session->open && !session->close_sent) {
        uint8_t payload[125];
        payload[0] = (uint8_t)((uint16_t)code >> 8);
        payload[1] = (uint8_t)code;
        if (reason_length) memcpy(payload + 2, reason_bytes, reason_length);
        error = send_frame(session, 8, payload, reason_length + 2, deadline);
        if (!error) session->close_sent = 1;
    }
    while (!error && session->open && !session->close_received &&
           Iron_deadline_remaining_ms(deadline) > 0) {
        Iron_WebSocketMessage message = Iron_websocket_receive(
            socket, Iron_deadline_remaining_ms(deadline));
        if (message.error != 0) { error = message.error; break; }
        if (message.kind == IRON_WEBSOCKET_CLOSE) break;
    }
    session->open = 0;
    transport_close(session->transport);
    session_free(session);
    return error;
}

void Iron_websocket_abort(Iron_WebSocket socket) {
    WsSession *session = (WsSession *)(intptr_t)socket.handle;
    if (!session) return;
    if (session->open) transport_close(session->transport);
    session->open = 0;
    session_free(session);
}

bool Iron_websocket_is_open(Iron_WebSocket socket) {
    WsSession *session = (WsSession *)(intptr_t)socket.handle;
    return session && session->open;
}
