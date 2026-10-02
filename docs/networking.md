<!-- doctest-imports: http websocket net io -->
# Networking

Iron ships synchronous, deadline-bounded networking layers:

- `import net` provides TCP, UDP, IP address parsing, and DNS.
- `import http` provides bounded HTTP/1.1 client and server models on top of
  TCP. It is designed to compose with `spawn` and `await`; the HTTP layer does
  not hide a scheduler or create handler threads by itself.
- `import websocket` provides RFC 6455 `ws://` and verified `wss://` clients,
  HTTP/HTTPS server upgrades, text and binary messages, fragmentation,
  ping/pong, and the closing handshake.
- `import io` provides the bounded binary-safe file operations commonly needed
  by network services: read, write, append, metadata, copy, and move.

This guide is published at
[ironlang.dev/networking](https://ironlang.dev/networking/), generated from
this file; every Iron example in it is compiled in CI. The runnable programs
live in [examples/networking](../examples/networking/).

Timeouts are integer milliseconds. A timeout is a single monotonic budget for
the complete operation, including connect, TLS handshake, all partial writes,
header parsing, and body/frame decoding. Transport errors use codes
`1000..1999`, TLS uses `3000..3099`, HTTP uses `5000..5099`, WebSocket uses
`6000..6099`, and file I/O uses `8000..8099`.

## Your first HTTP client

```iron
import http

func main() {
    val response = Http.get("http://127.0.0.1:8080/api/status", 5000)
    if response.error != 0 {
        println("request failed: {response.error} {response.error_message}")
    }
    if response.error == 0 {
        println("HTTP {response.status} {response.reason}")
        println(response.body)
    }
}
```

Build and run it against any local service:

```sh
iron build status_client.iron -o status-client
./status-client
```

Reading the result:

- `response.error != 0` means URL parsing, TCP, timeout, limit, or HTTP
  framing failed; `response.error_message` says which.
- `response.status` is meaningful only when `error == 0`. A 404 or 500 is a
  valid HTTP response, not a transport error.
- The timeout is one budget for the whole call: connect, writes, headers and
  body decoding.

## Result ownership

HTTP, WebSocket, and explicit file operations return model values that own
their string fields. Those strings are freed with the value that holds them,
like any other Iron string: nothing needs to be released by hand.

The `release` helpers (`HttpRequest.release`, `HttpResponse.release`,
`WebSocketMessage.release`, `FileReadResult.release`, `value.release()` on a
string, and the other `*Result.release` functions) remain for source
compatibility and do nothing.

Resource handles have a separate lifetime: close or transfer the server,
connection, client, or socket when you are done with it. Response
constructors clone their header and body inputs. Low-level `iron_tls.h`
result structs contain only resource handles and numeric `Iron_NetError`
values.

## Client requests

For JSON POST requests, use `Http.post_json`. Literal braces must be escaped
because unescaped `{...}` is Iron string interpolation:

```iron
val response = Http.post_json(
    "http://127.0.0.1:8080/api/items",
    "\{\"name\":\"anvil\"\}",
    5000,
)
```

`Http.request(method, url, headers, body, max_body_bytes, timeout)` is the
bounded general form:

```iron
val response = Http.request(
    "PUT",
    "http://127.0.0.1:8080/api/items/42",
    "Authorization: Bearer dev-token\r\nContent-Type: application/json",
    "\{\"name\":\"hammer\"\}",
    1048576,  -- maximum decoded response body
    5000,     -- complete request budget
)
```

User headers are raw `Name: value` lines separated by CRLF. `Host`,
`Connection`, `Content-Length`, and `Transfer-Encoding` are owned by the
client and rejected in the user block to avoid ambiguous message framing.

## Server, REST, and webpages

The server lifecycle is explicit:

```text
Http.listen -> HttpServer.accept -> HttpConnection.read_request
            -> HttpConnection.send_response -> HttpConnection.close
```

A handler reads one bounded request, answers it and closes the connection;
the server spawns one handler per accepted connection:

```iron
import http

func handle(connection: HttpConnection) -> Int {
    val request = HttpConnection.read_request(connection, 65536, 1048576, 5000)
    if request.error != 0 {
        val bad = Http.text_response(400, "bad request")
        val sent = HttpConnection.send_response(connection, bad, 5000)
        HttpConnection.close(connection)
        return sent
    }
    if request.method == "GET" and request.path == "/" {
        val page = Http.html_response(200, "<h1>Hello from Iron</h1>")
        val sent = HttpConnection.send_response(connection, page, 5000)
        HttpConnection.close(connection)
        return sent
    }
    val missing = Http.json_response(404, "\{\"error\":\"not found\"\}")
    val sent = HttpConnection.send_response(connection, missing, 5000)
    HttpConnection.close(connection)
    return sent
}

func main() {
    val listening = Http.listen("127.0.0.1", 8080)
    if listening.error != 0 {
        println("listen failed: {listening.error}")
        return
    }
    var served = 0
    while served < 100 {
        val accepted = HttpServer.accept(listening.server, 60000)
        if accepted.error == 0 {
            spawn("connection") {
                return handle(accepted.connection)
            }
            served += 1
        }
    }
    HttpServer.close(listening.server)
}
```

`HttpRequest` exposes the parsed request:

| Field | Meaning |
|---|---|
| `method` | Validated HTTP method token such as `GET` or `POST`. |
| `target` | The original path and query. |
| `path` | The path without the query string. |
| `query` | The raw query text without the leading question mark. |
| `version` | `HTTP/1.1`. |
| `headers` | Validated CRLF-separated header lines; `Http.header(headers, name)` looks one up case-insensitively. |
| `body` | The decoded fixed-length or chunked body, bounded by `max_body_bytes`. |

REST and page responses are built with:

- `Http.json_response(status, body)`
- `Http.html_response(status, body)`
- `Http.text_response(status, body)`
- `Http.file_response(status, path, content_type, max_body_bytes)`, which
  reads at most the given limit; the content type is given explicitly, Iron
  does not guess it from the file name
- `Http.response(status, headers, body)`

See [rest_server.iron](../examples/networking/rest_server.iron) for a runnable
webpage plus `GET /api/status` and `POST /api/items` service.

## HTTPS

`Http.get`, `Http.request`, and `Http.post_json` accept both `http://` and
`https://`. HTTPS verifies the certificate chain and hostname or IP address,
uses system trust roots, sends SNI for DNS names, requires TLS 1.2 or newer,
and shares one deadline across TCP connect, TLS handshake, request, and
response.

The source build enables the secure backend when CMake finds the OpenSSL
development headers and libraries. Without them, HTTP/WS and all plain socket
features still work; HTTPS/WSS calls return typed error `3000`
(`IRON_ERR_TLS_UNAVAILABLE`) instead of silently using plaintext.
Linux and macOS CI install OpenSSL explicitly, require the verified TLS test,
and compile an HTTPS program with a cleanly installed `ironc`. CMake passes the
exact include and library paths it validated to that compiler, including
Homebrew's keg-only OpenSSL location.

```iron
val response = Http.get("https://example.com/api/status", 5000)
```

Private PKI and local development certificates can be supplied explicitly for
GET or any custom REST method:

```iron
val response = Http.get_with_ca(
    "https://localhost:8443/",
    "cert.pem",
    5000,
)

val created = Http.request_with_ca(
    "POST",
    "https://localhost:8443/api/items",
    "Content-Type: application/json",
    "\{\"name\":\"anvil\"\}",
    1048576,
    "cert.pem",
    5000,
)
```

`Http.get_insecure` and `Http.request_insecure` are explicitly unsafe
development escape hatches for local development only. They must not be used
with production credentials.
Serve HTTPS with
`Http.listen_tls(host, port, certificate_chain_pem, private_key_pem)`, then use
the `HttpsServer` and `HttpsConnection` methods. See
[https_server.iron](../examples/networking/https_server.iron).
The matching private-CA REST client is
[https_client.iron](../examples/networking/https_client.iron).

Production HTTPS and WSS accept loops should separate TCP admission from TLS:

```iron
func accept_one(server: HttpsServer) {
    val pending = HttpsServer.accept_tcp(server, 60000)
    if pending.error == 0 {
        val connection = pending.connection
        spawn("tls-client") {
            val secure = HttpsPendingConnection.handshake(connection, 5000)
            val outcome = secure.error
            if secure.error == 0 {
                -- read HTTP or upgrade to WebSocket here
                HttpsConnection.close(secure.connection)
            }
            return outcome
        }
    }
}
```

The accept deadline applies only while waiting for a TCP client. Each spawned
handler has an independent handshake deadline, so a raw client that sends no
TLS ClientHello cannot hold up later clients. `handshake` consumes the pending
connection on success or failure; call `HttpsPendingConnection.close` instead
when abandoning it. Pending and established connections safely retain the TLS
certificate context while an old listener is being drained. The one-step
`HttpsServer.accept` API remains available for simple, controlled servers.

The certificate and key are loaded into the server context at listen time.
There is no in-place hot reload: rotate certificates by starting a new server
context (or restarting the service) with the replacement files, then drain the
old listener. For graceful shutdown, stop scheduling accepts, close the
`HttpServer`/`HttpsServer` to wake or prevent new work, await all bound handler
tasks, and close their remaining connections. Detached handlers cannot be
awaited, so production servers that require draining should retain task handles.

## WebSocket and secure WebSocket

Connect with a message allocation limit and one deadline:

```iron
import websocket

val connected = WebSocket.connect(
    "wss://events.example.com/v1",
    "Authorization: Bearer token",
    1048576,
    5000,
)
if connected.error == 0 {
    val sent = WebSocket.send_text(connected.socket, "subscribe", 5000)
    val message = WebSocket.receive(connected.socket, 30000)
    if message.error == 0 and message.kind == 1 {
        println(message.data)
    }
    val closed = WebSocket.close(connected.socket, 1000, "done", 5000)
}
```

Use `connect_with_ca` for a private root. The explicitly named
`connect_insecure` variant is development-only. `WebSocketMessage.kind` is
`1` for text, `2` for binary, `8` for close, `9` for ping, and `10` for pong.
Ping is answered automatically before it is returned. Fragmented messages are
reassembled and checked against `max_message_bytes`; text and close reasons
are UTF-8 validated.

Services such as GraphQL or MQTT can request subprotocols in preference order
without overriding handshake-owned headers:

```iron
val connected = WebSocket.connect_with_protocols(
    "wss://events.example.com/graphql",
    "Authorization: Bearer token",
    ["graphql-transport-ws", "graphql-ws"],
    1048576,
    5000,
)
if connected.error == 0 {
    println("selected {connected.protocol}")
}
```

Servers use `upgrade_websocket_protocol(..., selected_protocol, ...)` with
exactly one protocol offered by the request, or use the original upgrade call
to select none. The client rejects unsolicited or multiple server selections.
Raw `Sec-WebSocket-Protocol` and other handshake-owned header overrides remain
forbidden. Extension offers are never accepted silently: Iron currently emits
no `Sec-WebSocket-Extensions` response and rejects any extension selected by a
peer because no extension frame semantics are implemented.

On the server, first read the HTTP request and then transfer connection
ownership with `HttpConnection.upgrade_websocket` or
`HttpsConnection.upgrade_websocket`. A successful upgrade owns the original
connection, so do not close it separately. See
[websocket_echo_server.iron](../examples/networking/websocket_echo_server.iron)
and
[websocket_echo_secure_server.iron](../examples/networking/websocket_echo_secure_server.iron).

One task may receive from a socket while multiple tasks send; complete frame
writes are serialized. There must be only one receiving task, and close/abort
must not race other operations.

## Text and binary file operations

Iron `String` values preserve embedded zero and arbitrary bytes. The byte APIs
therefore use `String` without losing binary data:

```iron
import io

val written = IO.write_bytes("asset.bin", "Iron\0binary")
val loaded = IO.read_bytes("asset.bin", 1048576)
val info = IO.file_info("asset.bin")
if loaded.error == 0 {
    println("loaded {info.size} bytes")
}

val appended = IO.append_bytes("asset.bin", "\0suffix")
val copied = IO.copy_file("asset.bin", "asset-copy.bin", false)
val moved = IO.move_file("asset-copy.bin", "archive.bin", false)
```

Available result-returning operations are `read_text`, `read_bytes`,
`write_text`, `write_bytes`, `append_text`, `append_bytes`, `file_info`,
`copy_file`, and `move_file`. Reads reject a file larger than the caller's
limit before allocating. Writes report the committed byte count and surface
open, write, flush, and close errors. Copy/move take an explicit `overwrite`
flag. See [file_operations.iron](../examples/networking/file_operations.iron).
With `overwrite=false`, move commits the destination atomically: if another
task or process creates that path first, the move returns
`IRON_ERR_IO_ALREADY_EXISTS` and preserves both the source and the winning
destination. POSIX filesystems use an atomic hard-link commit (including after
a cross-device temporary copy); Windows uses `MoveFileEx` without replacement.
Filesystems that cannot provide that primitive return an error instead of
falling back to a racy check-then-rename.

## Persistent HTTP connections

One-shot calls such as `Http.get` remain the simplest safe option and close
their connection. For chatty same-origin traffic, an explicit `HttpClient`
owns a bounded pool:

```iron
val opened = HttpClient.open(
    "https://api.example.com", "", false, 4, 30000)
if opened.error != 0 {
    return
}

val first = HttpClient.request(
    opened.client, "GET", "/api/status", "", "", 1048576, 5000)
val second = HttpClient.request(
    opened.client, "GET", "/api/items", "", "", 1048576, 5000)
HttpClient.close(opened.client)
```

The origin fixes scheme, host, port, certificate roots, and verification mode;
it must not contain a path, query, or fragment (an optional trailing slash is
accepted). TLS-only CA and insecure options are rejected for plain HTTP rather
than ignored. Requests accept only origin-form targets beginning with `/`.
`max_connections` bounds concurrent sockets and `idle_timeout` retires old idle entries. A stale
reused connection is retried once only for bodyless GET or HEAD. POST and other
potentially non-idempotent requests are never replayed automatically.

Servers opt into persistence per response. Use `request.keep_alive` (which
implements HTTP/1.1 and HTTP/1.0 Connection-token rules), an idle timeout on
each next `read_request`, and an application request-count limit:

```iron
func serve_session(connection: HttpConnection) {
    var served = 0
    var running = true
    while running and served < 100 {
        val request = HttpConnection.read_request(connection, 16384, 1048576, 15000)
        if request.error != 0 { running = false }
        if request.error == 0 {
            served += 1
            val keep = request.keep_alive and served < 100
            val response = Http.text_response(200, "ok")
            val sent = HttpConnection.send_response_keep_alive(
                connection, response, keep, 5000)
            if sent != 0 { running = false }
            if sent == 0 { running = keep }
        }
    }
    HttpConnection.close(connection)
}
```

Every persistent response uses `Content-Length`, so the next message boundary
is unambiguous. Pipelining is intentionally unsupported: send the next request
only after reading the prior response. For graceful shutdown, close the
listener, stop spawning sessions, let bounded handlers finish, then close the
client/session handles. See
[http_keep_alive.iron](../examples/networking/http_keep_alive.iron).

## Framing and limits

- Server requests require HTTP/1.1 and exactly one `Host` header.
- Server request bodies accept one unambiguous `Content-Length` or bounded
  chunked transfer encoding, including validated bounded trailers.
- Client responses accept HTTP/1.0 and HTTP/1.1, `Content-Length`, chunked
  transfer encoding (including trailers), or close-delimited bodies.
- Duplicate framing headers, obsolete folded headers, control characters,
  malformed header names, and CRLF injection are rejected.
- Server header and body limits are supplied to `read_request`; client body
  limits are supplied to `request`. Convenience client calls use an 8 MiB body
  limit and a 64 KiB header limit.
- One-shot calls send `Connection: close`; explicit sessions use framed
  sequential HTTP persistence without pipelining.

## TCP

`Net.tcp_dial` connects with a deadline; `TcpSocket.read` returns up to the
caller's limit as a binary-safe `String` (embedded zero bytes are preserved):

```iron
import net

val (socket, dial_error) = Net.tcp_dial("127.0.0.1", 9000, 2000)
if dial_error.code != 0 { return }

val (written, write_error) = TcpSocket.write(socket, "ping", 1000)
val (payload, read_error) = TcpSocket.read(socket, 65536, 1000)
if read_error.code == 0 {
    println(payload)
}
TcpSocket.close(socket)
```

TCP is a byte stream: one write does not guarantee one read. Loop until the
protocol's delimiter or expected length is complete, and treat an empty
successful read as end of stream.

## UDP

Bind datagram sockets, send to typed IPv4 or IPv6 addresses, and receive a
bounded `UdpPacket` with the binary-safe payload and the sender's numeric
address and port:

```iron
import net

val (receiver, bind_error) = Net.udp_bind("127.0.0.1", 5353)
val (sender, sender_error) = Net.udp_bind("127.0.0.1", 0)
val (target, parse_error) = IPv4Addr.parse("127.0.0.1")
val (sent, send_error) = Net.udp_sendto_v4(sender, "ping", target, 5353, 1000)

val packet = UdpSocket.recvfrom(receiver, 65536, 1000)
if packet.error.code == 0 {
    println("{packet.address}:{packet.port} {packet.data}")
}
UdpSocket.close(sender)
UdpSocket.close(receiver)
```

Datagram boundaries are preserved: one successful receive consumes one
packet. If a datagram exceeds `max_bytes`, the captured prefix is returned
with `truncated == 1` and error code `1018`. A successful zero-length
datagram has empty data and error code `0`; a timeout has code `1004`.

## DNS and addresses

```iron
val (addresses, error) = Net.lookup_host("example.com", 2000)
if error.code != 0 {
    println("DNS failed: {error.code}")
    return
}

for address in addresses {
    match address {
        Address.V4(v4) -> println(IPv4Addr.format(v4))
        Address.V6(v6) -> println(IPv6Addr.format(v6))
    }
}
```

`IPv4Addr.parse`, `IPv6Addr.parse` and the matching `format` functions
convert between text and the typed addresses the socket calls take.

## Concurrency

A standalone `spawn` statement is detached: the handler runs to completion
on its own. Bind it when the parent must await completion or collect a
result:

```iron
val url1 = "http://127.0.0.1:8080/api/status"
val url2 = "http://127.0.0.1:8080/api/items"
val first = spawn("client-1") { return Http.get(url1, 5000) }
val second = spawn("client-2") { return Http.get(url2, 5000) }
val a = await first
val b = await second
println("{a.status} {b.status}")
```

Every accepted connection has an owner and must be closed on every handler
path. A slow connection occupies its handler until its deadline expires, so
give every public endpoint bounded header and body sizes and finite
timeouts. One task may receive from a WebSocket while several tasks send
(complete frames are serialized); there must be only one receiver.

## API summary

| API | Use |
|---|---|
| `Http.get(url, timeout)` | Bounded GET. |
| `Http.post_json(url, body, timeout)` | POST with a JSON content type. |
| `Http.request(method, url, headers, body, max_body_bytes, timeout)` | Any method, headers and body. |
| `Http.get_with_ca(...)`, `Http.request_with_ca(...)` | Verified HTTPS with a private trust root. |
| `Http.get_insecure(...)`, `Http.request_insecure(...)` | Unverified HTTPS, local development only. |
| `HttpClient.open(origin, ca, insecure, max_connections, idle_timeout)` | A bounded pool of persistent connections to one origin. |
| `Http.listen(host, port)`, `Http.listen_tls(host, port, chain, key)` | HTTP and HTTPS servers. |
| `HttpServer.accept(server, timeout)` | Accept one connection. |
| `HttpsServer.accept_tcp(server, timeout)`, `HttpsPendingConnection.handshake(pending, timeout)` | TCP admission separated from the TLS handshake. |
| `HttpConnection.read_request(connection, max_header_bytes, max_body_bytes, timeout)` | Parse one bounded HTTP/1.1 request. |
| `HttpConnection.send_response(connection, response, timeout)` | Send a complete response. |
| `Http.json_response`, `Http.html_response`, `Http.text_response`, `Http.file_response`, `Http.response` | Response constructors. |
| `Http.header(headers, name)` | Case-insensitive header lookup. |
| `WebSocket.connect(url, headers, max_message_bytes, timeout)` | A `ws://` or verified `wss://` client. |
| `HttpConnection.upgrade_websocket(connection, request, max_message_bytes, timeout)` | Validate an upgrade and take over the connection. |
| `WebSocket.send_text`, `WebSocket.send_bytes`, `WebSocket.receive`, `WebSocket.close` | Frames and the closing handshake. |
| `Net.tcp_dial`, `TcpSocket.read`, `TcpSocket.write`, `TcpSocket.close` | TCP. |
| `Net.udp_bind`, `Net.udp_sendto_v4`, `UdpSocket.recvfrom`, `UdpSocket.close` | UDP. |
| `Net.lookup_host`, `IPv4Addr.parse`, `IPv6Addr.parse` | DNS and addresses. |
| `IO.read_bytes`, `IO.write_bytes`, `IO.append_bytes`, `IO.file_info`, `IO.copy_file`, `IO.move_file` | Bounded binary file operations. |

## Production checklist

- Bind deliberately: loopback for local tools, a public interface only when
  intended.
- Set finite accept, header, body, handler and upstream timeouts.
- For public HTTPS and WSS listeners, use `accept_tcp` and run each bounded
  TLS handshake in its handler task.
- Set endpoint-specific body limits instead of the convenience maximum.
- Check both the transport error and the application status code.
- Close every accepted connection on every handler path.
- Use HTTPS and WSS, keep certificate verification on, protect private keys.
- Authentication, authorization, rate limiting, logging and graceful
  shutdown belong to the application.

## Verified status

Every example in this guide is compiled by the Doc Test workflow on every
change, with the pinned toolchain. The networking, TLS, WebSocket and file
suites (`test_stdlib_net_*`, `test_stdlib_http`, the chunked decoding,
concurrent client, HTTPS and WSS roundtrip tests, and the pure Iron
fixtures and examples under `examples/networking`) run in CI on Linux and
macOS in Debug (AddressSanitizer and UndefinedBehaviorSanitizer) and Release
builds, and under ThreadSanitizer. The suites also run on Windows, where the
compiler and generated programs build natively with the same pinned
toolchain; HTTPS there needs OpenSSL development files, as on the other
platforms (section "HTTPS").

Iron networking is alpha software: the APIs may evolve as production feedback
accumulates.
