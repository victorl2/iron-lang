<!-- doctest-imports: mysql -->
# MySQL

`import mysql` is a client for MySQL 5.7 and 8.x and MariaDB 10.x and 11.x.
It speaks the MySQL client/server protocol itself, so programs need no
client library: the module brings TLS, the authentication plugins, the text
and binary protocols, prepared statements and a thread-safe connection
pool, and links the same OpenSSL as `http`.

This guide is published at [ironlang.dev/mysql](https://ironlang.dev/mysql/),
generated from this file; every Iron example in it is compiled in CI. The
module's declarations are in
[src/stdlib/mysql.iron](../src/stdlib/mysql.iron).

## Connecting

```iron
import mysql

func main() {
    var conn = Mysql.connect_url("mysql://app:secret@127.0.0.1:3306/shop")
    if conn.error.failed() {
        println("connect failed: {conn.error.code} {conn.error.message}")
        return
    }
    println("connected to {conn.server_version()}")
}
```

A connection is a `MysqlConn`. It owns a socket, so it is `nocopy` and is
closed (with `COM_QUIT`) when its binding goes out of scope; `close()` closes
it earlier. Bind it with `var`: the methods that talk to the server change
the connection's state. A function that uses a caller's connection takes
`conn: *var MysqlConn`, and the call passes the binding as is.

`Mysql.connect(config)` takes a `MysqlConfig`. Construct one with the host,
port, user, password and database, then adjust the rest:

```iron
func main() {
    var cfg = MysqlConfig("db.internal", 3306, "app", "secret", "shop")
    cfg.tls = MysqlTls.VerifyIdentity
    cfg.ca_file = "/etc/ssl/db-ca.pem"
    cfg.connect_timeout = 3000
    cfg.timeout = 5000
    var conn = Mysql.connect(cfg)
    if conn.ok() {
        println("TLS: {conn.is_tls()}, auth: {conn.auth_plugin()}")
    }
}
```

| Field | Default | Meaning |
|---|---|---|
| `tls` | `Preferred` | `Disabled`, `Preferred` (TLS when offered, unverified), `Required` (unverified), `VerifyCa` (chain), `VerifyIdentity` (chain and host name) |
| `ca_file` | `""` | PEM trust roots for verification; `""` uses the system's |
| `server_public_key` | `""` | PEM file with the server's RSA key, for the password exchange without TLS |
| `allow_public_key_retrieval` | `false` | fetch that key from the server when none is configured |
| `allow_cleartext_password` | `false` | allow `mysql_clear_password` without TLS |
| `collation` | `""` | connection collation; `""` is utf8mb4 with the server's default |
| `connect_timeout` | `10000` | milliseconds for TCP, TLS and authentication |
| `timeout` | `0` | milliseconds for each command's round trip; `0` is no limit |
| `multi_statements` | `false` | let a query string hold several statements |
| `max_allowed_packet` | 1 GB | the largest packet accepted from the server |
| `init_command` | `""` | SQL run on each new connection (and after a pool reset), such as `SET time_zone = '+00:00'`; a failure fails the connection |

`Mysql.parse_url(url)` returns `(MysqlConfig, MysqlError)` and
`Mysql.connect_url(url)` connects with it. The URL is
`mysql://user:password@host:port/database?option=value&...` (`mariadb://`
also works); user, password, database and values are percent-decoded, and
an IPv6 host goes in brackets. The options are the fields above:
`tls` (or `ssl-mode`) with `disabled`, `preferred`, `required`,
`verify_ca` or `verify_identity`; `ca`, `server_public_key`,
`allow_public_key_retrieval`, `allow_cleartext_password`, `collation`,
`connect_timeout`, `timeout`, `multi_statements`, `max_allowed_packet` and
`init_command`.

### Authentication

The client supports `caching_sha2_password` (the MySQL 8 default, both the
fast path and full authentication), `mysql_native_password`,
`sha256_password` and `mysql_clear_password`, and follows the server's auth
switch requests. Full authentication with `caching_sha2_password` or
`sha256_password` sends the password inside TLS, or, on a connection
without TLS, encrypted with the server's RSA public key (from
`server_public_key`, or fetched when `allow_public_key_retrieval` is set).
Without either, the client refuses to send the password and reports
`IRON_ERR_MYSQL_INSECURE_AUTH` (7013) rather than exposing it.

## Queries

`exec(sql, params)` runs SQL with `?` placeholders as a prepared statement
over the binary protocol. It is the safe way to include values from outside
the program, and the fast one: each connection prepares a statement once
and keeps it in a cache keyed by the SQL text (256 per connection, least
recently used first out), so a repeated query costs one round trip.
`query(sql)` runs SQL with the text protocol, and `run(sql)` runs SQL that
returns no rows and reports only the error.

```iron
func main() {
    var conn = Mysql.connect_url("mysql://app:secret@127.0.0.1:3306/shop")
    if conn.error.failed() {
        return
    }
    conn.run("CREATE TABLE IF NOT EXISTS users (id BIGINT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(100) NOT NULL, email VARCHAR(200), born DATE)")
    val ins = conn.exec("INSERT INTO users (name, email, born) VALUES (?, ?, ?)",
        [MysqlValue.Text("Ada"), MysqlValue.Null, MysqlValue.Date(Mysql.date(1815, 12, 10))])
    if ins.ok() {
        println("inserted id {ins.last_insert_id()}, {ins.affected_rows()} row")
    }
    val res = conn.exec("SELECT id, name, email, born FROM users WHERE name = ?", [MysqlValue.Text("Ada")])
    if res.failed() {
        println("query failed: {res.error.message}")
        return
    }
    var row = 0
    while row < res.row_count() {
        val email = res.text(row, res.col("email"))
        println("{res.int(row, 0)} {res.text(row, 1)} email={email} born={res.text(row, 3)}")
        row += 1
    }
}
```

### Parameters

A parameter is a `MysqlValue`:

| Variant | Sent as |
|---|---|
| `Null` | NULL |
| `Int(Int)`, `UInt(UInt)` | BIGINT, BIGINT UNSIGNED |
| `Float(Float)` | DOUBLE |
| `Decimal(String)` | DECIMAL, exactly as written (`"12.50"`) |
| `Text(String)`, `Bytes(String)` | character string, binary string |
| `Bool(Bool)` | TINYINT 0 or 1 |
| `Date(MysqlDateTime)`, `DateTime(MysqlDateTime)` | DATE, DATETIME (with microseconds) |
| `Time(MysqlTime)` | TIME (negative and over 24 hours allowed) |

`Mysql.date(y, m, d)` and `Mysql.datetime(y, m, d, h, mi, s)` build the date
values. Passing more or fewer parameters than the statement has
placeholders is `IRON_ERR_MYSQL_PARAM_COUNT` (7011).

### Reading results

Every result is read from the server before `exec` or `query` returns: the
connection is free for the next command at once, and a `MysqlResult` holds
its rows in one buffer until its binding goes out of scope. `row_count()`
and `column_count()` give its shape, `col(name)` finds a column by name or
label (case-insensitive; `-1` when missing), and the readers take a row and
a column index:

| Reader | Returns |
|---|---|
| `int`, `uint`, `float`, `bool` | `Int?`, `UInt?`, `Float?`, `Bool?` |
| `int_or`, `uint_or`, `float_or`, `text_or`, `bool_or` | the value, or the fallback given as the third argument |
| `text`, `bytes` | `String?`: any value as text (dates as `YYYY-MM-DD HH:MM:SS`), or the raw bytes |
| `datetime`, `time` | `MysqlDateTime?`, `MysqlTime?` |
| `value` | `MysqlValue` chosen from the column type |
| `is_null` | `Bool` |

The short readers return `null` for SQL NULL and for a value that does not
convert (text to `Int`, `1.5` to `Int`, a BIGINT UNSIGNED over the `Int`
range); check them before use (`v != null and v > 0`), or read with an
`_or` form when a default will do (`res.int_or(0, 1, 0)`). The `get_` forms (`get_int`, `get_text`, ...) return the value with
a `MysqlError` that says which: `IRON_ERR_MYSQL_NULL` (7005),
`IRON_ERR_MYSQL_CONVERSION` (7006) or `IRON_ERR_MYSQL_RANGE` (7007). The
text and binary protocols read the same way: `text` of a DOUBLE or a
DATETIME gives the same string whichever produced it.

```iron
func main() {
    var conn = Mysql.connect_url("mysql://app:secret@127.0.0.1:3306/shop")
    val res = conn.query("SELECT 41 + 1 AS answer, NULL AS nothing")
    val (answer, err) = res.get_int(0, res.col("answer"))
    if err.ok() {
        println("answer {answer}")
    }
    val (_, null_err) = res.get_int(0, 1)
    println("NULL is error {null_err.code}")
    match res.value(0, 0) {
        MysqlValue.Int(v) -> println("an Int: {v}")
        else -> println("something else")
    }
}
```

`column(i)` describes a column: `name`, `org_name`, `table`, `org_table`,
`schema`, the wire `type` and its SQL `type_name`, `flags`, `decimals`,
`length` and the `charset` collation id, with `is_unsigned()`,
`is_nullable()`, `is_primary_key()` and `is_binary()`. `affected_rows()`,
`last_insert_id()`, `warnings()` and `info()` report what a statement
changed.

### Several result sets

With `multi_statements` set, one `query` may hold several statements, and a
stored procedure returns a result set per SELECT it runs (through `exec`
too). The result starts at the first set; `next_result()` moves to the next
one and returns `false` after the last. An error stops the batch: the sets
before it stay readable and `error` describes the failure.

```iron
func main() {
    var cfg = MysqlConfig("127.0.0.1", 3306, "app", "secret", "shop")
    cfg.multi_statements = true
    var conn = Mysql.connect(cfg)
    var res = conn.query("SELECT 1 AS a; SELECT 'two' AS b")
    println("{res.result_count()} sets, first {res.int(0, 0)}")
    if res.next_result() {
        println("second {res.text(0, 0)}")
    }
}
```

### Prepared statements

`exec` is usually all a program needs. `prepare(sql)` returns a
`MysqlStmt` to manage a statement directly: `exec(params)`, `reset()`,
`close()`, `param_count()` and `column_count()`. A statement belongs to its
connection and must not outlive it; using one after its connection closed
or went back to its pool is `IRON_ERR_MYSQL_STMT_CLOSED` (7015).

### Escaping

Prefer parameters. When SQL text must be built, `conn.escape(s)` escapes a
string for a quoted literal following the session's `NO_BACKSLASH_ESCAPES`
mode, `conn.quote(s)` also adds the quotes, `Mysql.escape(s)` assumes
backslash escapes, and `Mysql.quote_identifier(name)` quotes a table or
column name in backticks.

## Transactions

`begin()`, `commit()` and `rollback()` return a `MysqlError`;
`in_transaction()` reports the server's view.

```iron
func transfer(conn: *var MysqlConn, from: Int, to: Int, amount: Int) -> MysqlError {
    val started = conn.begin()
    if started.failed() {
        return started
    }
    val debit = conn.exec("UPDATE accounts SET balance = balance - ? WHERE id = ? AND balance >= ?",
        [MysqlValue.Int(amount), MysqlValue.Int(from), MysqlValue.Int(amount)])
    if debit.failed() or debit.affected_rows() != 1 {
        conn.rollback()
        return debit.error
    }
    val credit = conn.exec("UPDATE accounts SET balance = balance + ? WHERE id = ?",
        [MysqlValue.Int(amount), MysqlValue.Int(to)])
    if credit.failed() {
        conn.rollback()
        return credit.error
    }
    return conn.commit()
}

func main() {
    var conn = Mysql.connect_url("mysql://app:secret@127.0.0.1:3306/bank")
    val err = transfer(conn, 1, 2, 50)
    println("transfer: {err.code}")
}
```

## Connection pools

A service that handles requests concurrently shares an `rc MysqlPool`. Any
number of tasks may use it at once; each borrows its own connection.

```iron
import http

func handle(db: rc MysqlPool, connection: HttpConnection) -> Int {
    val request = HttpConnection.read_request(connection, 65536, 1048576, 5000)
    var status = 404
    var body = "\{\"error\":\"not found\"\}"
    if request.error == 0 and request.path == "/user" {
        val id = request.query.replace("id=", "").to_int()
        val res = db.exec("SELECT name FROM users WHERE id = ?", [MysqlValue.Int(id)])
        if res.failed() {
            status = 500
            body = "\{\"error\":\"database\"\}"
        } elif res.row_count() == 1 {
            status = 200
            body = "\{\"name\":\"{res.text_or(0, 0, "")}\"\}"
        }
    }
    val response = Http.json_response(status, body)
    val sent = HttpConnection.send_response(connection, response, 5000)
    HttpConnection.close(connection)
    return sent
}

func main() {
    val (cfg, _) = Mysql.parse_url("mysql://app:secret@127.0.0.1:3306/shop")
    var options = MysqlPoolOptions(4, 32)
    options.acquire_timeout = 2000
    val db = Mysql.open_pool(cfg, options)
    if not db.ok() {
        println("pool: {db.error.message}")
        return
    }
    val listening = Http.listen("127.0.0.1", 8080)
    if listening.error != 0 {
        return
    }
    while true {
        val accepted = HttpServer.accept(listening.server, 60000)
        if accepted.error == 0 {
            val connection = accepted.connection
            spawn("request") {
                return handle(db, connection)
            }
        }
    }
}
```

`query`, `exec` and `run` on the pool borrow a connection for one statement.
`acquire()` hands out a `MysqlConn` for several statements, such as a
transaction; it goes back to the pool when its binding goes out of scope,
and a transaction still open then is rolled back.

```iron
func move_stock(db: rc MysqlPool, item: Int) -> Bool {
    var conn = db.acquire()
    if conn.error.failed() {
        return false
    }
    conn.begin()
    conn.exec("UPDATE stock SET qty = qty - 1 WHERE item = ?", [MysqlValue.Int(item)])
    conn.exec("INSERT INTO moves (item) VALUES (?)", [MysqlValue.Int(item)])
    return conn.commit().ok()
}

func main() {
    val db = Mysql.open_pool(MysqlConfig("127.0.0.1", 3306, "app", "secret", "shop"), MysqlPoolOptions(1, 8))
    println("{move_stock(db, 7)} size={db.size()} idle={db.idle()} in use={db.in_use()}")
}
```

`MysqlPoolOptions(min, max)` sets the size; `min` connections open with the
pool, so a bad configuration fails at `open_pool`. The other fields:

| Field | Default | Meaning |
|---|---|---|
| `acquire_timeout` | 10000 | wait this long for a free connection, then `IRON_ERR_MYSQL_POOL_TIMEOUT` (7009) |
| `idle_timeout` | 300000 | close connections idle this long, beyond `min` |
| `health_check` | 30000 | ping a connection idle this long before handing it out (0: never) |
| `max_lifetime` | 1800000 | replace connections older than this |
| `reset_on_release` | `false` | send `COM_RESET_CONNECTION` when a connection comes back (clears session state, and the statement cache) |

Before handing out an idle connection, the pool checks that the server has
not closed it (a `wait_timeout`, a restart, `KILL`): dead connections are
replaced without the caller seeing an error. `close()` closes idle
connections and refuses new work; connections in use close when they come
back. `size()`, `idle()` and `in_use()` report the counts.

## Errors

Every operation returns a `MysqlError`, directly or in the `error` field of
what it returns, and nothing panics on a server or network failure.
`ok()` and `failed()` test it, and `is_connection_error()` says whether the
connection is unusable afterwards (a closed connection then answers every
command with `IRON_ERR_MYSQL_CLOSED`, 7004; open a new one, or let the pool
do it).

| `code` | `server` | Meaning |
|---|---|---|
| 1000 to 4999 or more | `true` | the server's error number, with its `sqlstate` (1045 access denied, 1062 duplicate key, 1064 syntax, 1146 no such table, ...) |
| 1000 to 1099 | `false` | network: 1001 refused, 1004 timeout, 1013 connection lost, ... |
| 3000 to 3099 | `false` | TLS: 3003 handshake, 3004 certificate or host name verification, ... |
| 7000 to 7099 | `false` | MySQL client: protocol (7000), auth (7001), argument (7002), packet too large (7003), closed (7004), NULL (7005), conversion (7006), range (7007), TLS required (7008), pool timeout (7009), pool closed (7010), parameter count (7011), URL (7012), insecure auth (7013), statement closed (7015) |

## Limits

- A packet of 16 MB or more is split and reassembled as the protocol
  requires, in both directions. Before sending a statement over 1 MB the
  client asks the server for its `max_allowed_packet` once and refuses a
  larger statement with 7003 without breaking the connection. A packet
  from the server larger than the client's `max_allowed_packet` ends the
  connection with 7003.
- Results are read completely into memory. Page large queries with
  `LIMIT`, or keep `max_allowed_packet` to the size you expect.
- `LOAD DATA LOCAL INFILE` is refused (7017), compression is not
  negotiated, connections are TCP only (no Unix socket or named pipe), and
  MariaDB's `client_ed25519` plugin is not supported.
- A connection is for one task at a time; share a pool across tasks.
