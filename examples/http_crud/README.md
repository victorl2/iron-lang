# http_crud — client/server HTTP CRUD in pure Iron

A complete HTTP/1.1 client-server pair written entirely in Iron, with CRUD
persistence to a **raw binary file** (no JSON/text storage).

| file | role |
|---|---|
| `server.iron` | HTTP/1.1 server, sequential accept loop, binary-file store |
| `client.iron` | phase 1: full CRUD exercise incl. binary-payload roundtrip |
| `client_verify.iron` | phase 2: proves persistence across a server restart |

## Storage format (`records.bin`)

```
[4] magic "IRDB"    [1] version 0x01    [4] u32-BE live record count
per record: [4] u32-BE id
            [4] u32-BE name byte length, raw name bytes
            [4] u32-BE note byte length, raw note bytes
```

Payload bytes are stored verbatim: embedded NUL, CRLF and 0xFF survive the
full HTTP body → memory → file → memory → HTTP body pipeline. The store is
written with `IO.write_bytes` and read with `IO.read_bytes`.

## HTTP surface

```
POST   /records        body "name\nnote"  -> 201 "id\n"
GET    /records                           -> 200 "count\n" + "id|name\n" rows
GET    /records/{id}                      -> 200 "id\nname\nnote\n" | 404
PUT    /records/{id}   body "name\nnote"  -> 200 | 404
DELETE /records/{id}                      -> 204 | 404
GET    /shutdown                          -> 200 (stops the accept loop)
```

## Run

```sh
ironc build server.iron -o server && ironc build client.iron -o client
ironc build client_verify.iron -o client_verify
./server &          # loads records.bin if present
./client            # runs the CRUD suite, ends with GET /shutdown
./server &          # restart: reloads store from disk
./client_verify     # proves the store survived the restart
```

Both clients end by printing `ALL CHECKS PASSED` on success.

`e2e.sh [workdir]` does all of this and hex-dumps `records.bin` after each
phase (set `IRONC` to use a compiler that is not on `PATH`).

## Notes on the code

The server speaks HTTP over raw `net` sockets on purpose, to show the
socket layer. For a real service, the `http` module (`Http.listen`,
`HttpConnection.read_request`, `send_response`) does the framing for you;
see `examples/networking/rest_server.iron`.

- **Bytes, not characters.** String positions (`len`, `index_of`,
  `substring`) count characters, so code that handles payload bytes uses
  `byte_len`, `byte_at` and `byte_slice`, plus a small `byte_index_of`
  helper. With character positions, a payload holding 0xFF or multi-byte
  UTF-8 would be cut at the wrong place.
- **Carriage return.** Iron has no `\r` escape (an unknown escape keeps its
  backslash), so CRLF is written `\u{D}\n`.
- **Building strings.** Responses and the store image are collected as a
  `[String]` of parts and joined once with `"".join(parts)`.
- **Store state** is kept in module-level parallel lists with a tombstone
  flag, and lookups return `-1` when nothing matches.
