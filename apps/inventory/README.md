# inventory

A complete HTTP service written in Iron: items and stock movements stored in
MySQL, with prices converted through an external exchange rate API. It is
built the way a production service is (configuration, structured logs,
metrics, readiness, graceful shutdown, retries, a circuit breaker, a cache
that survives outages) and it is part of Iron's CI, so the language, the
compiler and the standard library are exercised by a real program on every
change.

```
apps/inventory/
  iron.toml, src/        the service
  stub/                  fxstub, a local exchange rate API with fault injection
  tests/                 the integration suite (run.sh, integration.py, dbtool.iron)
  bench/                 wrk scenarios, the Go baseline and the benchmark driver
```

## What it does

| Method and path | |
|---|---|
| `GET /healthz` | liveness: the process is up |
| `GET /readyz` | readiness: schema migrated and MySQL reachable; reports the rate table (fresh, stale, unavailable) and the breaker |
| `GET /metrics` | Prometheus text format: requests by route and status, latency histograms, upstream calls by outcome, cache hits, pool and connection gauges |
| `POST /v1/items` | create an item: `{"sku", "name", "category", "price_cents", "quantity"}`; 201, 409 for a duplicate sku, 422 with per-field details |
| `GET /v1/items` | list: `category`, `min_price`, `max_price`, `q` (name prefix), `after` (keyset cursor), `limit` (1..500), `currency` |
| `GET /v1/items/{id}` | one item; `?currency=EUR` adds the converted price |
| `PATCH /v1/items/{id}` | change `name`, `category` or `price_cents` |
| `POST /v1/items/{id}/stock` | `{"delta": -3, "reason": "sale"}`: locks the row, refuses to go below zero (409), records a movement, in one transaction |
| `GET /v1/items/{id}/movements` | newest movements first |
| `GET /v1/reports/categories` | per category: items, units, stock value and the movements of the last `window` seconds (a join against an aggregated subquery); `currency` converts the values |
| `GET /v1/rates/{currency}` | the cached exchange rate |

Prices are stored in the base currency (`rates_base`, USD by default) in
integer cents. Conversion uses the exchange rate table, which is

- fetched from `{rates_url}/latest?from=USD` (the shape of
  [Frankfurter](https://www.frankfurter.app); point `rates_url` at
  `https://api.frankfurter.app` to use the real one),
- cached for `rates_ttl_ms` and refreshed in the background at half the TTL,
- retried `rates_retries` times per refresh with exponential backoff and
  jitter, each attempt bounded by `rates_timeout_ms`,
- protected by a circuit breaker that stops calling the upstream for
  `breaker_cooldown_ms` after `breaker_threshold` failed refreshes,
- stored in MySQL after every refresh and loaded at startup.

When the upstream is down the service answers from the stale table and
marks conversions `"stale": true`; with no table at all, rate lookups answer
503 and item reads leave the price out (`"price": null, "price_error": ...`).
When MySQL is down, database endpoints answer 503 with `Retry-After`, health
stays up, readiness goes down, and the pool reconnects when MySQL is back.
The service starts even when MySQL is unreachable and migrates once it
appears.

## Running it

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
cd apps/inventory
../../build/iron build --release          # target/inventory
(cd stub && ../../../build/iron build --release && ./target/fxstub 8090 &)
INVENTORY_MYSQL_URL=mysql://root:secret@127.0.0.1:3306/inventory ./target/inventory
```

The database must exist; the service creates its tables (migrations are
versioned in `src/db.iron` and serialized across instances with
`GET_LOCK`). A MySQL to try it with:

```sh
docker run -d --name inventory-mysql -p 3306:3306 -e MYSQL_ROOT_PASSWORD=secret \
    -e MYSQL_DATABASE=inventory mysql:8.4
```

### Configuration

Defaults, then an optional file (`--config FILE` or `INVENTORY_CONFIG`) of
`key = value` lines, then environment variables `INVENTORY_<KEY>`. Invalid
settings are reported one per log line and the process exits with status 2;
`--check-config` validates without starting.

| key | default | |
|---|---|---|
| `host`, `port` | `0.0.0.0`, `8080` | listen address (`port = 0` picks a free port, logged at startup) |
| `mysql_url` | `mysql://root@127.0.0.1:3306/inventory` | see [docs/mysql.md](../../docs/mysql.md) for the URL options (TLS, timeouts) |
| `db_pool_min`, `db_pool_max` | `0`, `32` | connection pool size |
| `db_timeout_ms`, `db_acquire_timeout_ms` | `5000`, `2000` | per statement (also set as the sessions' `max_execution_time`, so MySQL stops a SELECT the service gave up on); waiting for a pooled connection |
| `rates_url`, `rates_base` | `http://127.0.0.1:8090`, `USD` | the exchange rate API origin and base currency |
| `rates_ttl_ms`, `rates_timeout_ms`, `rates_retries`, `rates_backoff_ms` | `60000`, `1000`, `2`, `50` | cache lifetime, per attempt timeout, retries, first backoff |
| `breaker_threshold`, `breaker_cooldown_ms` | `5`, `10000` | consecutive failed refreshes that open the breaker, and for how long |
| `request_timeout_ms`, `idle_timeout_ms` | `5000`, `30000` | reading one request; keep-alive idle time |
| `max_header_bytes`, `max_body_bytes` | `16384`, `65536` | request limits (431, 413) |
| `workers` | `0` | serving threads; 0 picks the CPU count plus `db_pool_max` |
| `report_cache_ms` | `2000` | how long a category report is reused (0: never) |
| `log_level`, `log_requests` | `info`, `false` | `debug`, `info`, `warn`, `error`; an access log line per request |
| `shutdown_grace_ms` | `10000` | how long SIGTERM waits for requests in flight |

### Operating it

- Logs are JSON lines on standard output: `ts`, `level`, `msg` and fields.
- SIGTERM or SIGINT: stop accepting, answer the requests in flight with
  `Connection: close`, close the database pool and exit 0. A second signal
  exits at once.
- `GET /metrics` for Prometheus; `GET /readyz` for a load balancer.

## How it is built

- `src/main.iron` wires everything: configuration, the MySQL pool, the
  listener, the background threads (database initialization, rate refresh)
  and the serving threads.
- Requests are served with `Http.poller`: `workers` serving threads (bound
  spawns) each ask the poller for the next connection with a request ready,
  answer it in place and park the connection again, so thousands of idle
  keep-alive clients cost no threads and a slow database call holds only
  the thread serving it ([networking guide](../../docs/networking.md)).
- Each serving thread has its own metrics shard (summed when `/metrics` is
  read), so counting a request takes no shared lock.
- `src/api.iron` routes, validates and writes JSON with `JsonWriter`;
  request bodies are parsed with `Json.parse_with` and explicit limits.
- `src/db.iron` holds the migrations and every query, as prepared statements
  through the shared `rc MysqlPool`; multi-statement work uses `acquire()`
  and a transaction, retried on deadlock.
- `src/rates.iron` is the upstream client, cache and circuit breaker;
  `src/metrics.iron` the counters and their exposition.
- Shared state lives in `rc` objects guarded by a `Mutex` or an `RWLock`
  used as a lock; the rate table is read under a shared lock.

## Testing

```sh
IRON_MYSQL_URL=mysql://root:secret@127.0.0.1:3306/inventory apps/inventory/tests/run.sh build/iron
```

The suite builds the service, `fxstub` and `tests/dbtool.iron`, creates
throwaway databases on that server and drives the real binary: every
endpoint, validation and protocol errors, keyset pagination, concurrent
stock changes (no overselling), the report against recomputed totals,
upstream faults (errors, malformed and truncated JSON, hangs, slow bodies,
closed connections) with stale answers and the breaker, a MySQL outage and a
network partition (through a TCP proxy the suite controls), startup with
MySQL down, graceful shutdown with a request in flight, idle keep-alive
expiry, 300 concurrent connections, and configuration errors.

- `INVENTORY_SANITIZE=1`: build the service with AddressSanitizer and
  UndefinedBehaviorSanitizer (through `IRON_CFLAGS`) and fail on any report,
  leaks included on Linux.
- `INVENTORY_VALGRIND=1`: run the service under valgrind and fail on a leak
  or a memory error.
- `--only name1,name2` runs some tests; `--keep-db` keeps the databases.

In CI it is the ctest `test_inventory_service` (skipped without
`IRON_MYSQL_URL`): the hosted Linux job and `scripts/ci/local_ci.sh` start a
MySQL server for it; the sanitizer build runs it with ASan and the leaks
leg under valgrind.

## Benchmarking

```sh
podman build -t iron-service-bench -f apps/inventory/bench/Containerfile apps/inventory/bench
IRON_MYSQL_URL=mysql://root:secret@mysql:3306/inventory apps/inventory/bench/run.sh build/iron
```

`bench/run.sh` (inside that image: wrk, Go, procps) builds the service with
`--release`, seeds `BENCH_ITEMS` items and runs wrk against each target
pinned to `SERVICE_CPUS` (wrk on `WRK_CPUS`) for the scenarios `health`
(HTTP only), `rate` (in-memory cache), `get` and `get_eur` (primary key read,
with a conversion), `list` (20 rows by index), `stock` (a locking write
transaction) and `create` (an insert transaction) at several connection
counts. It reports requests/s, latency percentiles, errors, CPU time per
1000 requests and resident memory. `bench/go-baseline` is the same API in Go
(net/http, database/sql, go-sql-driver/mysql) for comparison.

## Editing and debugging

- Open `apps/inventory` as the editor's folder: the language server
  analyzes the files under `src/` together, as `iron build` compiles them,
  so names from the other files resolve, and go to definition, references
  and workspace symbols cross files. The other files are read as saved.
- `iron build --debug` and `iron debug`, or VS Code with the Iron extension
  (see docs/guide.md), for breakpoints on `.iron` lines: a breakpoint at
  `src/api.iron` in `dispatch` stops on the next request. The service
  starts without MySQL (database routes answer 503), which is enough to
  step through routing and the rate cache.
- `IRON_CFLAGS="-g -fsanitize=address,undefined" iron build` for a
  sanitizer build of the whole service, runtime included.
- `log_level = debug` logs every rate refresh; `log_requests = true` logs
  every request with its route, status and duration.
- `fxstub` takes `POST /_control {"mode": "hang", "latency_ms": 2000}` (and
  `error`, `malformed`, `wrong_shape`, `partial`, `slow`, `slow_body`,
  `close`) to reproduce upstream trouble by hand; `GET /_stats` counts calls.
