/* RFC 9110 and RFC 9112 black-box tests for the production HTTP parser. */
#include "runtime/iron_errors.h"
#include "runtime/iron_runtime.h"
#include "stdlib/iron_http.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        failures++; \
    } \
} while (0)

static Iron_String istr(const char *s) {
    return iron_string_from_literal(s, strlen(s));
}

static int string_is(Iron_String value, const char *expected) {
    size_t n = strlen(expected);
    return iron_string_byte_len(&value) == n &&
           memcmp(iron_string_cstr(&value), expected, n) == 0;
}

static int write_all(int fd, const uint8_t *data, size_t len) {
    while (len) {
        ssize_t n = send(fd, data, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        data += (size_t)n;
        len -= (size_t)n;
    }
    return 1;
}

typedef struct Sender {
    int fd;
    const uint8_t *data;
    size_t len;
    size_t split;
} Sender;

static void *send_fragments(void *arg) {
    Sender *sender = (Sender *)arg;
    size_t first = sender->split < sender->len ? sender->split : sender->len;
    if (first) (void)write_all(sender->fd, sender->data, first);
    if (first < sender->len) {
        usleep(1000);
        (void)write_all(sender->fd, sender->data + first, sender->len - first);
    }
    (void)shutdown(sender->fd, SHUT_WR);
    return NULL;
}

static Iron_HttpRequest parse_request(const char *wire, size_t len,
                                      size_t split, int64_t max_header,
                                      int64_t max_body) {
    int pair[2] = {-1, -1};
    Iron_HttpRequest out;
    memset(&out, 0, sizeof(out));
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        out.error = -1;
        return out;
    }
    Sender sender = {pair[0], (const uint8_t *)wire, len, split};
    pthread_t thread;
    if (pthread_create(&thread, NULL, send_fragments, &sender) != 0) {
        close(pair[0]);
        close(pair[1]);
        out.error = -1;
        return out;
    }
    Iron_HttpConnection connection = {pair[1]};
    out = Iron_httpconnection_read_request(
        connection, max_header, max_body, 3000);
    (void)pthread_join(thread, NULL);
    close(pair[0]);
    Iron_httpconnection_close(connection);
    return out;
}

typedef struct RequestCase {
    const char *name;
    const char *wire;
    int64_t error;
    const char *method;
    const char *target;
    const char *path;
    const char *query;
    const char *body;
    int keep_alive;
} RequestCase;

#define REJECT(n, w, e) {n, w, e, NULL, NULL, NULL, NULL, NULL, 0}

static void check_request_case(const RequestCase *test, size_t split) {
    Iron_HttpRequest out = parse_request(
        test->wire, strlen(test->wire), split, 65536, 1024 * 1024);
    CHECK(out.error == test->error, "%s: error %lld, expected %lld",
          test->name, (long long)out.error, (long long)test->error);
    if (test->error == 0 && out.error == 0) {
        CHECK(string_is(out.method, test->method), "%s: method", test->name);
        CHECK(string_is(out.target, test->target), "%s: target", test->name);
        CHECK(string_is(out.path, test->path), "%s: path", test->name);
        CHECK(string_is(out.query, test->query), "%s: query", test->name);
        CHECK(string_is(out.body, test->body), "%s: body", test->name);
        CHECK(out.keep_alive == test->keep_alive, "%s: keep_alive", test->name);
    }
    Iron_httprequest_release(out);
}

static void test_request_lines_headers_and_framing(void) {
    static const RequestCase cases[] = {
        {"origin", "GET /a%20b?q=x%2Fy HTTP/1.1\r\nHost: example\r\n\r\n",
         0, "GET", "/a%20b?q=x%2Fy", "/a%20b", "q=x%2Fy", "", 1},
        {"absolute", "GET http://example.test/a/b?q=1 HTTP/1.1\r\nHost: example.test\r\n\r\n",
         0, "GET", "http://example.test/a/b?q=1", "/a/b", "q=1", "", 1},
        {"absolute-empty-path", "GET http://example.test?q=1 HTTP/1.1\r\nHost: example.test\r\n\r\n",
         0, "GET", "http://example.test?q=1", "", "q=1", "", 1},
        {"asterisk", "OPTIONS * HTTP/1.1\r\nHost: example\r\n\r\n",
         0, "OPTIONS", "*", "*", "", "", 1},
        {"authority", "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n",
         0, "CONNECT", "example.com:443", "example.com:443", "", "", 1},
        {"authority-ipv6", "CONNECT [2001:db8::1]:443 HTTP/1.1\r\nHost: [2001:db8::1]:443\r\n\r\n",
         0, "CONNECT", "[2001:db8::1]:443", "[2001:db8::1]:443", "", "", 1},
        {"long-method", "CUSTOM-METHOD-WITH-MORE-THAN-THIRTY-TWO-CHARS / HTTP/1.1\r\nHost: x\r\n\r\n",
         0, "CUSTOM-METHOD-WITH-MORE-THAN-THIRTY-TWO-CHARS", "/", "/", "", "", 1},
        {"token-header-empty-and-repeated",
         "GET / HTTP/1.1\r\nHost: x\r\n!#$%&'*+-.^_`|~: token\r\nX-Empty:\t \r\nX-R: a\r\nX-R: b\r\nConnection: upgrade\r\nConnection: close\r\n\r\n",
         0, "GET", "/", "/", "", "", 0},
        {"content-length", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length:\t4 \r\n\r\ntest",
         0, "POST", "/", "/", "", "test", 1},
        {"content-length-repeated", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\ntest",
         0, "POST", "/", "/", "", "test", 1},
        {"content-length-list", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4, 4\r\n\r\ntest",
         0, "POST", "/", "/", "", "test", 1},
        {"chunked-extensions-trailers",
         "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
         "4; foo=bar; quoted=\"a\\\"b\"\r\nWiki\r\n5\r\npedia\r\n0; done=yes\r\nX-Checksum: ok\r\n\r\n",
         0, "POST", "/upload", "/upload", "", "Wikipedia", 1},
        {"http10-close", "GET / HTTP/1.0\r\n\r\n",
         0, "GET", "/", "/", "", "", 0},
        {"http10-keep-alive", "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
         0, "GET", "/", "/", "", "", 1},

        REJECT("missing-host", "GET / HTTP/1.1\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("empty-host", "GET / HTTP/1.1\r\nHost:\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("duplicate-host", "GET / HTTP/1.1\r\nHost: a\r\nHost: a\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("bad-method", "GE(T / HTTP/1.1\r\nHost: x\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("lowercase-version", "GET / http/1.1\r\nHost: x\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("bare-lf", "GET / HTTP/1.1\nHost: x\n\n", IRON_ERR_HTTP_TRUNCATED_MESSAGE),
        REJECT("obs-fold", "GET / HTTP/1.1\r\nHost: x\r\nX-A: one\r\n two\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("space-before-colon", "GET / HTTP/1.1\r\nHost : x\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("bad-header-name", "GET / HTTP/1.1\r\nHost: x\r\nBad(Name: x\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("ctl-value", "GET / HTTP/1.1\r\nHost: x\r\nX-A: a\001b\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("bad-percent", "GET /bad%zz HTTP/1.1\r\nHost: x\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("fragment", "GET /a#frag HTTP/1.1\r\nHost: x\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("asterisk-wrong-method", "GET * HTTP/1.1\r\nHost: x\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("authority-wrong-method", "GET example.com:443 HTTP/1.1\r\nHost: x\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("authority-bad-ipv6", "CONNECT [2001:::1]:443 HTTP/1.1\r\nHost: x\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("authority-missing-bracket", "CONNECT [2001:db8::1:443 HTTP/1.1\r\nHost: x\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("authority-bad-port", "CONNECT example.com:https HTTP/1.1\r\nHost: x\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("content-length-conflict", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\ntest", IRON_ERR_HTTP_BAD_CONTENT_LENGTH),
        REJECT("content-length-conflict-list", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4, 5\r\n\r\ntestx", IRON_ERR_HTTP_BAD_CONTENT_LENGTH),
        REJECT("content-length-sign", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +4\r\n\r\ntest", IRON_ERR_HTTP_BAD_CONTENT_LENGTH),
        REJECT("content-length-overflow", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 18446744073709551616\r\n\r\n", IRON_ERR_HTTP_BAD_CONTENT_LENGTH),
        REJECT("cl-and-te", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("unsupported-transfer", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n", IRON_ERR_HTTP_UNSUPPORTED_TRANSFER),
        REJECT("bad-chunk-hex", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nx\r\n0\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("bad-chunk-extension", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n1;=x\r\nx\r\n0\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("bad-chunk-quote", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n1;x=\"open\r\nx\r\n0\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("bad-chunk-data-crlf", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nxX\r\n0\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("forbidden-trailer", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nContent-Length: 1\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("chunk-size-size-max", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nffffffffffffffff\r\n", IRON_ERR_HTTP_BODY_TOO_LARGE),
        REJECT("chunk-size-overflow", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
        REJECT("unsupported-expect", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: something-else\r\n\r\n", IRON_ERR_HTTP_MALFORMED_MESSAGE),
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        check_request_case(&cases[i], SIZE_MAX);

    static const size_t split_cases[] = {0, 9, 11};
    for (size_t c = 0; c < sizeof(split_cases) / sizeof(split_cases[0]); c++) {
        const RequestCase *test = &cases[split_cases[c]];
        size_t n = strlen(test->wire);
        for (size_t split = 0; split <= n; split++)
            check_request_case(test, split);
    }

    const char *large = "GET / HTTP/1.1\r\nHost: x\r\nX-Large: 1234567890\r\n\r\n";
    Iron_HttpRequest limited = parse_request(
        large, strlen(large), SIZE_MAX, 32, 1024);
    CHECK(limited.error == IRON_ERR_HTTP_HEADERS_TOO_LARGE,
          "header limit: error %lld", (long long)limited.error);
    Iron_httprequest_release(limited);

    const char *body = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    limited = parse_request(body, strlen(body), SIZE_MAX, 1024, 4);
    CHECK(limited.error == IRON_ERR_HTTP_BODY_TOO_LARGE,
          "body limit: error %lld", (long long)limited.error);
    Iron_httprequest_release(limited);
}

static void test_pipelined_requests(void) {
    const char *wire =
        "GET /one HTTP/1.1\r\nHost: x\r\n\r\n"
        "POST /two HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\ntwo"
        "GET /three HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "pipeline socketpair");
    CHECK(write_all(pair[0], (const uint8_t *)wire, strlen(wire)), "pipeline write");
    (void)shutdown(pair[0], SHUT_WR);
    Iron_HttpConnection connection = {pair[1]};
    Iron_HttpRequest one = Iron_httpconnection_read_request(connection, 4096, 4096, 1000);
    Iron_HttpRequest two = Iron_httpconnection_read_request(connection, 4096, 4096, 1000);
    Iron_HttpRequest three = Iron_httpconnection_read_request(connection, 4096, 4096, 1000);
    CHECK(one.error == 0 && string_is(one.path, "/one"), "pipeline first");
    CHECK(two.error == 0 && string_is(two.path, "/two") && string_is(two.body, "two"),
          "pipeline second");
    CHECK(three.error == 0 && string_is(three.path, "/three") && !three.keep_alive,
          "pipeline third");
    Iron_httprequest_release(one);
    Iron_httprequest_release(two);
    Iron_httprequest_release(three);
    close(pair[0]);
    Iron_httpconnection_close(connection);
}

typedef struct ExpectReader {
    Iron_HttpConnection connection;
    Iron_HttpRequest result;
} ExpectReader;

static void *read_expected_request(void *arg) {
    ExpectReader *reader = (ExpectReader *)arg;
    reader->result = Iron_httpconnection_read_request(
        reader->connection, 4096, 4096, 3000);
    return NULL;
}

static void test_expect_continue(void) {
    const char *head = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n";
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "expect socketpair");
    ExpectReader reader;
    memset(&reader, 0, sizeof(reader));
    reader.connection.fd = pair[1];
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, read_expected_request, &reader) == 0,
          "expect reader thread");
    CHECK(write_all(pair[0], (const uint8_t *)head, strlen(head)), "expect head write");
    char interim[64] = {0};
    ssize_t n = recv(pair[0], interim, sizeof(interim) - 1, 0);
    CHECK(n == 25 && memcmp(interim, "HTTP/1.1 100 Continue\r\n\r\n", 25) == 0,
          "expect interim response: %.*s", n > 0 ? (int)n : 0, interim);
    CHECK(write_all(pair[0], (const uint8_t *)"body", 4), "expect body write");
    (void)shutdown(pair[0], SHUT_WR);
    (void)pthread_join(thread, NULL);
    CHECK(reader.result.error == 0 && string_is(reader.result.body, "body"),
          "expect parsed body");
    Iron_httprequest_release(reader.result);
    close(pair[0]);
    Iron_httpconnection_close(reader.connection);
}

static void test_body_free_serialization(void) {
    static const int statuses[] = {100, 204, 304};
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        int pair[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "serialize socketpair");
        Iron_HttpResponse response = Iron_http_response(
            statuses[i], istr("X-Test: yes"), istr("must-not-be-sent"));
        Iron_HttpConnection connection = {pair[1]};
        int64_t error = Iron_httpconnection_send_response(
            connection, response, 1000);
        (void)shutdown(pair[1], SHUT_WR);
        char wire[512];
        ssize_t n = recv(pair[0], wire, sizeof(wire) - 1, 0);
        CHECK(error == 0 && n > 0, "serialize status %d", statuses[i]);
        if (n > 0) {
            wire[n] = 0;
            CHECK(strstr(wire, "must-not-be-sent") == NULL,
                  "status %d serialized a body", statuses[i]);
            if (statuses[i] == 100 || statuses[i] == 204)
                CHECK(strstr(wire, "Content-Length") == NULL,
                      "status %d serialized Content-Length", statuses[i]);
        }
        Iron_httpresponse_release(response);
        close(pair[0]);
        Iron_httpconnection_close(connection);
    }
}

typedef struct RawResponseServer {
    Iron_HttpServer server;
    const char *wire;
    size_t split;
    int64_t error;
} RawResponseServer;

static void *serve_raw_response(void *arg) {
    RawResponseServer *server = (RawResponseServer *)arg;
    Iron_HttpConnectionResult accepted = Iron_httpserver_accept(server->server, 3000);
    if (accepted.error != 0) {
        server->error = accepted.error;
        return NULL;
    }
    Iron_HttpRequest request = Iron_httpconnection_read_request(
        accepted.connection, 4096, 4096, 3000);
    if (request.error != 0) {
        server->error = request.error;
    } else {
        size_t len = strlen(server->wire);
        size_t first = server->split < len ? server->split : len;
        if (!write_all((int)accepted.connection.fd,
                       (const uint8_t *)server->wire, first)) {
            server->error = -1;
        } else {
            if (first < len) usleep(1000);
            if (!write_all((int)accepted.connection.fd,
                           (const uint8_t *)server->wire + first,
                           len - first)) server->error = -1;
        }
    }
    Iron_httprequest_release(request);
    Iron_httpconnection_close(accepted.connection);
    Iron_httpconnectionresult_release(accepted);
    return NULL;
}

static Iron_HttpResponse parse_response_split(
    const char *method, const char *wire, int64_t max_body, size_t split) {
    Iron_HttpServerResult listening = Iron_http_listen(istr("127.0.0.1"), 0);
    Iron_HttpResponse out;
    memset(&out, 0, sizeof(out));
    if (listening.error != 0) {
        out.error = listening.error;
        return out;
    }
    int64_t port = Iron_httpserver_port(listening.server);
    RawResponseServer server = {listening.server, wire, split, 0};
    pthread_t thread;
    if (pthread_create(&thread, NULL, serve_raw_response, &server) != 0) {
        Iron_httpserver_close(listening.server);
        out.error = -1;
        return out;
    }
    char url[128];
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%lld/test",
                   (long long)port);
    out = Iron_http_request(istr(method), istr(url), istr(""), istr(""),
                            max_body, 3000);
    (void)pthread_join(thread, NULL);
    CHECK(server.error == 0, "raw response server error %lld",
          (long long)server.error);
    Iron_httpserver_close(listening.server);
    Iron_httpserverresult_release(listening);
    return out;
}

static Iron_HttpResponse parse_response(const char *method, const char *wire,
                                        int64_t max_body) {
    return parse_response_split(method, wire, max_body, SIZE_MAX);
}

typedef struct ResponseCase {
    const char *name;
    const char *method;
    const char *wire;
    int64_t error;
    int64_t status;
    const char *body;
    int keep_alive;
} ResponseCase;

#define RESPONSE_REJECT(n, w, e) {n, "GET", w, e, 0, NULL, 0}

static void test_response_lines_headers_and_framing(void) {
    static const ResponseCase cases[] = {
        {"fixed", "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
         0, 200, "hello", 1},
        {"empty-reason", "GET", "HTTP/1.1 200 \r\nContent-Length: 0\r\n\r\n",
         0, 200, "", 1},
        {"repeated-content-length", "GET",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello",
         0, 200, "hello", 1},
        {"combined-content-length", "GET",
         "HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\n\r\nhello",
         0, 200, "hello", 1},
        {"chunked", "GET",
         "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
         "2;x=y\r\nhe\r\n3\r\nllo\r\n0\r\nX-End: yes\r\n\r\n",
         0, 200, "hello", 1},
        {"close-delimited", "GET",
         "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello",
         0, 200, "hello", 0},
        {"http10-close-delimited", "GET", "HTTP/1.0 200 OK\r\n\r\nhello",
         0, 200, "hello", 0},
        {"head", "HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
         0, 200, "", 1},
        {"informational", "GET",
         "HTTP/1.1 100 Continue\r\n\r\n"
         "HTTP/1.1 103 Early Hints\r\nLink: </a>; rel=preload\r\n\r\n"
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
         0, 200, "hello", 1},
        {"no-content", "GET", "HTTP/1.1 204 No Content\r\n\r\nignored",
         0, 204, "", 1},
        {"not-modified", "GET",
         "HTTP/1.1 304 Not Modified\r\nContent-Length: 123\r\n\r\nignored",
         0, 304, "", 1},

        RESPONSE_REJECT("missing-reason-space",
                        "HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n",
                        IRON_ERR_HTTP_MALFORMED_MESSAGE),
        RESPONSE_REJECT("lowercase-version",
                        "http/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
                        IRON_ERR_HTTP_MALFORMED_MESSAGE),
        RESPONSE_REJECT("bad-status-text",
                        "HTTP/1.1 2x0 OK\r\nContent-Length: 0\r\n\r\n",
                        IRON_ERR_HTTP_MALFORMED_MESSAGE),
        RESPONSE_REJECT("bad-status-low",
                        "HTTP/1.1 099 Nope\r\nContent-Length: 0\r\n\r\n",
                        IRON_ERR_HTTP_MALFORMED_MESSAGE),
        RESPONSE_REJECT("bad-status-high",
                        "HTTP/1.1 600 Nope\r\nContent-Length: 0\r\n\r\n",
                        IRON_ERR_HTTP_MALFORMED_MESSAGE),
        RESPONSE_REJECT("reason-control",
                        "HTTP/1.1 200 bad\001reason\r\nContent-Length: 0\r\n\r\n",
                        IRON_ERR_HTTP_MALFORMED_MESSAGE),
        RESPONSE_REJECT("obs-fold",
                        "HTTP/1.1 200 OK\r\nX-A: one\r\n two\r\nContent-Length: 0\r\n\r\n",
                        IRON_ERR_HTTP_MALFORMED_MESSAGE),
        RESPONSE_REJECT("content-length-conflict",
                        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\nhello",
                        IRON_ERR_HTTP_BAD_CONTENT_LENGTH),
        RESPONSE_REJECT("cl-and-te",
                        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                        IRON_ERR_HTTP_MALFORMED_MESSAGE),
        RESPONSE_REJECT("unsupported-transfer",
                        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
                        IRON_ERR_HTTP_UNSUPPORTED_TRANSFER),
        RESPONSE_REJECT("content-length-on-204",
                        "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n",
                        IRON_ERR_HTTP_MALFORMED_MESSAGE),
        RESPONSE_REJECT("transfer-encoding-on-100",
                        "HTTP/1.1 100 Continue\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                        IRON_ERR_HTTP_MALFORMED_MESSAGE),
        RESPONSE_REJECT("bad-chunk-extension",
                        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;=x\r\nx\r\n0\r\n\r\n",
                        IRON_ERR_HTTP_MALFORMED_MESSAGE),
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Iron_HttpResponse out = parse_response(
            cases[i].method, cases[i].wire, 1024 * 1024);
        CHECK(out.error == cases[i].error,
              "%s: response error %lld, expected %lld", cases[i].name,
              (long long)out.error, (long long)cases[i].error);
        if (out.error == 0 && cases[i].error == 0) {
            CHECK(out.status == cases[i].status, "%s: response status", cases[i].name);
            CHECK(string_is(out.body, cases[i].body), "%s: response body", cases[i].name);
            CHECK(out.keep_alive == cases[i].keep_alive,
                  "%s: response keep_alive", cases[i].name);
        }
        Iron_httpresponse_release(out);
    }

    static const size_t split_cases[] = {0, 4, 8};
    for (size_t c = 0; c < sizeof(split_cases) / sizeof(split_cases[0]); c++) {
        const ResponseCase *test = &cases[split_cases[c]];
        size_t len = strlen(test->wire);
        for (size_t split = 0; split <= len; split++) {
            Iron_HttpResponse out = parse_response_split(
                test->method, test->wire, 1024 * 1024, split);
            CHECK(out.error == 0 && out.status == test->status &&
                  string_is(out.body, test->body),
                  "%s split %zu: error %lld", test->name, split,
                  (long long)out.error);
            Iron_httpresponse_release(out);
        }
    }

    Iron_HttpResponse limited = parse_response(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", 4);
    CHECK(limited.error == IRON_ERR_HTTP_BODY_TOO_LARGE,
          "response body limit: %lld", (long long)limited.error);
    Iron_httpresponse_release(limited);
}

int main(void) {
    iron_runtime_init(0, NULL);
    test_request_lines_headers_and_framing();
    test_pipelined_requests();
    test_expect_continue();
    test_body_free_serialization();
    test_response_lines_headers_and_framing();
    iron_runtime_shutdown();
    if (failures) {
        fprintf(stderr, "%d HTTP conformance checks failed\n", failures);
        return 1;
    }
    puts("HTTP conformance: all checks passed");
    return 0;
}
