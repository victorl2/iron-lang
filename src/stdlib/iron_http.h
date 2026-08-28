#ifndef IRON_HTTP_H
#define IRON_HTTP_H

#include <stdint.h>
#include <stdbool.h>
#include "runtime/iron_runtime.h"

/* These layouts must match src/stdlib/http.iron exactly. */
typedef struct Iron_HttpServer { int64_t fd; } Iron_HttpServer;
typedef struct Iron_HttpConnection { int64_t fd; } Iron_HttpConnection;
typedef struct Iron_HttpsServer { int64_t fd; int64_t context; } Iron_HttpsServer;
typedef struct Iron_HttpsPendingConnection {
    int64_t fd;
    int64_t context;
} Iron_HttpsPendingConnection;
typedef struct Iron_HttpsConnection { int64_t fd; int64_t tls; } Iron_HttpsConnection;
typedef struct Iron_HttpClient { int64_t handle; } Iron_HttpClient;

typedef struct Iron_HttpServerResult {
    Iron_HttpServer server;
    int64_t error;
    Iron_String error_message;
} Iron_HttpServerResult;

typedef struct Iron_HttpConnectionResult {
    Iron_HttpConnection connection;
    int64_t error;
    Iron_String error_message;
} Iron_HttpConnectionResult;

typedef struct Iron_HttpsServerResult {
    Iron_HttpsServer server;
    int64_t error;
    Iron_String error_message;
} Iron_HttpsServerResult;

typedef struct Iron_HttpsConnectionResult {
    Iron_HttpsConnection connection;
    int64_t error;
    Iron_String error_message;
} Iron_HttpsConnectionResult;

typedef struct Iron_HttpsPendingConnectionResult {
    Iron_HttpsPendingConnection connection;
    int64_t error;
    Iron_String error_message;
} Iron_HttpsPendingConnectionResult;

typedef struct Iron_HttpClientResult {
    Iron_HttpClient client;
    int64_t error;
    Iron_String error_message;
} Iron_HttpClientResult;

typedef struct Iron_HttpRequest {
    Iron_String method;
    Iron_String target;
    Iron_String path;
    Iron_String query;
    Iron_String version;
    Iron_String headers;
    Iron_String body;
    bool keep_alive;
    int64_t error;
    Iron_String error_message;
} Iron_HttpRequest;

typedef struct Iron_HttpResponse {
    int64_t status;
    Iron_String reason;
    Iron_String headers;
    Iron_String body;
    bool keep_alive;
    int64_t error;
    Iron_String error_message;
} Iron_HttpResponse;

Iron_HttpServerResult Iron_http_listen(Iron_String host, int64_t port);
Iron_HttpConnectionResult Iron_httpserver_accept(Iron_HttpServer server,
                                                  int64_t timeout);
int64_t Iron_httpserver_port(Iron_HttpServer server);
void Iron_httpserver_close(Iron_HttpServer server);

Iron_HttpRequest Iron_httpconnection_read_request(Iron_HttpConnection connection,
                                                   int64_t max_header_bytes,
                                                   int64_t max_body_bytes,
                                                   int64_t timeout);
int64_t Iron_httpconnection_send_response(Iron_HttpConnection connection,
                                           Iron_HttpResponse response,
                                           int64_t timeout);
int64_t Iron_httpconnection_send_response_keep_alive(
    Iron_HttpConnection connection, Iron_HttpResponse response,
    bool keep_alive, int64_t timeout);
void Iron_httpconnection_close(Iron_HttpConnection connection);

Iron_HttpsServerResult Iron_http_listen_tls(Iron_String host, int64_t port,
                                             Iron_String certificate_file,
                                             Iron_String private_key_file);
Iron_HttpsConnectionResult Iron_httpsserver_accept(Iron_HttpsServer server,
                                                    int64_t timeout);
Iron_HttpsPendingConnectionResult Iron_httpsserver_accept_tcp(
    Iron_HttpsServer server, int64_t timeout);
Iron_HttpsConnectionResult Iron_httpspendingconnection_handshake(
    Iron_HttpsPendingConnection connection, int64_t timeout);
void Iron_httpspendingconnection_close(Iron_HttpsPendingConnection connection);
int64_t Iron_httpsserver_port(Iron_HttpsServer server);
void Iron_httpsserver_close(Iron_HttpsServer server);
Iron_HttpRequest Iron_httpsconnection_read_request(Iron_HttpsConnection connection,
                                                    int64_t max_header_bytes,
                                                    int64_t max_body_bytes,
                                                    int64_t timeout);
int64_t Iron_httpsconnection_send_response(Iron_HttpsConnection connection,
                                            Iron_HttpResponse response,
                                            int64_t timeout);
int64_t Iron_httpsconnection_send_response_keep_alive(
    Iron_HttpsConnection connection, Iron_HttpResponse response,
    bool keep_alive, int64_t timeout);
void Iron_httpsconnection_close(Iron_HttpsConnection connection);

Iron_HttpResponse Iron_http_request(Iron_String method, Iron_String url,
                                     Iron_String headers, Iron_String body,
                                     int64_t max_body_bytes, int64_t timeout);
Iron_HttpResponse Iron_http_request_with_ca(
    Iron_String method, Iron_String url, Iron_String headers, Iron_String body,
    int64_t max_body_bytes, Iron_String ca_file, int64_t timeout);
Iron_HttpResponse Iron_http_request_insecure(
    Iron_String method, Iron_String url, Iron_String headers, Iron_String body,
    int64_t max_body_bytes, int64_t timeout);
Iron_HttpResponse Iron_http_get(Iron_String url, int64_t timeout);
Iron_HttpResponse Iron_http_post_json(Iron_String url, Iron_String body,
                                       int64_t timeout);
Iron_HttpResponse Iron_http_get_with_ca(Iron_String url, Iron_String ca_file,
                                        int64_t timeout);
Iron_HttpResponse Iron_http_get_insecure(Iron_String url, int64_t timeout);

Iron_HttpClientResult Iron_httpclient_open(
    Iron_String origin, Iron_String ca_file, bool insecure,
    int64_t max_connections, int64_t idle_timeout);
Iron_HttpResponse Iron_httpclient_request(
    Iron_HttpClient client, Iron_String method, Iron_String target,
    Iron_String headers, Iron_String body, int64_t max_body_bytes,
    int64_t timeout);
void Iron_httpclient_close(Iron_HttpClient client);

Iron_HttpResponse Iron_http_response(int64_t status, Iron_String headers,
                                      Iron_String body);
Iron_HttpResponse Iron_http_json_response(int64_t status, Iron_String body);
Iron_HttpResponse Iron_http_html_response(int64_t status, Iron_String body);
Iron_HttpResponse Iron_http_text_response(int64_t status, Iron_String body);
Iron_HttpResponse Iron_http_file_response(int64_t status, Iron_String path,
                                           Iron_String content_type,
                                           int64_t max_body_bytes);
Iron_String Iron_http_header(Iron_String headers, Iron_String name);

#endif
