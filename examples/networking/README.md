# Iron networking examples

Build and run the REST/page server from the repository root:

```sh
build/ironc build examples/networking/rest_server.iron -o rest_server
./rest_server
```

Then visit `http://127.0.0.1:8080/` or call:

```sh
curl http://127.0.0.1:8080/api/status
curl -X POST -H 'Content-Type: application/json' \
  --data '{"name":"anvil"}' http://127.0.0.1:8080/api/items
```

The server deliberately uses a standalone fire-and-forget `spawn` for each
accepted connection. Bind a spawn expression to a task handle when its result
must be awaited; leave it standalone for an intentionally detached handler.

The complete practical guide is published at
[ironlang.dev/networking](https://ironlang.dev/networking/).

Additional compiler-tested examples:

- `https_server.iron` serves an HTML page over TLS; `https_client.iron` makes
  a verified private-CA REST request.
- `http_keep_alive.iron` shows an explicit same-origin client pool and a
  server handler with idle and maximum-request bounds.
- `websocket_echo_server.iron` and `websocket_client.iron` demonstrate
  `ws://`, text/binary messages, and a graceful close.
- `websocket_echo_secure_server.iron` serves the same protocol over verified
  `wss://`.
- `file_operations.iron` demonstrates bounded, binary-safe read/write,
  metadata, copy, and move.
