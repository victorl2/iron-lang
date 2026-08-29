#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_errors.h"
#include "stdlib/iron_http.h"
#include "stdlib/iron_net.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE HTTP_THREAD;
#else
  #include <pthread.h>
  typedef pthread_t HTTP_THREAD;
#endif

typedef void *(*http_thread_fn)(void *);

#ifdef _WIN32
typedef struct ThreadStart { http_thread_fn fn; void *arg; } ThreadStart;
static DWORD WINAPI thread_trampoline(LPVOID p) {
    ThreadStart *start = (ThreadStart *)p;
    http_thread_fn fn = start->fn;
    void *arg = start->arg;
    free(start);
    (void)fn(arg);
    return 0;
}
static int thread_start(HTTP_THREAD *thread, http_thread_fn fn, void *arg) {
    ThreadStart *start = (ThreadStart *)malloc(sizeof(*start));
    if (!start) return -1;
    start->fn = fn;
    start->arg = arg;
    *thread = CreateThread(NULL, 0, thread_trampoline, start, 0, NULL);
    if (!*thread) { free(start); return -1; }
    return 0;
}
static int thread_join(HTTP_THREAD thread) {
    DWORD result = WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    return result == WAIT_OBJECT_0 ? 0 : -1;
}
#else
static int thread_start(HTTP_THREAD *thread, http_thread_fn fn, void *arg) {
    return pthread_create(thread, NULL, fn, arg);
}
static int thread_join(HTTP_THREAD thread) { return pthread_join(thread, NULL); }
#endif

void setUp(void) { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

static Iron_String istr(const char *s) { return iron_string_from_literal(s, strlen(s)); }

static void assert_istr(const char *expected, Iron_String actual) {
    size_t len = iron_string_byte_len(&actual);
    TEST_ASSERT_EQUAL_size_t(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, iron_string_cstr(&actual), len);
}

typedef struct ServerCase {
    Iron_HttpServer server;
    int mode;
    int64_t error;
    Iron_HttpRequest request;
} ServerCase;

static void *serve_one(void *arg) {
    ServerCase *ctx = (ServerCase *)arg;
    Iron_HttpConnectionResult accepted = Iron_httpserver_accept(ctx->server, 5000);
    if (accepted.error) {
        ctx->error = accepted.error;
        Iron_httpconnectionresult_release(accepted);
        return NULL;
    }
    if (ctx->mode == 2 || ctx->mode == 3 || ctx->mode == 4 || ctx->mode == 5) {
        /* Consume the request before closing. Closing a TCP socket with unread
         * request bytes may produce a legal RST on Linux and make this raw
         * mock flaky even though the response bytes were already sent. */
        ctx->request = Iron_httpconnection_read_request(
            accepted.connection, 16384, 1024, 3000);
        if (ctx->request.error) {
            ctx->error = ctx->request.error;
            Iron_httpconnection_close(accepted.connection);
            Iron_httpconnectionresult_release(accepted);
            return NULL;
        }
        const char *wire;
        if (ctx->mode == 2) {
            wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                   "4\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Trailer: yes\r\n\r\n";
        } else if (ctx->mode == 3) {
            wire = "HTTP/1.0 200 OK\r\nContent-Length: 6\r\n\r\nlegacy";
        } else if (ctx->mode == 4) {
            wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n"
                   "Connection: close\r\n\r\n0\r\n\r\n";
        } else {
            wire = "HTTP/1.1 200 OK\r\nContent-Length: 123\r\n"
                   "Connection: close\r\n\r\n";
        }
        Iron_TcpSocket socket = { accepted.connection.fd };
        Iron_Result_Int_Error wr = Iron_net_tcp_send_bytes(
            socket, (const uint8_t *)wire, (int64_t)strlen(wire), 3000);
        ctx->error = wr.v1.code;
        Iron_httpconnection_close(accepted.connection);
        Iron_httpconnectionresult_release(accepted);
        return NULL;
    }
    ctx->request = Iron_httpconnection_read_request(
        accepted.connection, 16384, 1024 * 1024, 3000);
    if (ctx->request.error) {
        ctx->error = ctx->request.error;
        Iron_httpconnection_close(accepted.connection);
        Iron_httpconnectionresult_release(accepted);
        return NULL;
    }
    Iron_HttpResponse response;
    if (ctx->mode == 0) {
        response = Iron_http_html_response(200,
            istr("<!doctype html><title>Iron</title><h1>networking works</h1>"));
    } else {
        response = Iron_http_json_response(201, istr("{\"created\":true}"));
    }
    ctx->error = Iron_httpconnection_send_response(accepted.connection, response, 3000);
    Iron_httpresponse_release(response);
    Iron_httpconnection_close(accepted.connection);
    Iron_httpconnectionresult_release(accepted);
    return NULL;
}

static Iron_HttpServer make_server(int64_t *port_out) {
    Iron_HttpServerResult listen = Iron_http_listen(istr("127.0.0.1"), 0);
    TEST_ASSERT_EQUAL_INT64(0, listen.error);
    *port_out = Iron_httpserver_port(listen.server);
    TEST_ASSERT_GREATER_THAN_INT64(0, *port_out);
    Iron_HttpServer server = listen.server;
    Iron_httpserverresult_release(listen);
    return server;
}

void test_http_response_models_and_header_lookup(void) {
    Iron_HttpResponse response = Iron_http_json_response(201, istr("{\"ok\":true}"));
    TEST_ASSERT_EQUAL_INT64(201, response.status);
    assert_istr("Created", response.reason);
    assert_istr("{\"ok\":true}", response.body);
    Iron_String ct = Iron_http_header(response.headers, istr("content-type"));
    assert_istr("application/json; charset=utf-8", ct);

    Iron_HttpResponse invalid = Iron_http_response(99, istr(""), istr(""));
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_INVALID_ARGUMENT, invalid.error);
    Iron_HttpResponse malformed = Iron_http_response(200, istr("Broken Header"), istr(""));
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_INVALID_ARGUMENT, malformed.error);

    const char *caller_headers =
        "X-Caller-Owned: this header remains valid after response release";
    const char *caller_body =
        "this caller-owned response body is deliberately larger than SSO";
    Iron_String dynamic_headers = iron_string_from_cstr(
        caller_headers, strlen(caller_headers));
    Iron_String dynamic_body = iron_string_from_cstr(
        caller_body, strlen(caller_body));
    Iron_HttpResponse cloned = Iron_http_response(
        200, dynamic_headers, dynamic_body);
    TEST_ASSERT_EQUAL_INT64(0, cloned.error);
    Iron_httpresponse_release(cloned);
    assert_istr(caller_headers, dynamic_headers);
    assert_istr(caller_body, dynamic_body);

    Iron_HttpResponse file = Iron_http_file_response(
        200, istr(__FILE__), istr("text/plain"), 1024 * 1024);
    TEST_ASSERT_EQUAL_INT64(0, file.error);
    TEST_ASSERT_TRUE(strstr(iron_string_cstr(&file.body),
                            "test_http_response_models_and_header_lookup") != NULL);
    Iron_String file_type = Iron_http_header(file.headers, istr("Content-Type"));
    assert_istr("text/plain", file_type);
    Iron_HttpResponse file_too_large = Iron_http_file_response(
        200, istr(__FILE__), istr("text/plain"), 1);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_BODY_TOO_LARGE, file_too_large.error);
    iron_string_release(&ct);
    iron_string_release(&file_type);
    Iron_httpresponse_release(response);
    Iron_httpresponse_release(invalid);
    Iron_httpresponse_release(malformed);
    Iron_httpresponse_release(file);
    Iron_httpresponse_release(file_too_large);
    iron_string_release(&dynamic_headers);
    iron_string_release(&dynamic_body);
}

void test_http_serves_webpage_and_client_gets_it(void) {
    int64_t port = 0;
    Iron_HttpServer server = make_server(&port);
    ServerCase ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = server;
    ctx.mode = 0;
    HTTP_THREAD thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&thread, serve_one, &ctx));

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%lld/", (long long)port);
    Iron_HttpResponse response = Iron_http_get(istr(url), 5000);
    TEST_ASSERT_EQUAL_INT64(0, response.error);
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_TRUE(strstr(iron_string_cstr(&response.body), "networking works") != NULL);
    Iron_String content_type = Iron_http_header(
        response.headers, istr("Content-Type"));
    assert_istr("text/html; charset=utf-8", content_type);

    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, ctx.error);
    assert_istr("GET", ctx.request.method);
    assert_istr("/", ctx.request.path);
    iron_string_release(&content_type);
    Iron_httpresponse_release(response);
    Iron_httprequest_release(ctx.request);
    Iron_httpserver_close(server);
}

void test_http_rest_json_post_roundtrip(void) {
    int64_t port = 0;
    Iron_HttpServer server = make_server(&port);
    ServerCase ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = server;
    ctx.mode = 1;
    HTTP_THREAD thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&thread, serve_one, &ctx));

    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%lld/api/items?draft=1", (long long)port);
    Iron_HttpResponse response = Iron_http_post_json(
        istr(url), istr("{\"name\":\"anvil\"}"), 5000);
    TEST_ASSERT_EQUAL_INT64(0, response.error);
    TEST_ASSERT_EQUAL_INT64(201, response.status);
    assert_istr("{\"created\":true}", response.body);

    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, ctx.error);
    assert_istr("POST", ctx.request.method);
    assert_istr("/api/items", ctx.request.path);
    assert_istr("draft=1", ctx.request.query);
    assert_istr("{\"name\":\"anvil\"}", ctx.request.body);
    Iron_String content_type = Iron_http_header(
        ctx.request.headers, istr("content-type"));
    assert_istr("application/json; charset=utf-8", content_type);
    iron_string_release(&content_type);
    Iron_httpresponse_release(response);
    Iron_httprequest_release(ctx.request);
    Iron_httpserver_close(server);
}

void test_http_client_decodes_chunked_response(void) {
    int64_t port = 0;
    Iron_HttpServer server = make_server(&port);
    ServerCase ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = server;
    ctx.mode = 2;
    HTTP_THREAD thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&thread, serve_one, &ctx));
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%lld/chunked", (long long)port);
    Iron_HttpResponse response = Iron_http_get(istr(url), 5000);
    TEST_ASSERT_EQUAL_INT64(0, response.error);
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    assert_istr("Wikipedia", response.body);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, ctx.error);
    Iron_httpresponse_release(response);
    Iron_httprequest_release(ctx.request);
    Iron_httpserver_close(server);
}

void test_http_client_accepts_http_1_0_response(void) {
    int64_t port = 0;
    Iron_HttpServer server = make_server(&port);
    ServerCase ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = server;
    ctx.mode = 3;
    HTTP_THREAD thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&thread, serve_one, &ctx));
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%lld/legacy", (long long)port);
    Iron_HttpResponse response = Iron_http_get(istr(url), 5000);
    TEST_ASSERT_EQUAL_INT64(0, response.error);
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    assert_istr("legacy", response.body);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, ctx.error);
    Iron_httpresponse_release(response);
    Iron_httprequest_release(ctx.request);
    Iron_httpserver_close(server);
}

void test_http_client_head_does_not_read_declared_body(void) {
    int64_t port = 0;
    Iron_HttpServer server = make_server(&port);
    ServerCase ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = server;
    ctx.mode = 5;
    HTTP_THREAD thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&thread, serve_one, &ctx));
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%lld/metadata",
             (long long)port);
    Iron_HttpResponse response = Iron_http_request(
        istr("HEAD"), istr(url), istr(""), istr(""), 1024, 5000);
    TEST_ASSERT_EQUAL_INT64(0, response.error);
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&response.body));
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, ctx.error);
    assert_istr("HEAD", ctx.request.method);
    Iron_httpresponse_release(response);
    Iron_httprequest_release(ctx.request);
    Iron_httpserver_close(server);
}

void test_http_rejects_unsupported_and_ambiguous_inputs(void) {
    Iron_HttpResponse unsupported = Iron_http_get(istr("ftp://example.com/"), 1000);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_UNSUPPORTED_SCHEME, unsupported.error);

    Iron_HttpResponse framed = Iron_http_request(
        istr("POST"), istr("http://127.0.0.1:1/"),
        istr("Content-Length: 999"), istr("x"), 1024, 100);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_INVALID_ARGUMENT, framed.error);

    Iron_HttpResponse control = Iron_http_response(
        200, istr("X-Test: bad\x7f"), istr(""));
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_INVALID_ARGUMENT, control.error);

    const char nul_url[] = "http://localhost/\0hidden";
    Iron_String nul_input = iron_string_from_cstr(
        nul_url, sizeof(nul_url) - 1);
    Iron_HttpResponse embedded_nul = Iron_http_get(nul_input, 100);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_BAD_URL, embedded_nul.error);

    Iron_HttpClientResult path_origin = Iron_httpclient_open(
        istr("http://example.com/api"), istr(""), false, 1, 1000);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_INVALID_ARGUMENT,
                            path_origin.error);
    Iron_HttpClientResult query_origin = Iron_httpclient_open(
        istr("http://example.com?tenant=one"), istr(""), false, 1, 1000);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_INVALID_ARGUMENT,
                            query_origin.error);
    Iron_HttpClientResult fragment_origin = Iron_httpclient_open(
        istr("http://example.com/#section"), istr(""), false, 1, 1000);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_INVALID_ARGUMENT,
                            fragment_origin.error);
    Iron_HttpClientResult plain_tls_options = Iron_httpclient_open(
        istr("http://example.com"), istr("ca.pem"), false, 1, 1000);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_INVALID_ARGUMENT,
                            plain_tls_options.error);
    Iron_HttpClientResult trailing_slash = Iron_httpclient_open(
        istr("http://example.com/"), istr(""), false, 1, 1000);
    TEST_ASSERT_EQUAL_INT64(0, trailing_slash.error);
    Iron_httpclient_close(trailing_slash.client);
    iron_string_release(&nul_input);
    Iron_httpresponse_release(unsupported);
    Iron_httpresponse_release(framed);
    Iron_httpresponse_release(control);
    Iron_httpresponse_release(embedded_nul);
    Iron_httpclientresult_release(path_origin);
    Iron_httpclientresult_release(query_origin);
    Iron_httpclientresult_release(fragment_origin);
    Iron_httpclientresult_release(plain_tls_options);
    Iron_httpclientresult_release(trailing_slash);
}

static Iron_HttpRequest parse_raw_request(const char *wire, int64_t max_body) {
    Iron_HttpRequest out;
    memset(&out, 0, sizeof(out));
    int64_t port = 0;
    Iron_HttpServer server = make_server(&port);
    Iron_Result_TcpSocket_Error dial = Iron_net_tcp_dial(
        istr("127.0.0.1"), port, 2000);
    if (dial.v1.code != 0) {
        out.error = dial.v1.code;
        Iron_httpserver_close(server);
        return out;
    }
    Iron_HttpConnectionResult accepted = Iron_httpserver_accept(server, 2000);
    if (accepted.error != 0) {
        out.error = accepted.error;
        Iron_httpconnectionresult_release(accepted);
        Iron_tcpsocket_close(dial.v0);
        Iron_httpserver_close(server);
        return out;
    }
    Iron_Result_Int_Error sent = Iron_net_tcp_send_bytes(
        dial.v0, (const uint8_t *)wire, (int64_t)strlen(wire), 2000);
    if (sent.v1.code != 0) out.error = sent.v1.code;
    else out = Iron_httpconnection_read_request(
        accepted.connection, 16384, max_body, 2000);
    Iron_httpconnection_close(accepted.connection);
    Iron_httpconnectionresult_release(accepted);
    Iron_tcpsocket_close(dial.v0);
    Iron_httpserver_close(server);
    return out;
}

void test_http_server_rejects_ambiguous_and_bounded_requests(void) {
    Iron_HttpRequest empty_host = parse_raw_request(
        "GET / HTTP/1.1\r\nHost: \r\n\r\n", 1024);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_MALFORMED_MESSAGE, empty_host.error);

    Iron_HttpRequest ambiguous = parse_raw_request(
        "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n"
        "Transfer-Encoding: chunked\r\n\r\n", 1024);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_MALFORMED_MESSAGE, ambiguous.error);

    Iron_HttpRequest bad_length = parse_raw_request(
        "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: nope\r\n\r\n",
        1024);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_BAD_CONTENT_LENGTH, bad_length.error);

    Iron_HttpRequest oversized = parse_raw_request(
        "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello",
        4);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_BODY_TOO_LARGE, oversized.error);

    Iron_HttpRequest chunked = parse_raw_request(
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nping\r\n0\r\nX-Checksum: ok\r\n\r\n", 1024);
    TEST_ASSERT_EQUAL_INT64(0, chunked.error);
    assert_istr("ping", chunked.body);

    Iron_HttpRequest bad_trailer = parse_raw_request(
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
        "1\r\nx\r\n0\r\nContent-Length: 9\r\n\r\n", 1024);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_MALFORMED_MESSAGE, bad_trailer.error);

    Iron_HttpRequest legacy_keep_alive = parse_raw_request(
        "GET /legacy HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", 1024);
    TEST_ASSERT_EQUAL_INT64(0, legacy_keep_alive.error);
    TEST_ASSERT_TRUE(legacy_keep_alive.keep_alive);

    Iron_HttpRequest explicit_close = parse_raw_request(
        "GET /close HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        1024);
    TEST_ASSERT_EQUAL_INT64(0, explicit_close.error);
    TEST_ASSERT_FALSE(explicit_close.keep_alive);
    Iron_httprequest_release(empty_host);
    Iron_httprequest_release(ambiguous);
    Iron_httprequest_release(bad_length);
    Iron_httprequest_release(oversized);
    Iron_httprequest_release(chunked);
    Iron_httprequest_release(bad_trailer);
    Iron_httprequest_release(legacy_keep_alive);
    Iron_httprequest_release(explicit_close);
}

void test_http_client_rejects_transfer_coding_chain(void) {
    int64_t port = 0;
    Iron_HttpServer server = make_server(&port);
    ServerCase ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = server;
    ctx.mode = 4;
    HTTP_THREAD thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&thread, serve_one, &ctx));
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%lld/coding", (long long)port);
    Iron_HttpResponse response = Iron_http_get(istr(url), 5000);
    TEST_ASSERT_EQUAL_INT64(IRON_ERR_HTTP_UNSUPPORTED_TRANSFER, response.error);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, ctx.error);
    Iron_httpresponse_release(response);
    Iron_httprequest_release(ctx.request);
    Iron_httpserver_close(server);
}

enum { STRESS_CLIENTS = 32 };
typedef struct StressServer {
    Iron_HttpServer server;
    int64_t errors;
} StressServer;

static void *stress_server(void *arg) {
    StressServer *ctx = (StressServer *)arg;
    for (int i = 0; i < STRESS_CLIENTS; i++) {
        Iron_HttpConnectionResult accepted = Iron_httpserver_accept(ctx->server, 10000);
        if (accepted.error) {
            ctx->errors++;
            Iron_httpconnectionresult_release(accepted);
            continue;
        }
        Iron_HttpRequest request = Iron_httpconnection_read_request(
            accepted.connection, 16384, 1024, 5000);
        if (request.error) ctx->errors++;
        else {
            Iron_HttpResponse response = Iron_http_json_response(200, istr("{\"ok\":true}"));
            if (Iron_httpconnection_send_response(accepted.connection, response, 5000)) ctx->errors++;
            Iron_httpresponse_release(response);
        }
        Iron_httprequest_release(request);
        Iron_httpconnection_close(accepted.connection);
        Iron_httpconnectionresult_release(accepted);
    }
    return NULL;
}

typedef struct StressClient { char url[128]; int64_t error; } StressClient;
static void *stress_client(void *arg) {
    StressClient *ctx = (StressClient *)arg;
    Iron_HttpResponse response = Iron_http_get(istr(ctx->url), 10000);
    ctx->error = response.error != 0 ? response.error : (response.status == 200 ? 0 : -1);
    Iron_httpresponse_release(response);
    return NULL;
}

void test_http_32_concurrent_clients(void) {
    int64_t port = 0;
    StressServer server_ctx;
    memset(&server_ctx, 0, sizeof(server_ctx));
    server_ctx.server = make_server(&port);
    HTTP_THREAD server_thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&server_thread, stress_server, &server_ctx));
    HTTP_THREAD clients[STRESS_CLIENTS];
    StressClient client_ctx[STRESS_CLIENTS];
    for (int i = 0; i < STRESS_CLIENTS; i++) {
        memset(&client_ctx[i], 0, sizeof(client_ctx[i]));
        snprintf(client_ctx[i].url, sizeof(client_ctx[i].url),
                 "http://127.0.0.1:%lld/api/status?client=%d",
                 (long long)port, i);
        TEST_ASSERT_EQUAL_INT(0, thread_start(&clients[i], stress_client, &client_ctx[i]));
    }
    for (int i = 0; i < STRESS_CLIENTS; i++) {
        TEST_ASSERT_EQUAL_INT(0, thread_join(clients[i]));
        TEST_ASSERT_EQUAL_INT64(0, client_ctx[i].error);
    }
    TEST_ASSERT_EQUAL_INT(0, thread_join(server_thread));
    TEST_ASSERT_EQUAL_INT64(0, server_ctx.errors);
    Iron_httpserver_close(server_ctx.server);
}

typedef struct PersistentServer {
    Iron_HttpServer server;
    int requests;
    int accepts;
    int64_t error;
} PersistentServer;

static void *persistent_server(void *arg) {
    PersistentServer *ctx = (PersistentServer *)arg;
    Iron_HttpConnectionResult accepted = Iron_httpserver_accept(ctx->server, 5000);
    if (accepted.error) {
        ctx->error = accepted.error;
        Iron_httpconnectionresult_release(accepted);
        return NULL;
    }
    ctx->accepts++;
    for (int i = 0; i < 3; i++) {
        Iron_HttpRequest request = Iron_httpconnection_read_request(
            accepted.connection, 16384, 1024, 3000);
        if (request.error) {
            ctx->error = request.error;
            Iron_httprequest_release(request);
            break;
        }
        ctx->requests++;
        int keep = i < 2;
        Iron_HttpResponse response = Iron_http_text_response(200, request.path);
        ctx->error = Iron_httpconnection_send_response_keep_alive(
            accepted.connection, response, keep, 3000);
        Iron_httpresponse_release(response);
        Iron_httprequest_release(request);
        if (ctx->error) break;
    }
    Iron_httpconnection_close(accepted.connection);
    Iron_httpconnectionresult_release(accepted);
    return NULL;
}

void test_http_client_reuses_one_connection_sequentially(void) {
    int64_t port = 0;
    PersistentServer server = { make_server(&port), 0, 0, 0 };
    HTTP_THREAD thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&thread, persistent_server, &server));
    char origin[128];
    snprintf(origin, sizeof(origin), "http://127.0.0.1:%lld",
             (long long)port);
    Iron_HttpClientResult opened = Iron_httpclient_open(
        istr(origin), istr(""), false, 1, 10000);
    TEST_ASSERT_EQUAL_INT64(0, opened.error);
    const char *targets[] = { "/one", "/two", "/three" };
    for (int i = 0; i < 3; i++) {
        Iron_HttpResponse response = Iron_httpclient_request(
            opened.client, istr("GET"), istr(targets[i]), istr(""), istr(""),
            1024, 5000);
        TEST_ASSERT_EQUAL_INT64(0, response.error);
        TEST_ASSERT_EQUAL_INT64(200, response.status);
        assert_istr(targets[i], response.body);
        TEST_ASSERT_EQUAL_INT(i < 2, response.keep_alive);
        Iron_httpresponse_release(response);
    }
    Iron_httpclient_close(opened.client);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    TEST_ASSERT_EQUAL_INT(1, server.accepts);
    TEST_ASSERT_EQUAL_INT(3, server.requests);
    Iron_httpclientresult_release(opened);
    Iron_httpserver_close(server.server);
}

static void *stale_retry_server(void *arg) {
    PersistentServer *ctx = (PersistentServer *)arg;
    for (int connection_index = 0; connection_index < 2; connection_index++) {
        Iron_HttpConnectionResult accepted = Iron_httpserver_accept(
            ctx->server, 5000);
        if (accepted.error) {
            ctx->error = accepted.error;
            Iron_httpconnectionresult_release(accepted);
            return NULL;
        }
        ctx->accepts++;
        Iron_HttpRequest request = Iron_httpconnection_read_request(
            accepted.connection, 16384, 1024, 3000);
        if (request.error) {
            ctx->error = request.error;
            Iron_httprequest_release(request);
            Iron_httpconnection_close(accepted.connection);
            Iron_httpconnectionresult_release(accepted);
            return NULL;
        }
        ctx->requests++;
        Iron_HttpResponse response = Iron_http_text_response(
            200, istr(connection_index == 0 ? "first" : "retried"));
        ctx->error = Iron_httpconnection_send_response_keep_alive(
            accepted.connection, response, connection_index == 0, 3000);
        Iron_httpresponse_release(response);
        Iron_httprequest_release(request);
        Iron_httpconnection_close(accepted.connection);
        Iron_httpconnectionresult_release(accepted);
        if (ctx->error) return NULL;
    }
    return NULL;
}

void test_http_client_retries_stale_get_connection(void) {
    int64_t port = 0;
    PersistentServer server = { make_server(&port), 0, 0, 0 };
    HTTP_THREAD thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&thread, stale_retry_server, &server));
    char origin[128];
    snprintf(origin, sizeof(origin), "http://127.0.0.1:%lld",
             (long long)port);
    Iron_HttpClientResult opened = Iron_httpclient_open(
        istr(origin), istr(""), false, 1, 10000);
    TEST_ASSERT_EQUAL_INT64(0, opened.error);
    Iron_HttpResponse first = Iron_httpclient_request(
        opened.client, istr("GET"), istr("/first"), istr(""), istr(""),
        1024, 5000);
    TEST_ASSERT_EQUAL_INT64(0, first.error);
    assert_istr("first", first.body);
    Iron_HttpResponse second = Iron_httpclient_request(
        opened.client, istr("GET"), istr("/second"), istr(""), istr(""),
        1024, 5000);
    TEST_ASSERT_EQUAL_INT64(0, second.error);
    assert_istr("retried", second.body);
    Iron_httpclient_close(opened.client);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    TEST_ASSERT_EQUAL_INT(2, server.accepts);
    Iron_httpresponse_release(first);
    Iron_httpresponse_release(second);
    Iron_httpclientresult_release(opened);
    Iron_httpserver_close(server.server);
}

static void *stale_post_server(void *arg) {
    PersistentServer *ctx = (PersistentServer *)arg;
    Iron_HttpConnectionResult accepted = Iron_httpserver_accept(ctx->server, 5000);
    if (accepted.error) {
        ctx->error = accepted.error;
        Iron_httpconnectionresult_release(accepted);
        return NULL;
    }
    ctx->accepts++;
    Iron_HttpRequest request = Iron_httpconnection_read_request(
        accepted.connection, 16384, 1024, 3000);
    if (request.error) {
        ctx->error = request.error;
        Iron_httprequest_release(request);
        Iron_httpconnection_close(accepted.connection);
        Iron_httpconnectionresult_release(accepted);
        return NULL;
    }
    ctx->requests++;
    Iron_HttpResponse response = Iron_http_text_response(200, istr("primed"));
    ctx->error = Iron_httpconnection_send_response_keep_alive(
        accepted.connection, response, true, 3000);
    Iron_httpresponse_release(response);
    Iron_httprequest_release(request);
    Iron_httpconnection_close(accepted.connection);
    Iron_httpconnectionresult_release(accepted);
    if (ctx->error) return NULL;

    /* A POST attempted on the stale pooled socket must not be replayed onto a
     * new connection. A short accept timeout provides the negative oracle. */
    Iron_HttpConnectionResult replay = Iron_httpserver_accept(ctx->server, 300);
    if (replay.error == 0) {
        ctx->error = -90;
        Iron_httpconnection_close(replay.connection);
    } else if (replay.error != IRON_ERR_NET_TIMEOUT) {
        ctx->error = replay.error;
    }
    Iron_httpconnectionresult_release(replay);
    return NULL;
}

void test_http_client_does_not_retry_stale_post(void) {
    int64_t port = 0;
    PersistentServer server = { make_server(&port), 0, 0, 0 };
    HTTP_THREAD thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&thread, stale_post_server, &server));
    char origin[128];
    snprintf(origin, sizeof(origin), "http://127.0.0.1:%lld",
             (long long)port);
    Iron_HttpClientResult opened = Iron_httpclient_open(
        istr(origin), istr(""), false, 1, 10000);
    TEST_ASSERT_EQUAL_INT64(0, opened.error);
    Iron_HttpResponse primed = Iron_httpclient_request(
        opened.client, istr("GET"), istr("/prime"), istr(""), istr(""),
        1024, 5000);
    TEST_ASSERT_EQUAL_INT64(0, primed.error);
    Iron_HttpResponse post = Iron_httpclient_request(
        opened.client, istr("POST"), istr("/side-effect"),
        istr("Content-Type: text/plain"), istr("create"), 1024, 2000);
    TEST_ASSERT_NOT_EQUAL(0, post.error);
    Iron_httpclient_close(opened.client);
    TEST_ASSERT_EQUAL_INT(0, thread_join(thread));
    TEST_ASSERT_EQUAL_INT64(0, server.error);
    TEST_ASSERT_EQUAL_INT(1, server.accepts);
    Iron_httpresponse_release(primed);
    Iron_httpresponse_release(post);
    Iron_httpclientresult_release(opened);
    Iron_httpserver_close(server.server);
}

enum { POOLED_REQUESTS = 12, POOL_LIMIT = 4 };
typedef struct PoolServer {
    Iron_HttpServer server;
    atomic_int active;
    atomic_int peak;
    atomic_int errors;
    atomic_int release_first_wave;
} PoolServer;

typedef struct PoolHandler {
    PoolServer *server;
    Iron_HttpConnection connection;
} PoolHandler;

static void pool_pause_one_millisecond(void) {
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec pause = { 0, 1000 * 1000 };
    nanosleep(&pause, NULL);
#endif
}

static void *pool_handler(void *arg) {
    PoolHandler *handler = (PoolHandler *)arg;
    int active = atomic_fetch_add(&handler->server->active, 1) + 1;
    int peak = atomic_load(&handler->server->peak);
    while (peak < active && !atomic_compare_exchange_weak(
        &handler->server->peak, &peak, active)) { }
    /* Hold the first admitted requests so no client slot can be released.
     * The test can then observe the admission bound without counting the
     * server-side close/bookkeeping interval as an extra connection. */
    while (!atomic_load_explicit(&handler->server->release_first_wave,
                                 memory_order_acquire))
        pool_pause_one_millisecond();
    Iron_HttpRequest request = Iron_httpconnection_read_request(
        handler->connection, 16384, 1024, 3000);
    Iron_HttpResponse response = Iron_http_text_response(200, istr("pooled"));
    if (request.error || Iron_httpconnection_send_response(
            handler->connection, response, 3000))
        atomic_fetch_add(&handler->server->errors, 1);
    Iron_httpresponse_release(response);
    Iron_httprequest_release(request);
    Iron_httpconnection_close(handler->connection);
    atomic_fetch_sub(&handler->server->active, 1);
    return NULL;
}

static void *pool_server(void *arg) {
    PoolServer *ctx = (PoolServer *)arg;
    HTTP_THREAD handlers[POOLED_REQUESTS];
    PoolHandler cases[POOLED_REQUESTS];
    int started = 0;
    for (int i = 0; i < POOLED_REQUESTS; i++) {
        Iron_HttpConnectionResult accepted = Iron_httpserver_accept(
            ctx->server, 10000);
        if (accepted.error) {
            atomic_fetch_add(&ctx->errors, 1);
            Iron_httpconnectionresult_release(accepted);
            break;
        }
        cases[i].server = ctx;
        cases[i].connection = accepted.connection;
        if (thread_start(&handlers[i], pool_handler, &cases[i]) != 0) {
            atomic_fetch_add(&ctx->errors, 1);
            Iron_httpconnection_close(accepted.connection);
            Iron_httpconnectionresult_release(accepted);
            break;
        }
        Iron_httpconnectionresult_release(accepted);
        started++;
    }
    for (int i = 0; i < started; i++)
        if (thread_join(handlers[i]) != 0) atomic_fetch_add(&ctx->errors, 1);
    return NULL;
}

typedef struct PoolClient {
    Iron_HttpClient client;
    int64_t error;
} PoolClient;

static void *pool_client(void *arg) {
    PoolClient *test = (PoolClient *)arg;
    Iron_HttpResponse response = Iron_httpclient_request(
        test->client, istr("GET"), istr("/pool"), istr(""), istr(""),
        1024, 10000);
    test->error = response.error;
    Iron_httpresponse_release(response);
    return NULL;
}

void test_http_client_pool_bounds_concurrent_connections(void) {
    int64_t port = 0;
    PoolServer server;
    memset(&server, 0, sizeof(server));
    server.server = make_server(&port);
    atomic_init(&server.active, 0);
    atomic_init(&server.peak, 0);
    atomic_init(&server.errors, 0);
    atomic_init(&server.release_first_wave, 0);
    HTTP_THREAD server_thread;
    TEST_ASSERT_EQUAL_INT(0, thread_start(&server_thread, pool_server, &server));
    char origin[128];
    snprintf(origin, sizeof(origin), "http://127.0.0.1:%lld",
             (long long)port);
    Iron_HttpClientResult opened = Iron_httpclient_open(
        istr(origin), istr(""), false, POOL_LIMIT, 10000);
    TEST_ASSERT_EQUAL_INT64(0, opened.error);
    HTTP_THREAD threads[POOLED_REQUESTS];
    PoolClient clients[POOLED_REQUESTS];
    for (int i = 0; i < POOLED_REQUESTS; i++) {
        clients[i].client = opened.client;
        clients[i].error = 0;
        TEST_ASSERT_EQUAL_INT(0, thread_start(
            &threads[i], pool_client, &clients[i]));
    }
    /* Wait for the four slots to fill, then leave them blocked briefly. If
     * the pool admits a fifth request, the server peak records it before the
     * barrier is released. */
    for (int i = 0; i < 3000 &&
         atomic_load_explicit(&server.active, memory_order_acquire) <
             POOL_LIMIT; i++)
        pool_pause_one_millisecond();
    for (int i = 0; i < 100; i++) pool_pause_one_millisecond();
    int first_wave_peak = atomic_load_explicit(&server.peak,
                                                memory_order_acquire);
    atomic_store_explicit(&server.release_first_wave, 1,
                          memory_order_release);
    for (int i = 0; i < POOLED_REQUESTS; i++) {
        TEST_ASSERT_EQUAL_INT(0, thread_join(threads[i]));
        TEST_ASSERT_EQUAL_INT64(0, clients[i].error);
    }
    Iron_httpclient_close(opened.client);
    TEST_ASSERT_EQUAL_INT(0, thread_join(server_thread));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&server.errors));
    TEST_ASSERT_EQUAL_INT(POOL_LIMIT, first_wave_peak);
    Iron_httpclientresult_release(opened);
    Iron_httpserver_close(server.server);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_http_response_models_and_header_lookup);
    RUN_TEST(test_http_serves_webpage_and_client_gets_it);
    RUN_TEST(test_http_rest_json_post_roundtrip);
    RUN_TEST(test_http_client_decodes_chunked_response);
    RUN_TEST(test_http_client_accepts_http_1_0_response);
    RUN_TEST(test_http_client_head_does_not_read_declared_body);
    RUN_TEST(test_http_rejects_unsupported_and_ambiguous_inputs);
    RUN_TEST(test_http_server_rejects_ambiguous_and_bounded_requests);
    RUN_TEST(test_http_client_rejects_transfer_coding_chain);
    RUN_TEST(test_http_32_concurrent_clients);
    RUN_TEST(test_http_client_reuses_one_connection_sequentially);
    RUN_TEST(test_http_client_retries_stale_get_connection);
    RUN_TEST(test_http_client_does_not_retry_stale_post);
    RUN_TEST(test_http_client_pool_bounds_concurrent_connections);
    return UNITY_END();
}
