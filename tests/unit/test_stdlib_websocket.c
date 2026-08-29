#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_errors.h"
#include "stdlib/iron_http.h"
#include "stdlib/iron_net.h"
#include "stdlib/iron_websocket.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE WS_THREAD;
typedef struct ThreadStart { void *(*fn)(void *); void *arg; } ThreadStart;
static DWORD WINAPI thread_trampoline(LPVOID pointer) {
    ThreadStart *start = (ThreadStart *)pointer;
    void *(*fn)(void *) = start->fn;
    void *arg = start->arg;
    free(start);
    (void)fn(arg);
    return 0;
}
static int thread_start(WS_THREAD *thread, void *(*fn)(void *), void *arg) {
    ThreadStart *start = (ThreadStart *)malloc(sizeof(*start));
    if (!start) return -1;
    start->fn = fn; start->arg = arg;
    *thread = CreateThread(NULL, 0, thread_trampoline, start, 0, NULL);
    if (!*thread) { free(start); return -1; }
    return 0;
}
static int thread_join(WS_THREAD thread) {
    DWORD result = WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    return result == WAIT_OBJECT_0 ? 0 : -1;
}
#else
#include <pthread.h>
typedef pthread_t WS_THREAD;
static int thread_start(WS_THREAD *thread, void *(*fn)(void *), void *arg) {
    return pthread_create(thread, NULL, fn, arg);
}
static int thread_join(WS_THREAD thread) { return pthread_join(thread, NULL); }
#endif

static Iron_String istrn(const void *bytes, size_t length) {
    return iron_string_from_cstr((const char *)bytes, length);
}
static Iron_String istr(const char *text) {
    return iron_string_from_literal(text, strlen(text));
}

void setUp(void) { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

static void assert_bytes(const void *expected, size_t expected_length,
                         Iron_String actual) {
    TEST_ASSERT_EQUAL_size_t(expected_length, iron_string_byte_len(&actual));
    TEST_ASSERT_EQUAL_MEMORY(expected, iron_string_cstr(&actual), expected_length);
}

typedef struct WsServerCase {
    Iron_HttpServer server;
    int mode;
    int64_t error;
    Iron_String text;
    Iron_String binary;
    Iron_String protocol;
} WsServerCase;

enum { WS_WRITERS = 24 };

static int server_upgrade(WsServerCase *test, Iron_WebSocket *socket,
                          int64_t max_message) {
    Iron_HttpConnectionResult accepted = Iron_httpserver_accept(test->server, 5000);
    if (accepted.error != 0) {
        int error = (int)accepted.error;
        Iron_httpconnectionresult_release(accepted);
        return error;
    }
    Iron_HttpRequest request = Iron_httpconnection_read_request(
        accepted.connection, 16384, 1024, 5000);
    if (request.error != 0) {
        int error = (int)request.error;
        Iron_httprequest_release(request);
        Iron_httpconnection_close(accepted.connection);
        Iron_httpconnectionresult_release(accepted);
        return error;
    }
    Iron_WebSocketResult upgraded = test->mode == 7 || test->mode == 8
        ? Iron_httpconnection_upgrade_websocket_protocol(
            accepted.connection, request,
            istr(test->mode == 7 ? "graphql-transport-ws" : "mqtt"),
            max_message, 5000)
        : Iron_httpconnection_upgrade_websocket(
            accepted.connection, request, max_message, 5000);
    if (upgraded.error != 0) {
        int error = (int)upgraded.error;
        Iron_websocketresult_release(upgraded);
        Iron_httprequest_release(request);
        Iron_httpconnection_close(accepted.connection);
        Iron_httpconnectionresult_release(accepted);
        return error;
    }
    *socket = upgraded.socket;
    test->protocol = iron_string_from_cstr(
        iron_string_cstr(&upgraded.protocol),
        iron_string_byte_len(&upgraded.protocol));
    Iron_websocketresult_release(upgraded);
    Iron_httprequest_release(request);
    Iron_httpconnectionresult_release(accepted);
    return 0;
}

static void release_server_case(WsServerCase *test) {
    iron_string_release(&test->text);
    iron_string_release(&test->binary);
    iron_string_release(&test->protocol);
}

static void *serve_websocket(void *pointer) {
    WsServerCase *test = (WsServerCase *)pointer;
    Iron_WebSocket socket;
    int64_t max_message = test->mode == 3 ? 5 : (test->mode == 5 ? 70000 : 1024);
    int error = server_upgrade(test, &socket, max_message);
    if (error) { test->error = error; return NULL; }
    if (test->mode == 0) {
        Iron_WebSocketMessage text = Iron_websocket_receive(socket, 5000);
        if (text.error || text.kind != IRON_WEBSOCKET_TEXT) {
            test->error = text.error ? text.error : -1;
            Iron_websocketmessage_release(text);
            Iron_websocket_abort(socket);
            return NULL;
        }
        test->text = iron_string_from_cstr(
            iron_string_cstr(&text.data), iron_string_byte_len(&text.data));
        test->error = Iron_websocket_send_text(socket, text.data, 5000);
        Iron_websocketmessage_release(text);
        if (test->error) { Iron_websocket_abort(socket); return NULL; }

        Iron_WebSocketMessage binary = Iron_websocket_receive(socket, 5000);
        if (binary.error || binary.kind != IRON_WEBSOCKET_BINARY) {
            test->error = binary.error ? binary.error : -2;
            Iron_websocketmessage_release(binary);
            Iron_websocket_abort(socket);
            return NULL;
        }
        test->binary = iron_string_from_cstr(
            iron_string_cstr(&binary.data), iron_string_byte_len(&binary.data));
        test->error = Iron_websocket_send_bytes(socket, binary.data, 5000);
        Iron_websocketmessage_release(binary);
        if (test->error) { Iron_websocket_abort(socket); return NULL; }

        Iron_WebSocketMessage ping = Iron_websocket_receive(socket, 5000);
        if (ping.error || ping.kind != IRON_WEBSOCKET_PING) {
            test->error = ping.error ? ping.error : -3;
            Iron_websocketmessage_release(ping);
            Iron_websocket_abort(socket);
            return NULL;
        }
        Iron_websocketmessage_release(ping);
        Iron_WebSocketMessage close = Iron_websocket_receive(socket, 5000);
        if (close.error || close.kind != IRON_WEBSOCKET_CLOSE ||
            close.close_code != 1000) {
            test->error = close.error ? close.error : -4;
        }
        Iron_websocketmessage_release(close);
        Iron_websocket_abort(socket);
        return NULL;
    }
    if (test->mode == 1) {
        Iron_WebSocketMessage ping = Iron_websocket_receive(socket, 5000);
        if (ping.error || ping.kind != IRON_WEBSOCKET_PING)
            test->error = ping.error ? ping.error : -10;
        Iron_websocketmessage_release(ping);
        if (!test->error) {
            Iron_WebSocketMessage text = Iron_websocket_receive(socket, 5000);
            if (text.error || text.kind != IRON_WEBSOCKET_TEXT)
                test->error = text.error ? text.error : -11;
            else test->text = iron_string_from_cstr(
                iron_string_cstr(&text.data), iron_string_byte_len(&text.data));
            Iron_websocketmessage_release(text);
        }
    } else if (test->mode == 4) {
        for (int i = 0; i < WS_WRITERS; i++) {
            Iron_WebSocketMessage message = Iron_websocket_receive(socket, 5000);
            if (message.error != 0 || message.kind != IRON_WEBSOCKET_TEXT) {
                test->error = message.error ? message.error : -20;
                Iron_websocketmessage_release(message);
                break;
            }
            Iron_websocketmessage_release(message);
        }
        if (!test->error) {
            Iron_WebSocketMessage close = Iron_websocket_receive(socket, 5000);
            if (close.error != 0 || close.kind != IRON_WEBSOCKET_CLOSE)
                test->error = close.error ? close.error : -21;
            Iron_websocketmessage_release(close);
        }
    } else if (test->mode == 5) {
        Iron_WebSocketMessage small = Iron_websocket_receive(socket, 5000);
        if (small.error != 0 || small.kind != IRON_WEBSOCKET_BINARY) {
            test->error = small.error ? small.error : -29;
        } else {
            test->error = Iron_websocket_send_bytes(socket, small.data, 5000);
        }
        Iron_websocketmessage_release(small);
        Iron_WebSocketMessage message = Iron_websocket_receive(socket, 5000);
        if (message.error != 0 || message.kind != IRON_WEBSOCKET_BINARY) {
            test->error = message.error ? message.error : -30;
        } else {
            test->binary = iron_string_from_cstr(
                iron_string_cstr(&message.data),
                iron_string_byte_len(&message.data));
            test->error = Iron_websocket_send_bytes(socket, message.data, 5000);
        }
        Iron_websocketmessage_release(message);
        if (!test->error) {
            Iron_WebSocketMessage close = Iron_websocket_receive(socket, 5000);
            if (close.error != 0 || close.kind != IRON_WEBSOCKET_CLOSE)
                test->error = close.error ? close.error : -31;
            Iron_websocketmessage_release(close);
        }
    } else if (test->mode == 7) {
        Iron_WebSocketMessage close = Iron_websocket_receive(socket, 5000);
        if (close.error != 0 || close.kind != IRON_WEBSOCKET_CLOSE)
            test->error = close.error ? close.error : -40;
        Iron_websocketmessage_release(close);
    } else {
        Iron_WebSocketMessage message = Iron_websocket_receive(socket, 5000);
        test->error = message.error;
        Iron_websocketmessage_release(message);
    }
    Iron_websocket_abort(socket);
    return NULL;
}

typedef struct WriterCase {
    Iron_WebSocket socket;
    int index;
    int64_t error;
} WriterCase;

static void *send_from_writer(void *pointer) {
    WriterCase *writer = (WriterCase *)pointer;
    char payload[64];
    snprintf(payload, sizeof(payload), "writer-%d", writer->index);
    writer->error = Iron_websocket_send_text(writer->socket, istr(payload), 5000);
    return NULL;
}

static Iron_HttpServer make_server(int64_t *port) {
    Iron_HttpServerResult result = Iron_http_listen(istr("127.0.0.1"), 0);
    TEST_ASSERT_EQUAL_INT64(0, result.error);
    *port = Iron_httpserver_port(result.server);
    TEST_ASSERT_GREATER_THAN_INT64(0, *port);
    Iron_HttpServer server = result.server;
    Iron_httpserverresult_release(result);
    return server;
}

static void start_server_case(WsServerCase *test, WS_THREAD *thread,
                              int mode, int64_t *port) {
    memset(test, 0, sizeof(*test));
    test->server = make_server(port);
    test->mode = mode;
    TEST_ASSERT_EQUAL_INT(0, thread_start(thread, serve_websocket, test));
}

void test_websocket_client_server_text_binary_ping_close(void) {
    int64_t port = 0;
    WsServerCase server;
    WS_THREAD thread;
    start_server_case(&server, &thread, 0, &port);
    char url[128];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%lld/chat?room=iron", (long long)port);
    Iron_WebSocketResult connected = Iron_websocket_connect(
        istr(url), istr("Authorization: Bearer local-test"), 1024, 5000);
    TEST_ASSERT_EQUAL_INT64(0, connected.error);
    TEST_ASSERT_TRUE(Iron_websocket_is_open(connected.socket));
    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_send_text(
        connected.socket, istr("hello websocket"), 5000));
    Iron_WebSocketMessage text = Iron_websocket_receive(connected.socket, 5000);
    TEST_ASSERT_EQUAL_INT64(0, text.error);
    TEST_ASSERT_EQUAL_INT64(IRON_WEBSOCKET_TEXT, text.kind);
    assert_bytes("hello websocket", 15, text.data);

    const uint8_t binary_bytes[] = { 0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff };
    Iron_String outbound_binary = istrn(binary_bytes, sizeof(binary_bytes));
    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_send_bytes(
        connected.socket, outbound_binary, 5000));
    iron_string_release(&outbound_binary);
    Iron_WebSocketMessage binary = Iron_websocket_receive(connected.socket, 5000);
    TEST_ASSERT_EQUAL_INT64(0, binary.error);
    TEST_ASSERT_EQUAL_INT64(IRON_WEBSOCKET_BINARY, binary.kind);
    assert_bytes(binary_bytes, sizeof(binary_bytes), binary.data);

    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_ping(
        connected.socket, istr("health"), 5000));
    Iron_WebSocketMessage pong = Iron_websocket_receive(connected.socket, 5000);
    TEST_ASSERT_EQUAL_INT64(0, pong.error);
    TEST_ASSERT_EQUAL_INT64(IRON_WEBSOCKET_PONG, pong.kind);
    assert_bytes("health", 6, pong.data);
    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_close(
        connected.socket, 1000, istr("done"), 5000));

    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    assert_bytes("hello websocket", 15, server.text);
    assert_bytes(binary_bytes, sizeof(binary_bytes), server.binary);
    Iron_websocketmessage_release(text);
    Iron_websocketmessage_release(binary);
    Iron_websocketmessage_release(pong);
    Iron_websocketresult_release(connected);
    release_server_case(&server);
    Iron_httpserver_close(server.server);
}

static Iron_TcpSocket raw_send_upgrade(int64_t port, const char *key) {
    Iron_Result_TcpSocket_Error dial = Iron_net_tcp_dial(
        istr("127.0.0.1"), port, 5000);
    TEST_ASSERT_EQUAL_INT64(0, dial.v1.code);
    char request[512];
    int request_length = snprintf(request, sizeof(request),
        "GET /raw HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", key);
    TEST_ASSERT_GREATER_THAN_INT(0, request_length);
    TEST_ASSERT_LESS_THAN_INT((int)sizeof(request), request_length);
    Iron_Result_Int_Error sent = Iron_net_tcp_send_bytes(
        dial.v0, (const uint8_t *)request, request_length, 5000);
    TEST_ASSERT_EQUAL_INT64(0, sent.v1.code);
    return dial.v0;
}

static Iron_TcpSocket raw_upgrade(int64_t port) {
    Iron_TcpSocket socket = raw_send_upgrade(
        port, "dGhlIHNhbXBsZSBub25jZQ==");
    char response[512];
    size_t used = 0;
    while (used + 1 < sizeof(response)) {
        Iron_Result_Int_Error read = Iron_net_tcp_recv_bytes(
            socket, (uint8_t *)response + used,
            (int64_t)(sizeof(response) - used - 1), 5000);
        TEST_ASSERT_EQUAL_INT64(0, read.v1.code);
        TEST_ASSERT_GREATER_THAN_INT64(0, read.v0);
        used += (size_t)read.v0;
        response[used] = '\0';
        if (strstr(response, "\r\n\r\n")) break;
    }
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 101"));
    TEST_ASSERT_NOT_NULL(strstr(response,
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    return socket;
}

static void raw_send_masked(Iron_TcpSocket socket, uint8_t first,
                            const uint8_t *payload, size_t length,
                            const uint8_t mask[4]) {
    TEST_ASSERT_LESS_OR_EQUAL_size_t(125, length);
    uint8_t frame[2 + 4 + 125];
    frame[0] = first;
    frame[1] = (uint8_t)(0x80 | length);
    memcpy(frame + 2, mask, 4);
    for (size_t i = 0; i < length; i++) frame[6 + i] = payload[i] ^ mask[i & 3];
    Iron_Result_Int_Error sent = Iron_net_tcp_send_bytes(
        socket, frame, (int64_t)(6 + length), 5000);
    TEST_ASSERT_EQUAL_INT64(0, sent.v1.code);
    TEST_ASSERT_EQUAL_INT64((int64_t)(6 + length), sent.v0);
}

static void raw_read_exact(Iron_TcpSocket socket, uint8_t *bytes, size_t length) {
    size_t used = 0;
    while (used < length) {
        Iron_Result_Int_Error read = Iron_net_tcp_recv_bytes(
            socket, bytes + used, (int64_t)(length - used), 5000);
        TEST_ASSERT_EQUAL_INT64(0, read.v1.code);
        TEST_ASSERT_GREATER_THAN_INT64(0, read.v0);
        used += (size_t)read.v0;
    }
}

void test_websocket_fragmentation_with_interleaved_ping(void) {
    int64_t port = 0;
    WsServerCase server;
    WS_THREAD thread;
    start_server_case(&server, &thread, 1, &port);
    Iron_TcpSocket raw = raw_upgrade(port);
    const uint8_t mask1[4] = { 1, 2, 3, 4 };
    const uint8_t mask2[4] = { 5, 6, 7, 8 };
    const uint8_t mask3[4] = { 9, 10, 11, 12 };
    raw_send_masked(raw, 0x01, (const uint8_t *)"Hel", 3, mask1);
    raw_send_masked(raw, 0x89, (const uint8_t *)"hi", 2, mask2);
    uint8_t pong[4];
    raw_read_exact(raw, pong, sizeof(pong));
    TEST_ASSERT_EQUAL_HEX8(0x8a, pong[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, pong[1]);
    TEST_ASSERT_EQUAL_MEMORY("hi", pong + 2, 2);
    raw_send_masked(raw, 0x80, (const uint8_t *)"lo", 2, mask3);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    assert_bytes("Hello", 5, server.text);
    release_server_case(&server);
    Iron_tcpsocket_close(raw);
    Iron_httpserver_close(server.server);
}

void test_websocket_rejects_unmasked_client_frame(void) {
    int64_t port = 0;
    WsServerCase server;
    WS_THREAD thread;
    start_server_case(&server, &thread, 2, &port);
    Iron_TcpSocket raw = raw_upgrade(port);
    const uint8_t frame[] = { 0x81, 0x01, 'x' };
    Iron_Result_Int_Error sent = Iron_net_tcp_send_bytes(
        raw, frame, sizeof(frame), 5000);
    TEST_ASSERT_EQUAL_INT64(0, sent.v1.code);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_WS_PROTOCOL, server.error);
    release_server_case(&server);
    Iron_tcpsocket_close(raw);
    Iron_httpserver_close(server.server);
}

void test_websocket_enforces_message_limit(void) {
    int64_t port = 0;
    WsServerCase server;
    WS_THREAD thread;
    start_server_case(&server, &thread, 3, &port);
    Iron_TcpSocket raw = raw_upgrade(port);
    const uint8_t mask[4] = { 12, 34, 56, 78 };
    raw_send_masked(raw, 0x82, (const uint8_t *)"123456", 6, mask);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_WS_MESSAGE_TOO_LARGE, server.error);
    release_server_case(&server);
    Iron_tcpsocket_close(raw);
    Iron_httpserver_close(server.server);
}

void test_websocket_rejects_invalid_utf8_text(void) {
    int64_t port = 0;
    WsServerCase server;
    WS_THREAD thread;
    start_server_case(&server, &thread, 6, &port);
    Iron_TcpSocket raw = raw_upgrade(port);
    const uint8_t invalid[] = { 0xc0, 0xaf };
    const uint8_t mask[4] = { 99, 7, 41, 3 };
    raw_send_masked(raw, 0x81, invalid, sizeof(invalid), mask);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_WS_INVALID_UTF8, server.error);
    release_server_case(&server);
    Iron_tcpsocket_close(raw);
    Iron_httpserver_close(server.server);
}

void test_websocket_rejects_handshake_header_override(void) {
    Iron_WebSocketResult result = Iron_websocket_connect(
        istr("ws://127.0.0.1:1/"), istr("Sec-WebSocket-Key: attacker"),
        1024, 100);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_WS_INVALID_ARGUMENT, result.error);
    Iron_websocketresult_release(result);
}

void test_websocket_negotiates_ordered_subprotocols(void) {
    int64_t port = 0;
    WsServerCase server;
    WS_THREAD thread;
    start_server_case(&server, &thread, 7, &port);
    char url[128];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%lld/graphql",
             (long long)port);
    Iron_String requested_items[] = {
        istr("graphql-transport-ws"), istr("graphql-ws")
    };
    Iron_List_Iron_String requested = { requested_items, 2, 2 };
    Iron_WebSocketResult connected = Iron_websocket_connect_with_protocols(
        istr(url), istr(""), requested, 1024, 5000);
    TEST_ASSERT_EQUAL_INT64(0, connected.error);
    TEST_ASSERT_EQUAL_STRING("graphql-transport-ws",
                             iron_string_cstr(&connected.protocol));
    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_close(
        connected.socket, 1000, istr("negotiated"), 5000));
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    TEST_ASSERT_EQUAL_STRING("graphql-transport-ws",
                             iron_string_cstr(&server.protocol));
    Iron_websocketresult_release(connected);
    release_server_case(&server);
    Iron_httpserver_close(server.server);
}

void test_websocket_rejects_unoffered_server_protocol(void) {
    int64_t port = 0;
    WsServerCase server;
    WS_THREAD thread;
    start_server_case(&server, &thread, 8, &port);
    char url[128];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%lld/protocol",
             (long long)port);
    Iron_String requested_items[] = { istr("graphql-transport-ws") };
    Iron_List_Iron_String requested = { requested_items, 1, 1 };
    Iron_WebSocketResult connected = Iron_websocket_connect_with_protocols(
        istr(url), istr(""), requested, 1024, 5000);
    TEST_ASSERT_NOT_EQUAL(0, connected.error);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_WS_HANDSHAKE, server.error);
    Iron_websocketresult_release(connected);
    release_server_case(&server);
    Iron_httpserver_close(server.server);
}

void test_websocket_rejects_invalid_or_duplicate_protocols(void) {
    Iron_String invalid_items[] = { istr("graphql transport") };
    Iron_List_Iron_String invalid = { invalid_items, 1, 1 };
    Iron_WebSocketResult bad = Iron_websocket_connect_with_protocols(
        istr("ws://127.0.0.1:1/"), istr(""), invalid, 1024, 100);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_WS_INVALID_ARGUMENT, bad.error);
    Iron_websocketresult_release(bad);

    Iron_String duplicate_items[] = { istr("mqtt"), istr("mqtt") };
    Iron_List_Iron_String duplicate = { duplicate_items, 2, 2 };
    bad = Iron_websocket_connect_with_protocols(
        istr("ws://127.0.0.1:1/"), istr(""), duplicate, 1024, 100);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_WS_INVALID_ARGUMENT, bad.error);
    Iron_websocketresult_release(bad);
}

void test_websocket_rejects_noncanonical_client_key(void) {
    int64_t port = 0;
    WsServerCase server;
    WS_THREAD thread;
    start_server_case(&server, &thread, 2, &port);
    Iron_TcpSocket raw = raw_send_upgrade(
        port, "dGhlIHNhbXBsZSBub25jZR==");
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_WS_HANDSHAKE, server.error);
    release_server_case(&server);
    Iron_tcpsocket_close(raw);
    Iron_httpserver_close(server.server);
}

void test_websocket_serializes_concurrent_writers(void) {
    int64_t port = 0;
    WsServerCase server;
    WS_THREAD server_thread;
    start_server_case(&server, &server_thread, 4, &port);
    char url[128];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%lld/concurrent", (long long)port);
    Iron_WebSocketResult connected = Iron_websocket_connect(
        istr(url), istr(""), 1024, 5000);
    TEST_ASSERT_EQUAL_INT64(0, connected.error);
    WS_THREAD threads[WS_WRITERS];
    WriterCase writers[WS_WRITERS];
    for (int i = 0; i < WS_WRITERS; i++) {
        writers[i].socket = connected.socket;
        writers[i].index = i;
        writers[i].error = 0;
        TEST_ASSERT_EQUAL_INT(0, thread_start(&threads[i], send_from_writer, &writers[i]));
    }
    for (int i = 0; i < WS_WRITERS; i++) {
        TEST_ASSERT_EQUAL_INT(0, thread_join(threads[i]));
        TEST_ASSERT_EQUAL_INT64(0, writers[i].error);
    }
    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_close(
        connected.socket, 1000, istr("writers done"), 5000));
    TEST_ASSERT_EQUAL_INT(0, thread_join(server_thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    Iron_websocketresult_release(connected);
    release_server_case(&server);
    Iron_httpserver_close(server.server);
}

void test_websocket_64_bit_length_binary_roundtrip(void) {
    const size_t payload_length = 65536;
    uint8_t *payload = (uint8_t *)malloc(payload_length);
    TEST_ASSERT_NOT_NULL(payload);
    for (size_t i = 0; i < payload_length; i++) payload[i] = (uint8_t)(i * 31u);
    int64_t port = 0;
    WsServerCase server;
    WS_THREAD thread;
    start_server_case(&server, &thread, 5, &port);
    char url[128];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%lld/large", (long long)port);
    Iron_WebSocketResult connected = Iron_websocket_connect(
        istr(url), istr(""), 70000, 5000);
    TEST_ASSERT_EQUAL_INT64(0, connected.error);
    uint8_t medium[126];
    for (size_t i = 0; i < sizeof(medium); i++) medium[i] = (uint8_t)i;
    Iron_String medium_out = istrn(medium, sizeof(medium));
    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_send_bytes(
        connected.socket, medium_out, 5000));
    iron_string_release(&medium_out);
    Iron_WebSocketMessage medium_echo = Iron_websocket_receive(connected.socket, 5000);
    TEST_ASSERT_EQUAL_INT64(0, medium_echo.error);
    assert_bytes(medium, sizeof(medium), medium_echo.data);
    Iron_String payload_out = istrn(payload, payload_length);
    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_send_bytes(
        connected.socket, payload_out, 5000));
    iron_string_release(&payload_out);
    Iron_WebSocketMessage echoed = Iron_websocket_receive(connected.socket, 5000);
    TEST_ASSERT_EQUAL_INT64(0, echoed.error);
    TEST_ASSERT_EQUAL_INT64(IRON_WEBSOCKET_BINARY, echoed.kind);
    assert_bytes(payload, payload_length, echoed.data);
    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_close(
        connected.socket, 1000, istr("large done"), 5000));
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    assert_bytes(payload, payload_length, server.binary);
    Iron_websocketmessage_release(medium_echo);
    Iron_websocketmessage_release(echoed);
    Iron_websocketresult_release(connected);
    release_server_case(&server);
    Iron_httpserver_close(server.server);
    free(payload);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_websocket_client_server_text_binary_ping_close);
    RUN_TEST(test_websocket_fragmentation_with_interleaved_ping);
    RUN_TEST(test_websocket_rejects_unmasked_client_frame);
    RUN_TEST(test_websocket_enforces_message_limit);
    RUN_TEST(test_websocket_rejects_invalid_utf8_text);
    RUN_TEST(test_websocket_rejects_handshake_header_override);
    RUN_TEST(test_websocket_negotiates_ordered_subprotocols);
    RUN_TEST(test_websocket_rejects_unoffered_server_protocol);
    RUN_TEST(test_websocket_rejects_invalid_or_duplicate_protocols);
    RUN_TEST(test_websocket_rejects_noncanonical_client_key);
    RUN_TEST(test_websocket_serializes_concurrent_writers);
    RUN_TEST(test_websocket_64_bit_length_binary_roundtrip);
    return UNITY_END();
}
