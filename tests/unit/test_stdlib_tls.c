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
typedef HANDLE TLS_THREAD;
typedef struct ThreadStart { void *(*fn)(void *); void *arg; } ThreadStart;
static DWORD WINAPI thread_trampoline(LPVOID pointer) {
    ThreadStart *start = (ThreadStart *)pointer;
    void *(*fn)(void *) = start->fn;
    void *arg = start->arg;
    free(start);
    (void)fn(arg);
    return 0;
}
static int thread_start(TLS_THREAD *thread, void *(*fn)(void *), void *arg) {
    ThreadStart *start = (ThreadStart *)malloc(sizeof(*start));
    if (!start) return -1;
    start->fn = fn;
    start->arg = arg;
    *thread = CreateThread(NULL, 0, thread_trampoline, start, 0, NULL);
    if (!*thread) { free(start); return -1; }
    return 0;
}
static int thread_join(TLS_THREAD thread) {
    DWORD result = WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    return result == WAIT_OBJECT_0 ? 0 : -1;
}
#else
#include <pthread.h>
typedef pthread_t TLS_THREAD;
static int thread_start(TLS_THREAD *thread, void *(*fn)(void *), void *arg) {
    return pthread_create(thread, NULL, fn, arg);
}
static int thread_join(TLS_THREAD thread) { return pthread_join(thread, NULL); }
#endif

static const char *certificate_path;
static const char *private_key_path;
enum { WSS_WRITERS = 24 };

static Iron_String istr(const char *text) {
    return iron_string_from_cstr(text, strlen(text));
}

void setUp(void) { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

typedef struct TlsServerCase {
    Iron_HttpsServer server;
    int64_t error;
    int expect_http;
    Iron_HttpRequest request;
} TlsServerCase;

typedef struct PendingHandshakeCase {
    Iron_HttpsPendingConnection pending;
    int64_t timeout;
    Iron_HttpsConnectionResult result;
} PendingHandshakeCase;

typedef struct TlsClientCase {
    Iron_String url;
    Iron_HttpResponse response;
} TlsClientCase;

static void *handshake_pending(void *pointer) {
    PendingHandshakeCase *test = (PendingHandshakeCase *)pointer;
    test->result = Iron_httpspendingconnection_handshake(
        test->pending, test->timeout);
    return NULL;
}

static void *request_with_test_ca(void *pointer) {
    TlsClientCase *test = (TlsClientCase *)pointer;
    test->response = Iron_http_get_with_ca(
        test->url, istr(certificate_path), 5000);
    return NULL;
}

static void *serve_tls_once(void *pointer) {
    TlsServerCase *test = (TlsServerCase *)pointer;
    Iron_HttpsConnectionResult accepted = Iron_httpsserver_accept(test->server, 5000);
    if (accepted.error != 0) {
        test->error = accepted.error;
        return NULL;
    }
    if (test->expect_http) {
        Iron_HttpRequest request = Iron_httpsconnection_read_request(
            accepted.connection, 16384, 1024, 5000);
        test->request = request;
        if (request.error != 0) {
            test->error = request.error;
        } else {
            Iron_HttpResponse response = Iron_http_text_response(
                200, istr("secure iron"));
            test->error = Iron_httpsconnection_send_response(
                accepted.connection, response, 5000);
        }
    }
    Iron_httpsconnection_close(accepted.connection);
    return NULL;
}

static void *serve_wss_once(void *pointer) {
    TlsServerCase *test = (TlsServerCase *)pointer;
    Iron_HttpsConnectionResult accepted = Iron_httpsserver_accept(test->server, 5000);
    if (accepted.error != 0) { test->error = accepted.error; return NULL; }
    Iron_HttpRequest request = Iron_httpsconnection_read_request(
        accepted.connection, 16384, 1024, 5000);
    if (request.error != 0) {
        test->error = request.error;
        Iron_httpsconnection_close(accepted.connection);
        return NULL;
    }
    Iron_WebSocketResult upgraded = Iron_httpsconnection_upgrade_websocket(
        accepted.connection, request, 1024, 5000);
    if (upgraded.error != 0) {
        test->error = upgraded.error;
        Iron_httpsconnection_close(accepted.connection);
        return NULL;
    }
    Iron_WebSocketMessage message = Iron_websocket_receive(upgraded.socket, 5000);
    if (message.error != 0 || message.kind != IRON_WEBSOCKET_TEXT) {
        test->error = message.error ? message.error : -1;
    } else {
        test->error = Iron_websocket_send_text(upgraded.socket, message.data, 5000);
    }
    if (!test->error) {
        Iron_WebSocketMessage close = Iron_websocket_receive(upgraded.socket, 5000);
        if (close.error != 0 || close.kind != IRON_WEBSOCKET_CLOSE)
            test->error = close.error ? close.error : -2;
    }
    Iron_websocket_abort(upgraded.socket);
    return NULL;
}

static void *serve_wss_concurrent(void *pointer) {
    TlsServerCase *test = (TlsServerCase *)pointer;
    Iron_HttpsConnectionResult accepted = Iron_httpsserver_accept(test->server, 5000);
    if (accepted.error != 0) { test->error = accepted.error; return NULL; }
    Iron_HttpRequest request = Iron_httpsconnection_read_request(
        accepted.connection, 16384, 1024, 5000);
    if (request.error != 0) {
        test->error = request.error;
        Iron_httpsconnection_close(accepted.connection);
        return NULL;
    }
    Iron_WebSocketResult upgraded = Iron_httpsconnection_upgrade_websocket(
        accepted.connection, request, 1024, 5000);
    if (upgraded.error != 0) {
        test->error = upgraded.error;
        Iron_httpsconnection_close(accepted.connection);
        return NULL;
    }
    for (int i = 0; i < WSS_WRITERS && test->error == 0; i++) {
        Iron_WebSocketMessage message = Iron_websocket_receive(upgraded.socket, 5000);
        if (message.error != 0 || message.kind != IRON_WEBSOCKET_TEXT) {
            test->error = message.error ? message.error : -10;
            break;
        }
        test->error = Iron_websocket_send_text(upgraded.socket, message.data, 5000);
    }
    if (test->error == 0) {
        Iron_WebSocketMessage close = Iron_websocket_receive(upgraded.socket, 5000);
        if (close.error != 0 || close.kind != IRON_WEBSOCKET_CLOSE)
            test->error = close.error ? close.error : -11;
    }
    Iron_websocket_abort(upgraded.socket);
    return NULL;
}

typedef struct WssWriterCase {
    Iron_WebSocket socket;
    Iron_String payload;
    int64_t error;
} WssWriterCase;

typedef struct WssReaderCase {
    Iron_WebSocket socket;
    int64_t error;
} WssReaderCase;

static void *send_wss_message(void *pointer) {
    WssWriterCase *writer = (WssWriterCase *)pointer;
    writer->error = Iron_websocket_send_text(
        writer->socket, writer->payload, 5000);
    return NULL;
}

static void *receive_wss_echoes(void *pointer) {
    WssReaderCase *reader = (WssReaderCase *)pointer;
    for (int i = 0; i < WSS_WRITERS; i++) {
        Iron_WebSocketMessage message = Iron_websocket_receive(reader->socket, 5000);
        if (message.error != 0 || message.kind != IRON_WEBSOCKET_TEXT) {
            reader->error = message.error ? message.error : -20;
            return NULL;
        }
    }
    return NULL;
}

static Iron_HttpsServer make_tls_server(int64_t *port) {
    Iron_HttpsServerResult result = Iron_http_listen_tls(
        istr("127.0.0.1"), 0, istr(certificate_path), istr(private_key_path));
    TEST_ASSERT_EQUAL_INT64(0, result.error);
    *port = Iron_httpsserver_port(result.server);
    TEST_ASSERT_GREATER_THAN_INT64(0, *port);
    return result.server;
}

static void start_case(TlsServerCase *test, TLS_THREAD *thread, int expect_http,
                       int64_t *port) {
    memset(test, 0, sizeof(*test));
    test->server = make_tls_server(port);
    test->expect_http = expect_http;
    TEST_ASSERT_EQUAL_INT(0, thread_start(thread, serve_tls_once, test));
}

void test_https_verified_custom_ca_roundtrip(void) {
    int64_t port = 0;
    TlsServerCase server;
    TLS_THREAD thread;
    start_case(&server, &thread, 1, &port);
    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%lld/secure", (long long)port);
    Iron_HttpResponse response = Iron_http_request_with_ca(
        istr("POST"), istr(url), istr("Content-Type: application/json"),
        istr("{\"secure\":true}"), 1024, istr(certificate_path), 5000);
    TEST_ASSERT_EQUAL_INT64(0, response.error);
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_STRING_LEN("secure iron", iron_string_cstr(&response.body),
                                 strlen("secure iron"));
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    TEST_ASSERT_EQUAL_STRING("POST", iron_string_cstr(&server.request.method));
    TEST_ASSERT_EQUAL_STRING("{\"secure\":true}",
                             iron_string_cstr(&server.request.body));
    Iron_httpsserver_close(server.server);
}

void test_https_default_rejects_untrusted_certificate(void) {
    int64_t port = 0;
    TlsServerCase server;
    TLS_THREAD thread;
    start_case(&server, &thread, 0, &port);
    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%lld/", (long long)port);
    Iron_HttpResponse response = Iron_http_get(istr(url), 5000);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_TLS_VERIFY, response.error);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_TRUE(server.error == IRON_ERR_TLS_HANDSHAKE ||
                     server.error == IRON_ERR_TLS_CLOSED ||
                     server.error == IRON_ERR_TLS_IO);
    Iron_httpsserver_close(server.server);
}

void test_https_rejects_hostname_mismatch(void) {
    int64_t port = 0;
    TlsServerCase server;
    TLS_THREAD thread;
    start_case(&server, &thread, 0, &port);
    char url[128];
    snprintf(url, sizeof(url), "https://127.0.0.1:%lld/", (long long)port);
    Iron_HttpResponse response = Iron_http_get_with_ca(
        istr(url), istr(certificate_path), 5000);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_TLS_VERIFY, response.error);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    Iron_httpsserver_close(server.server);
}

void test_https_explicit_insecure_mode_roundtrip(void) {
    int64_t port = 0;
    TlsServerCase server;
    TLS_THREAD thread;
    start_case(&server, &thread, 1, &port);
    char url[128];
    snprintf(url, sizeof(url), "https://127.0.0.1:%lld/dev", (long long)port);
    Iron_HttpResponse response = Iron_http_get_insecure(istr(url), 5000);
    TEST_ASSERT_EQUAL_INT64(0, response.error);
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    Iron_httpsserver_close(server.server);
}

void test_https_server_rejects_missing_certificate(void) {
    Iron_HttpsServerResult result = Iron_http_listen_tls(
        istr("127.0.0.1"), 0, istr("/definitely/missing/cert.pem"),
        istr("/definitely/missing/key.pem"));
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_TLS_CERTIFICATE, result.error);
    TEST_ASSERT_EQUAL_INT64(-1, result.server.fd);
}

void test_https_stalled_tcp_does_not_block_next_client(void) {
    int64_t port = 0;
    Iron_HttpsServer server = make_tls_server(&port);

    Iron_Result_TcpSocket_Error raw = Iron_net_tcp_dial(
        istr("127.0.0.1"), port, 1000);
    TEST_ASSERT_EQUAL_INT64(0, raw.v1.code);
    Iron_HttpsPendingConnectionResult stalled =
        Iron_httpsserver_accept_tcp(server, 1000);
    TEST_ASSERT_EQUAL_INT64(0, stalled.error);

    PendingHandshakeCase slow = { stalled.connection, 3000, {0} };
    TLS_THREAD slow_thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(
        &slow_thread, handshake_pending, &slow));

    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%lld/admitted",
             (long long)port);
    TlsClientCase client = { istr(url), {0} };
    TLS_THREAD client_thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(
        &client_thread, request_with_test_ca, &client));

    Iron_HttpsPendingConnectionResult admitted =
        Iron_httpsserver_accept_tcp(server, 1000);
    TEST_ASSERT_EQUAL_INT64(0, admitted.error);

    /* Pending connections retain the certificate context, so listener
     * shutdown cannot invalidate either in-flight handshake. */
    Iron_httpsserver_close(server);
    Iron_HttpsConnectionResult connection =
        Iron_httpspendingconnection_handshake(admitted.connection, 3000);
    TEST_ASSERT_EQUAL_INT64(0, connection.error);
    Iron_HttpRequest request = Iron_httpsconnection_read_request(
        connection.connection, 16384, 1024, 3000);
    TEST_ASSERT_EQUAL_INT64(0, request.error);
    TEST_ASSERT_EQUAL_STRING("/admitted", iron_string_cstr(&request.path));
    TEST_ASSERT_EQUAL_INT64(0, Iron_httpsconnection_send_response(
        connection.connection, Iron_http_text_response(200, istr("admitted")),
        3000));
    Iron_httpsconnection_close(connection.connection);

    TEST_ASSERT_EQUAL_INT(0, thread_join(client_thread));
    TEST_ASSERT_EQUAL_INT64(0, client.response.error);
    TEST_ASSERT_EQUAL_INT64(200, client.response.status);
    TEST_ASSERT_EQUAL_STRING("admitted",
                             iron_string_cstr(&client.response.body));

    Iron_tcpsocket_close(raw.v0);
    TEST_ASSERT_EQUAL_INT(0, thread_join(slow_thread));
    TEST_ASSERT_NOT_EQUAL(0, slow.result.error);
}

void test_wss_verified_upgrade_and_message_roundtrip(void) {
    int64_t port = 0;
    TlsServerCase server;
    memset(&server, 0, sizeof(server));
    server.server = make_tls_server(&port);
    TLS_THREAD thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&thread, serve_wss_once, &server));
    char url[128];
    snprintf(url, sizeof(url), "wss://localhost:%lld/events", (long long)port);
    Iron_WebSocketResult connected = Iron_websocket_connect_with_ca(
        istr(url), istr(""), istr(certificate_path), 1024, 5000);
    TEST_ASSERT_EQUAL_INT64(0, connected.error);
    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_send_text(
        connected.socket, istr("secure websocket"), 5000));
    Iron_WebSocketMessage echoed = Iron_websocket_receive(connected.socket, 5000);
    TEST_ASSERT_EQUAL_INT64(0, echoed.error);
    TEST_ASSERT_EQUAL_INT64(IRON_WEBSOCKET_TEXT, echoed.kind);
    TEST_ASSERT_EQUAL_STRING_LEN("secure websocket",
        iron_string_cstr(&echoed.data), strlen("secure websocket"));
    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_close(
        connected.socket, 1000, istr("done"), 5000));
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    Iron_httpsserver_close(server.server);
}

void test_wss_concurrent_reader_and_writers(void) {
    int64_t port = 0;
    TlsServerCase server;
    memset(&server, 0, sizeof(server));
    server.server = make_tls_server(&port);
    TLS_THREAD server_thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(
        &server_thread, serve_wss_concurrent, &server));
    char url[128];
    snprintf(url, sizeof(url), "wss://localhost:%lld/concurrent",
             (long long)port);
    Iron_WebSocketResult connected = Iron_websocket_connect_with_ca(
        istr(url), istr(""), istr(certificate_path), 1024, 5000);
    TEST_ASSERT_EQUAL_INT64(0, connected.error);

    WssReaderCase reader = { connected.socket, 0 };
    TLS_THREAD reader_thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(
        &reader_thread, receive_wss_echoes, &reader));
    TLS_THREAD writers[WSS_WRITERS];
    WssWriterCase writer_cases[WSS_WRITERS];
    char payloads[WSS_WRITERS][32];
    for (int i = 0; i < WSS_WRITERS; i++) {
        snprintf(payloads[i], sizeof(payloads[i]), "secure-writer-%d", i);
        writer_cases[i].socket = connected.socket;
        writer_cases[i].payload = istr(payloads[i]);
        writer_cases[i].error = 0;
        TEST_ASSERT_EQUAL_INT(0, thread_start(
            &writers[i], send_wss_message, &writer_cases[i]));
    }
    for (int i = 0; i < WSS_WRITERS; i++) {
        TEST_ASSERT_EQUAL_INT(0, thread_join(writers[i]));
        TEST_ASSERT_EQUAL_INT64(0, writer_cases[i].error);
    }
    TEST_ASSERT_EQUAL_INT(0, thread_join(reader_thread));
    TEST_ASSERT_EQUAL_INT64(0, reader.error);
    TEST_ASSERT_EQUAL_INT64(0, Iron_websocket_close(
        connected.socket, 1000, istr("concurrency done"), 5000));
    TEST_ASSERT_EQUAL_INT(0, thread_join(server_thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    Iron_httpsserver_close(server.server);
}

int main(void) {
    certificate_path = getenv("IRON_TEST_TLS_CERT");
    private_key_path = getenv("IRON_TEST_TLS_KEY");
    if (!certificate_path || !private_key_path) {
        fprintf(stderr, "IRON_TEST_TLS_CERT and IRON_TEST_TLS_KEY are required\n");
        return 2;
    }
    UNITY_BEGIN();
    RUN_TEST(test_https_verified_custom_ca_roundtrip);
    RUN_TEST(test_https_default_rejects_untrusted_certificate);
    RUN_TEST(test_https_rejects_hostname_mismatch);
    RUN_TEST(test_https_explicit_insecure_mode_roundtrip);
    RUN_TEST(test_https_server_rejects_missing_certificate);
    RUN_TEST(test_https_stalled_tcp_does_not_block_next_client);
    RUN_TEST(test_wss_verified_upgrade_and_message_roundtrip);
    RUN_TEST(test_wss_concurrent_reader_and_writers);
    return UNITY_END();
}
