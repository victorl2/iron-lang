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

The same material is published as the practical guide at
[ironlang.dev/networking](https://ironlang.dev/networking/).

Timeouts are integer milliseconds. A timeout is a single monotonic budget for
the complete operation, including connect, TLS handshake, all partial writes,
header parsing, and body/frame decoding. Transport errors use codes
`1000..1999`, TLS uses `3000..3099`, HTTP uses `5000..5099`, WebSocket uses
`6000..6099`, and file I/O uses `8000..8099`.

## Result ownership

HTTP, WebSocket, and explicit file operations return model values that own
their string fields. Consume those strings with the matching `release` call
after the last read, on both success and error paths:

- `HttpServerResult.release`, `HttpConnectionResult.release`,
  `HttpsServerResult.release`, `HttpsConnectionResult.release`,
  `HttpsPendingConnectionResult.release`, and `HttpClientResult.release`
- `HttpRequest.release` and `HttpResponse.release`
- `WebSocketResult.release` and `WebSocketMessage.release`
- `FileReadResult.release`, `FileWriteResult.release`, and `FileInfo.release`

Resource handles have a separate lifetime: close or transfer the server,
connection, client, or socket first, then release its result model. Response
constructors clone their header and body inputs, so releasing a response never
invalidates the caller's strings. `Http.header` and other standalone dynamic
strings can be consumed with `value.release()`.

Release is consuming. Iron strings and models are value types, so do not use a
released value or a by-value alias afterwards. Inline strings and interned
literals are safe no-ops. C callers use `iron_string_release` and the
`Iron_*_release` functions declared in the corresponding public headers; the
same no-alias-after-release rule applies. Low-level `iron_tls.h` result structs
contain only resource handles and numeric `Iron_NetError` values, so they own
no strings and need no result release; close or free any successful handle.

## Client

```iron
import http

func main() {
    val response = Http.get("http://127.0.0.1:8080/api/status", 5000)
    if response.error != 0 {
        println("request failed: {response.error} {response.error_message}")
    } else {
        println("status={response.status}")
        println(response.body)
    }
    HttpResponse.release(response)
}
```

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
bounded general form. User headers are raw `Name: value` lines separated by
CRLF. `Host`, `Connection`, `Content-Length`, and `Transfer-Encoding` are
owned by the client and rejected in the user block to avoid ambiguous message
framing.

## Server, REST, and webpages

The server lifecycle is explicit:

```text
Http.listen -> HttpServer.accept -> HttpConnection.read_request
            -> HttpConnection.send_response -> HttpConnection.close
```

`HttpRequest` exposes `method`, `target`, `path`, `query`, `version`, raw
`headers`, and `body`. `Http.header` performs case-insensitive header lookup.
REST and page responses can be built with:

- `Http.json_response(status, body)`
- `Http.html_response(status, body)`
- `Http.text_response(status, body)`
- `Http.file_response(status, path, content_type, max_body_bytes)`
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
development escape hatches. They must not be used with production credentials.
Serve HTTPS with
`Http.listen_tls(host, port, certificate_chain_pem, private_key_pem)`, then use
the `HttpsServer` and `HttpsConnection` methods. See
[https_server.iron](../examples/networking/https_server.iron).
The matching private-CA REST client is
[https_client.iron](../examples/networking/https_client.iron).

Production HTTPS and WSS accept loops should separate TCP admission from TLS:

```iron
val pending = HttpsServer.accept_tcp(server, 60000)
if pending.error == 0 {
    val connection = pending.connection
    HttpsPendingConnectionResult.release(pending)
    spawn("tls-client") {
        val secure = HttpsPendingConnection.handshake(connection, 5000)
        val outcome = secure.error
        if secure.error == 0 {
            -- read HTTP or upgrade to WebSocket here
            HttpsConnection.close(secure.connection)
        }
        HttpsConnectionResult.release(secure)
        return outcome
    }
} else {
    HttpsPendingConnectionResult.release(pending)
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
    WebSocketMessage.release(message)
}
WebSocketResult.release(connected)
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
FileWriteResult.release(written)
FileReadResult.release(loaded)
FileInfo.release(info)
FileWriteResult.release(appended)
FileWriteResult.release(copied)
FileWriteResult.release(moved)
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
    HttpClientResult.release(opened)
    return
}

val first = HttpClient.request(
    opened.client, "GET", "/api/status", "", "", 1048576, 5000)
val second = HttpClient.request(
    opened.client, "GET", "/api/items", "", "", 1048576, 5000)
HttpClient.close(opened.client)
HttpResponse.release(first)
HttpResponse.release(second)
HttpClientResult.release(opened)
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
        HttpResponse.release(response)
        if sent != 0 { running = false }
        if sent == 0 { running = keep }
    }
    HttpRequest.release(request)
}
HttpConnection.close(connection)
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

## UDP packets

`UdpSocket.recvfrom(socket, max_bytes, timeout)` returns a `UdpPacket` with a
binary-safe `data` string and the sender's numeric `address` and `port`.
Datagram boundaries are preserved. If a datagram exceeds `max_bytes`, the
captured prefix is returned with `truncated == 1` and error code `1018`. A
successful zero-length datagram has empty data and error code `0`; a timeout
has code `1004`.

```iron
val packet = UdpSocket.recvfrom(socket, 65536, 1000)
if packet.error.code == 0 {
    println("{packet.address}:{packet.port} {packet.data}")
}
```

## Verified status

The following matrix is maintained from remote Linux x86_64 runs on
`silvaserver.local` (`iron-lsp-build:latest`, 8 GiB cap). The image is built
from `tools/containers/iron-lsp-build.Containerfile` and includes the Clang 14
ASan/UBSan/TSan runtimes plus OpenSSL development files. It was last validated
on 2026-08-28 in Debug, Release, and instrumented builds with the secure
backend. "Passing" means the
feature worked without an observed defect in the tested scope; limitations
are listed and linked immediately below the matrix.

| Capability | Status | Evidence |
|---|---|---|
| TCP loopback, payload reads, timeout, dual stack | Passing | `test_stdlib_net_tcp`, pure-Iron payload roundtrip fixtures |
| UDP binary payload, sender, truncation, zero-length packet, timeout | Passing | `test_stdlib_net_udp`, pure-Iron UDP payload roundtrip |
| IPv4/IPv6 parse and format | Passing | `test_stdlib_net_ip` |
| Concurrent DNS lookup | Passing | `test_stdlib_net_dns` |
| Cross-platform net battery | Passing | `test_stdlib_net_cross_platform` |
| HTTP models, header lookup, and HEAD semantics | Passing | `test_stdlib_http` |
| HTTP client/server webpage | Passing | C model test and live Python HTTP client against the compiled Iron example |
| REST JSON GET/POST | Passing | C roundtrip and live compiled Iron server smoke |
| Chunked response decoding | Passing | `test_http_client_decodes_chunked_response` |
| Chunked request decoding | Passing | decoded body and bounded-trailer parser tests |
| Concurrent native clients | Passing | 32 simultaneous clients; the complete test passed 100/100 repeated runs |
| Iron `spawn`/`await` HTTP composition | Passing | 8 accepting server tasks + 8 client tasks; passed 20/20 repeated runs |
| Detached server handlers | Passing | standalone `spawn` is intentional fire-and-forget; example checks without diagnostics |
| HTTPS verified client and TLS server | Passing | custom CA success, self-signed rejection, hostname mismatch rejection, explicit insecure mode, stalled-client admission, curl interoperability |
| WSS verified client/server upgrade | Passing | C roundtrip plus dependency-free Python TLS/WebSocket interoperability |
| WebSocket frames and control flow | Passing | masking rules, 7/16/64-bit lengths, binary/text, fragmentation with interleaved ping, pong, close, UTF-8 and protocol rejection |
| Concurrent WebSocket writers | Passing | 24 simultaneous writers serialized into valid frames with one receiver over both WS and verified WSS |
| Text and binary file operations | Passing | NUL/high-byte roundtrip, bounded read rejection, append, metadata, copy, move, overwrite refusal, pure-Iron fixture/example |

The Debug and optimized Release builds each passed all 14 focused networking,
file, and example checks.
The latest stress validation passed 30 complete WebSocket rounds (including 24
concurrent writers each), 30 TLS/WSS rounds (including a concurrent reader and
24 secure writers each), 100 HTTP model rounds (32 clients each), 100 native
file-operation rounds, 20 complete Iron HTTP concurrency rounds (eight clients
plus eight server tasks per round), 20 compiled Iron WebSocket rounds, and 20
compiled Iron binary-file rounds. Earlier validation also passed 50 UDP rounds. A
live compiled Iron server also returned the expected HTML page, JSON status
document, and JSON POST response.

The reproducible remote image can be rebuilt with
`podman build -f tools/containers/iron-lsp-build.Containerfile -t localhost/iron-lsp-build:latest .`.
In that image, the focused runtime-thread, HTTP, HTTPS/TLS, WebSocket/WSS,
concurrent Iron client/server, and text/binary-file battery passed 8/8 under
ASan/UBSan with LeakSanitizer enabled and 8/8 under TSan. All eight practical
networking examples also passed `ironc check` and native Release compilation.

## Known gaps

- Native Windows remains outside Iron's supported compiler matrix; Windows
  users should use WSL. Secure networking CI covers both supported native
  platforms (Linux and macOS).

Completed follow-ups: intentional detached spawn semantics
([#84](https://github.com/victorl2/iron-lang/issues/84)), payload-returning
binary-safe TCP reads ([#85](https://github.com/victorl2/iron-lang/issues/85)),
and bounded chunked server request decoding
([#88](https://github.com/victorl2/iron-lang/issues/88)), and payload-returning
UDP receive ([#90](https://github.com/victorl2/iron-lang/issues/90)).
Result-model ownership and strict LeakSanitizer coverage are completed in
[#99](https://github.com/victorl2/iron-lang/issues/99).
