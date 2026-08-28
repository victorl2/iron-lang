#ifndef IRON_WEBSOCKET_H
#define IRON_WEBSOCKET_H

#include <stdbool.h>
#include <stdint.h>

#include "iron_http.h"

/* Layout-compatible with src/stdlib/websocket.iron. The handle is an opaque
 * native session pointer. A successful HTTP upgrade transfers ownership of
 * the connection to the returned WebSocket. */
typedef struct Iron_WebSocket { int64_t handle; } Iron_WebSocket;

typedef struct Iron_WebSocketResult {
    Iron_WebSocket socket;
    int64_t error;
    Iron_String error_message;
} Iron_WebSocketResult;

typedef struct Iron_WebSocketMessage {
    int64_t kind;
    Iron_String data;
    int64_t close_code;
    int64_t error;
    Iron_String error_message;
} Iron_WebSocketMessage;

enum {
    IRON_WEBSOCKET_TEXT = 1,
    IRON_WEBSOCKET_BINARY = 2,
    IRON_WEBSOCKET_CLOSE = 8,
    IRON_WEBSOCKET_PING = 9,
    IRON_WEBSOCKET_PONG = 10
};

Iron_WebSocketResult Iron_websocket_connect(Iron_String url,
                                             Iron_String headers,
                                             int64_t max_message_bytes,
                                             int64_t timeout);
Iron_WebSocketResult Iron_websocket_connect_with_ca(Iron_String url,
                                                     Iron_String headers,
                                                     Iron_String ca_file,
                                                     int64_t max_message_bytes,
                                                     int64_t timeout);
Iron_WebSocketResult Iron_websocket_connect_insecure(Iron_String url,
                                                      Iron_String headers,
                                                      int64_t max_message_bytes,
                                                      int64_t timeout);

Iron_WebSocketResult Iron_httpconnection_upgrade_websocket(
    Iron_HttpConnection connection, Iron_HttpRequest request,
    int64_t max_message_bytes, int64_t timeout);
Iron_WebSocketResult Iron_httpsconnection_upgrade_websocket(
    Iron_HttpsConnection connection, Iron_HttpRequest request,
    int64_t max_message_bytes, int64_t timeout);

int64_t Iron_websocket_send_text(Iron_WebSocket socket, Iron_String data,
                                  int64_t timeout);
int64_t Iron_websocket_send_bytes(Iron_WebSocket socket, Iron_String data,
                                   int64_t timeout);
int64_t Iron_websocket_ping(Iron_WebSocket socket, Iron_String data,
                            int64_t timeout);
Iron_WebSocketMessage Iron_websocket_receive(Iron_WebSocket socket,
                                              int64_t timeout);
int64_t Iron_websocket_close(Iron_WebSocket socket, int64_t code,
                              Iron_String reason, int64_t timeout);
void Iron_websocket_abort(Iron_WebSocket socket);
bool Iron_websocket_is_open(Iron_WebSocket socket);

#endif
