#!/usr/bin/env python3
"""Integration suite for apps/inventory.

Drives the real service binary against a real MySQL server and the fxstub
exchange rate stub. MySQL is reached through a fault-injecting TCP proxy run
by this script, so a database outage (all connections cut, connects refused,
then back) can be produced without controlling the server itself.

    integration.py --service BIN --stub BIN --dbtool BIN --mysql-url URL
                   [--wrap "valgrind ..."] [--only NAME[,NAME]] [--keep-db]

Each test prints [PASS] or [FAIL]; the exit status is 1 when any failed.
Uses only the Python standard library.
"""
import argparse
import http.client
import json
import os
import random
import re
import shlex
import signal
import socket
import string
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.parse

ARGS = None
FAILURES = []
MEMORY_REPORTS = []
SANITIZER_PATTERN = r"ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:|definitely lost: [1-9]|indirectly lost: [1-9]|Invalid (read|write)|uninitialised|panic"


# ---------------------------------------------------------------- helpers

class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def eq(actual, expected, what):
    if actual != expected:
        raise Fail(f"{what}: expected {expected!r}, got {actual!r}")


def wait_for(pred, timeout, what, interval=0.05):
    end = time.time() + timeout
    last = None
    while time.time() < end:
        try:
            last = pred()
            if last:
                return last
        except (OSError, http.client.HTTPException) as e:
            last = e
        time.sleep(interval)
    raise Fail(f"timed out after {timeout}s waiting for {what} (last: {last!r})")


def rand_suffix(n=8):
    return "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(n))


class Client:
    """A persistent HTTP/1.1 connection to the service."""

    def __init__(self, port, timeout=10):
        self.port = port
        self.timeout = timeout
        self.conn = None

    def request(self, method, path, body=None, headers=None, raw_body=None):
        hdrs = dict(headers or {})
        data = raw_body
        if body is not None:
            data = json.dumps(body).encode()
            hdrs.setdefault("Content-Type", "application/json")
        for attempt in range(2):
            if self.conn is None:
                self.conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=self.timeout)
            try:
                self.conn.request(method, path, body=data, headers=hdrs)
                resp = self.conn.getresponse()
                text = resp.read()
                if resp.getheader("Connection", "").lower() == "close":
                    self.close()
                return Response(resp.status, dict((k.lower(), v) for k, v in resp.getheaders()), text)
            except (http.client.RemoteDisconnected, BrokenPipeError, ConnectionResetError):
                # The server closed an idle keep-alive connection: reconnect once.
                self.close()
                if attempt == 1:
                    raise
        raise Fail("unreachable")

    def close(self):
        if self.conn is not None:
            self.conn.close()
            self.conn = None

    def get(self, path, **kw):
        return self.request("GET", path, **kw)

    def post(self, path, body=None, **kw):
        return self.request("POST", path, body=body, **kw)

    def patch(self, path, body=None, **kw):
        return self.request("PATCH", path, body=body, **kw)


class Response:
    def __init__(self, status, headers, raw):
        self.status = status
        self.headers = headers
        self.raw = raw
        self._json = None

    @property
    def json(self):
        if self._json is None:
            try:
                self._json = json.loads(self.raw)
            except ValueError as e:
                raise Fail(f"response is not JSON ({e}): {self.raw[:200]!r}")
        return self._json

    def __repr__(self):
        return f"<{self.status} {self.raw[:300]!r}>"


def expect(resp, status, what):
    if resp.status != status:
        raise Fail(f"{what}: expected HTTP {status}, got {resp.status}: {resp.raw[:300]!r}")
    return resp


# ------------------------------------------------------------ fault proxy

class FaultProxy:
    """TCP proxy in front of MySQL. cut() closes every relayed connection and
    stops listening (connects are refused, as during a restart); restore()
    listens again on the same port; blackhole() accepts but relays nothing."""

    def __init__(self, upstream_host, upstream_port):
        self.upstream = (upstream_host, upstream_port)
        self.lock = threading.Lock()
        self.pairs = []
        self.mode = "relay"
        self.listener = None
        self.port = 0
        self.relayed = 0
        self._listen()

    def _listen(self):
        ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind(("127.0.0.1", self.port))
        ls.listen(256)
        self.port = ls.getsockname()[1]
        self.listener = ls
        threading.Thread(target=self._accept_loop, args=(ls,), daemon=True).start()

    def _accept_loop(self, ls):
        while True:
            try:
                c, _ = ls.accept()
            except OSError:
                return
            with self.lock:
                mode = self.mode
            if mode == "down" or ls is not self.listener:
                c.close()
                return
            if mode == "blackhole":
                with self.lock:
                    self.pairs.append((c, None))
                continue
            try:
                u = socket.create_connection(self.upstream, timeout=5)
                u.settimeout(None)
            except OSError:
                c.close()
                continue
            with self.lock:
                self.pairs.append((c, u))
                self.relayed += 1
            threading.Thread(target=self._pump, args=(c, u), daemon=True).start()
            threading.Thread(target=self._pump, args=(u, c), daemon=True).start()

    @staticmethod
    def _pump(src, dst):
        try:
            while True:
                data = src.recv(65536)
                if not data:
                    break
                dst.sendall(data)
        except OSError:
            pass
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def _drop_all(self):
        with self.lock:
            pairs, self.pairs = self.pairs, []
        for a, b in pairs:
            for s in (a, b):
                if s is None:
                    continue
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                s.close()

    def cut(self):
        with self.lock:
            self.mode = "down"
        # shutdown() wakes a thread blocked in accept() (close() alone does
        # not on Linux, and the socket would keep accepting).
        for op in (lambda: self.listener.shutdown(socket.SHUT_RDWR), self.listener.close):
            try:
                op()
            except OSError:
                pass
        self._drop_all()

    def restore(self):
        with self.lock:
            self.mode = "relay"
        self._listen()

    def blackhole(self):
        with self.lock:
            self.mode = "blackhole"
        self._drop_all()

    def relay(self):
        self._drop_all()
        with self.lock:
            self.mode = "relay"


# ------------------------------------------------------------- processes

class Stub:
    def __init__(self):
        self.proc = subprocess.Popen([ARGS.stub, "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        line = self.proc.stdout.readline()
        m = re.match(r"listening (\d+)", line)
        if not m:
            raise Fail(f"fxstub did not start: {line!r}")
        self.port = int(m.group(1))
        self.client = Client(self.port)

    def control(self, **kw):
        r = self.client.post("/_control", body=kw)
        expect(r, 200, "stub control")
        return r.json

    def reset(self):
        expect(self.client.post("/_reset", body={}), 200, "stub reset")

    def stats(self):
        return self.client.get("/_stats").json

    def stop(self):
        self.client.close()
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


class Service:
    """The service under test. Its stdout (JSON log lines) is collected."""

    def __init__(self, db_url, rates_url, extra_env=None, args=None, wait_ready=True):
        env = dict(os.environ)
        env.update({
            "INVENTORY_HOST": "127.0.0.1",
            "INVENTORY_PORT": "0",
            "INVENTORY_MYSQL_URL": db_url,
            "INVENTORY_RATES_URL": rates_url,
            "INVENTORY_RATES_TTL_MS": "1500",
            "INVENTORY_RATES_TIMEOUT_MS": "400",
            "INVENTORY_RATES_RETRIES": "1",
            "INVENTORY_RATES_BACKOFF_MS": "20",
            "INVENTORY_BREAKER_THRESHOLD": "2",
            "INVENTORY_BREAKER_COOLDOWN_MS": "2000",
            "INVENTORY_REQUEST_TIMEOUT_MS": "2000",
            "INVENTORY_IDLE_TIMEOUT_MS": "3000",
            "INVENTORY_DB_TIMEOUT_MS": "2000",
            "INVENTORY_DB_ACQUIRE_TIMEOUT_MS": "1500",
            "INVENTORY_DB_POOL_MAX": "16",
            "INVENTORY_MAX_BODY_BYTES": "16384",
            "INVENTORY_SHUTDOWN_GRACE_MS": "8000",
            "INVENTORY_LOG_LEVEL": "info",
            "INVENTORY_REPORT_CACHE_MS": "0",
        })
        env.update(extra_env or {})
        cmd = shlex.split(ARGS.wrap) if ARGS.wrap else []
        cmd += [ARGS.service] + (args or [])
        self.stderr_file = tempfile.TemporaryFile()
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=self.stderr_file, env=env, text=True)
        self.lines = []
        self.lock = threading.Lock()
        self.port_event = threading.Event()
        self.port = None
        threading.Thread(target=self._read, daemon=True).start()
        if not self.port_event.wait(60 if ARGS.wrap else 30):
            self.stop(force=True)
            raise Fail(f"service did not report its port; output: {self.output()[-2000:]}")
        if self.port is None:
            raise Fail(f"service exited early; output: {self.output()[-2000:]}")
        self.client = Client(self.port)
        if wait_ready:
            try:
                wait_for(lambda: self.client.get("/readyz").status == 200, 60 if ARGS.wrap else 20, "service readiness")
            except Fail as e:
                raise Fail(f"{e}; service output:\n{self.output()[-3000:]}\n{self.stderr()[-2000:]}")

    def _read(self):
        for line in self.proc.stdout:
            with self.lock:
                self.lines.append(line.rstrip("\n"))
            if self.port is None:
                try:
                    rec = json.loads(line)
                    if rec.get("msg") == "listening":
                        self.port = rec["port"]
                        self.port_event.set()
                except ValueError:
                    pass
        self.port_event.set()

    def output(self):
        with self.lock:
            return "\n".join(self.lines)

    def logs(self):
        out = []
        with self.lock:
            for l in self.lines:
                try:
                    out.append(json.loads(l))
                except ValueError:
                    out.append({"raw": l})
        return out

    def stderr(self):
        self.stderr_file.seek(0)
        return self.stderr_file.read().decode(errors="replace")

    def wait_exit(self, timeout=None):
        try:
            return self.proc.wait(timeout or (120 if ARGS.wrap else 20))
        except subprocess.TimeoutExpired:
            raise Fail("service did not exit")
        finally:
            self.check_stderr()

    def check_stderr(self):
        err = self.stderr()
        if re.search(SANITIZER_PATTERN, err):
            MEMORY_REPORTS.append(err[-6000:])

    def stop(self, force=False, timeout=None):
        try:
            return self._stop(force, timeout)
        finally:
            self.check_stderr()

    def _stop(self, force=False, timeout=None):
        if self.proc.poll() is None:
            if hasattr(self, "client"):
                self.client.close()
            self.proc.send_signal(signal.SIGKILL if force else signal.SIGTERM)
        try:
            self.proc.wait(timeout or (120 if ARGS.wrap else 20))
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            raise Fail("service did not stop after SIGTERM")
        return self.proc.returncode


def dbtool(*args):
    r = subprocess.run([ARGS.dbtool] + list(args), capture_output=True, text=True)
    if r.returncode != 0:
        raise Fail(f"dbtool {args[0]} failed: {r.stderr.strip()}")
    return r.stdout.strip()


def with_database(url, name):
    """URL with its database path replaced by `name`."""
    p = urllib.parse.urlsplit(url)
    return urllib.parse.urlunsplit((p.scheme, p.netloc, "/" + name, p.query, p.fragment))


def via_proxy(url, port):
    p = urllib.parse.urlsplit(url)
    userinfo = p.netloc.rsplit("@", 1)[0] + "@" if "@" in p.netloc else ""
    return urllib.parse.urlunsplit((p.scheme, f"{userinfo}127.0.0.1:{port}", p.path, p.query, p.fragment))


# ------------------------------------------------------------------ suite

class Env:
    pass


E = Env()


def new_item(client, **over):
    body = {
        "sku": "SKU-" + rand_suffix(),
        "name": "Widget " + rand_suffix(4),
        "category": "widgets",
        "price_cents": 1000,
        "quantity": 5,
    }
    body.update(over)
    r = client.post("/v1/items", body=body)
    expect(r, 201, f"create {body['sku']}")
    return r.json


def t_health_and_ready():
    c = E.svc.client
    r = expect(c.get("/healthz"), 200, "healthz")
    eq(r.json, {"status": "ok"}, "healthz body")
    r = expect(c.get("/readyz"), 200, "readyz")
    eq(r.json["status"], "ready", "readyz status")
    eq(r.json["checks"]["database"], "ok", "readyz database")
    r = c.request("HEAD", "/healthz")
    eq(r.status, 200, "HEAD healthz")
    eq(r.raw, b"", "HEAD has no body")


def t_database_sessions():
    """Every pooled session is set up by the pool's init_command: UTC, and a
    statement time limit equal to the client's timeout, so the server stops
    a SELECT the client no longer waits for."""
    c = E.svc.client
    expect(c.get("/v1/items?limit=1"), 200, "a database request")
    sql = ("SELECT CONCAT(MIN(v.VARIABLE_VALUE), ' ', MAX(v.VARIABLE_VALUE), ' ', COUNT(*) > 0) "
           "FROM performance_schema.variables_by_thread v "
           "JOIN performance_schema.threads t USING (THREAD_ID) "
           "WHERE v.VARIABLE_NAME = '%s' AND t.PROCESSLIST_DB = '" + E.db_name + "'")
    eq(dbtool("scalar", ARGS.mysql_url, sql % "max_execution_time"), "2000 2000 1", "max_execution_time of the sessions")
    eq(dbtool("scalar", ARGS.mysql_url, sql % "time_zone"), "+00:00 +00:00 1", "time_zone of the sessions")


def t_create_and_get():
    c = E.svc.client
    item = new_item(c, name="  Claw hammer  ", category="tools", price_cents=1599, quantity=10)
    eq(item["name"], "Claw hammer", "name is trimmed")
    eq(item["quantity"], 10, "quantity")
    check(re.match(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}Z$", item["created_at"]), f"created_at format {item['created_at']}")
    r = expect(c.get(f"/v1/items/{item['id']}"), 200, "get item")
    for k in ("id", "sku", "name", "category", "price_cents", "quantity"):
        eq(r.json[k], item[k], f"get {k}")
    check("price" not in r.json, "no conversion without ?currency")
    r = c.get(f"/v1/items/{item['id']}/movements")
    expect(r, 200, "movements")
    eq([(m["delta"], m["reason"]) for m in r.json["movements"]], [(10, "initial")], "initial movement")
    # Unicode survives the round trip.
    u = new_item(c, name="Café crème 日本 ✓")
    eq(expect(c.get(f"/v1/items/{u['id']}"), 200, "get unicode").json["name"], "Café crème 日本 ✓", "unicode name")


def t_create_errors():
    c = E.svc.client
    sku = "DUP-" + rand_suffix()
    new_item(c, sku=sku)
    r = expect(c.post("/v1/items", body={"sku": sku, "name": "x", "category": "c", "price_cents": 1}), 409, "duplicate sku")
    check("already exists" in r.json["error"], "duplicate message")
    r = expect(c.post("/v1/items", body={"sku": "bad sku!", "name": "", "category": "Bad Cat", "price_cents": -5, "quantity": "7"}), 422, "validation")
    fields = sorted(d["field"] for d in r.json["details"])
    eq(fields, ["category", "name", "price_cents", "quantity", "sku"], "invalid fields")
    expect(c.post("/v1/items", body={"name": "x"}), 422, "missing fields")
    expect(c.post("/v1/items", raw_body=b'{"sku": "a",', headers={"Content-Type": "application/json"}), 400, "malformed JSON")
    expect(c.post("/v1/items", raw_body=b'[1,2]', headers={"Content-Type": "application/json"}), 400, "array body")
    expect(c.post("/v1/items", raw_body=b'sku=a', headers={"Content-Type": "application/x-www-form-urlencoded"}), 415, "form body")
    expect(c.post("/v1/items", body={"sku": "F-" + rand_suffix(), "name": "x", "category": "c", "price_cents": 1.5}), 422, "fractional price")
    expect(c.post("/v1/items", body={"sku": "F-" + rand_suffix(), "name": "a\u0001b", "category": "c", "price_cents": 1}), 422, "control char in name")
    big = {"sku": "BIG-1", "name": "x" * 20000, "category": "c", "price_cents": 1}
    r = c.post("/v1/items", body=big)
    eq(r.status, 413, "body over max_body_bytes")
    expect(c.post("/v1/items", body={"sku": "D-" + rand_suffix(), "name": "x", "category": "c", "price_cents": 10**13}), 422, "price too large")


def t_routing():
    c = E.svc.client
    expect(c.get("/nope"), 404, "unknown path")
    expect(c.get("/v1/items/abc"), 404, "non-numeric id")
    expect(c.get("/v1/items/0"), 404, "id 0")
    expect(c.get("/v1/items/999999999999"), 404, "missing item")
    expect(c.get("/v1/items/-3"), 404, "negative id")
    r = expect(c.request("DELETE", "/v1/items/1"), 405, "DELETE item")
    eq(r.headers.get("allow"), "GET, PATCH", "Allow header")
    expect(c.request("PUT", "/v1/items"), 405, "PUT collection")
    expect(c.get("/v1/items/1/stock"), 405, "GET stock")
    expect(c.get("/v1/items?limit=%zz"), 400, "bad percent escape")
    expect(c.get("/v1/unknown/thing"), 404, "unknown v1 path")


def t_list_filters_and_pagination():
    c = E.svc.client
    cat = "cat-" + rand_suffix(6)
    created = []
    for i in range(23):
        created.append(new_item(c, category=cat, price_cents=100 * (i + 1), name=f"Gizmo {i:02d}"))
    special = new_item(c, category=cat, price_cents=5, name="50% off_sale")
    created.append(special)
    ids = [x["id"] for x in created]
    # Walk every page with keyset pagination.
    seen = []
    after = 0
    pages = 0
    while True:
        r = expect(c.get(f"/v1/items?category={cat}&limit=7&after={after}"), 200, "list page")
        page = r.json
        seen += [x["id"] for x in page["items"]]
        pages += 1
        eq(page["count"], len(page["items"]), "count")
        if page["next_after"] is None:
            break
        after = page["next_after"]
        check(pages < 20, "pagination does not end")
    eq(seen, sorted(ids), "every item exactly once, in id order")
    eq(pages, 4, "pages of 7 for 24 items")
    r = expect(c.get(f"/v1/items?category={cat}&min_price=500&max_price=900&limit=100"), 200, "price range")
    eq(sorted(x["price_cents"] for x in r.json["items"]), [500, 600, 700, 800, 900], "price filter")
    r = expect(c.get(f"/v1/items?category={cat}&q=Gizmo%201&limit=100"), 200, "name prefix")
    eq(len(r.json["items"]), 10, "prefix 'Gizmo 1' matches 10..19")
    r = expect(c.get(f"/v1/items?category={cat}&q=50%25&limit=100"), 200, "prefix with %")
    eq([x["id"] for x in r.json["items"]], [special["id"]], "LIKE escapes %")
    r = expect(c.get(f"/v1/items?category={cat}&q=5_&limit=100"), 200, "prefix with _")
    eq(r.json["items"], [], "LIKE escapes _")
    r = expect(c.get(f"/v1/items?category={cat}&q=Gizmo+0&limit=100"), 200, "plus is space")
    eq(len(r.json["items"]), 10, "'Gizmo 0' prefix")
    for bad in ("limit=0", "limit=501", "limit=abc", "min_price=-1", "after=x", "max_price=1e3"):
        expect(c.get(f"/v1/items?{bad}"), 400, f"bad query {bad}")


def t_update():
    c = E.svc.client
    item = new_item(c, price_cents=1000)
    r = expect(c.patch(f"/v1/items/{item['id']}", body={"price_cents": 1250, "name": "Renamed"}), 200, "patch")
    eq((r.json["price_cents"], r.json["name"], r.json["sku"]), (1250, "Renamed", item["sku"]), "patched fields")
    expect(c.patch(f"/v1/items/{item['id']}", body={"sku": "NEW"}), 422, "sku is immutable")
    expect(c.patch(f"/v1/items/{item['id']}", body={}), 422, "empty patch")
    expect(c.patch(f"/v1/items/{item['id']}", body={"price_cents": "x"}), 422, "bad price")
    expect(c.patch("/v1/items/999999999", body={"name": "x"}), 404, "patch missing")
    r = expect(c.get(f"/v1/items/{item['id']}"), 200, "get after patch")
    eq(r.json["price_cents"], 1250, "persisted")


def t_stock():
    c = E.svc.client
    item = new_item(c, quantity=5)
    r = expect(c.post(f"/v1/items/{item['id']}/stock", body={"delta": -3, "reason": "sale"}), 200, "decrement")
    eq(r.json["quantity"], 2, "quantity after sale")
    r = expect(c.post(f"/v1/items/{item['id']}/stock", body={"delta": -3, "reason": "sale"}), 409, "insufficient")
    eq((r.json["quantity"], r.json["requested"]), (2, -3), "409 body")
    expect(c.post(f"/v1/items/{item['id']}/stock", body={"delta": 0}), 422, "zero delta")
    expect(c.post(f"/v1/items/{item['id']}/stock", body={"delta": 1, "reason": "Bad Reason"}), 422, "bad reason")
    expect(c.post("/v1/items/999999999/stock", body={"delta": 1}), 404, "stock of missing item")
    r = expect(c.post(f"/v1/items/{item['id']}/stock", body={"delta": 8}), 200, "restock")
    eq(r.json["quantity"], 10, "quantity after restock")
    r = expect(c.get(f"/v1/items/{item['id']}/movements?limit=2"), 200, "movements")
    eq([(m["delta"], m["reason"]) for m in r.json["movements"]], [(8, "adjustment"), (-3, "sale")], "newest first")
    expect(c.get("/v1/items/999999999/movements"), 404, "movements of missing item")


def t_stock_concurrency():
    item = new_item(E.svc.client, quantity=20)
    results = []
    lock = threading.Lock()

    def worker():
        cl = Client(E.svc.port)
        for _ in range(5):
            r = cl.post(f"/v1/items/{item['id']}/stock", body={"delta": -1, "reason": "sale"})
            with lock:
                results.append(r.status)
        cl.close()

    threads = [threading.Thread(target=worker) for _ in range(10)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    eq(results.count(200), 20, "exactly the stock is sold")
    eq(results.count(409), 30, "the rest is refused")
    r = expect(E.svc.client.get(f"/v1/items/{item['id']}"), 200, "final item")
    eq(r.json["quantity"], 0, "final quantity")
    r = expect(E.svc.client.get(f"/v1/items/{item['id']}/movements?limit=200"), 200, "movements")
    eq(len(r.json["movements"]), 21, "initial + 20 sales")
    eq(sum(m["delta"] for m in r.json["movements"]), 0, "movements sum to the quantity")


def t_report():
    c = E.svc.client
    r = expect(c.get("/v1/reports/categories"), 200, "report")
    rows = {x["category"]: x for x in r.json["categories"]}
    # Recompute from the list endpoint.
    totals = {}
    after = 0
    while True:
        page = expect(c.get(f"/v1/items?limit=500&after={after}"), 200, "list all").json
        for it in page["items"]:
            t = totals.setdefault(it["category"], [0, 0, 0])
            t[0] += 1
            t[1] += it["quantity"]
            t[2] += it["price_cents"] * it["quantity"]
        if page["next_after"] is None:
            break
        after = page["next_after"]
    eq(set(rows), set(totals), "categories")
    for cat, (n, units, value) in totals.items():
        eq((rows[cat]["items"], rows[cat]["units"], rows[cat]["value_cents"]), (n, units, value), f"report {cat}")
    eq(r.json["total_units"], sum(t[1] for t in totals.values()), "total units")
    r2 = expect(c.get("/v1/reports/categories?currency=EUR"), 200, "report in EUR")
    eq(r2.json["currency"], "EUR", "currency")
    usd = r.json["total_value_cents"]
    eur = r2.json["total_value_cents"]
    check(abs(eur - usd * 0.9213) <= len(totals) + 1 + usd * 0.01, f"converted total {eur} vs {usd}")
    expect(c.get("/v1/reports/categories?window=0"), 400, "bad window")
    expect(c.get("/v1/reports/categories?currency=XX"), 400, "bad currency")


def t_rates_and_conversion():
    c = E.svc.client
    E.stub.reset()
    r = expect(c.get("/v1/rates/eur"), 200, "rate")
    eq((r.json["base"], r.json["currency"]), ("USD", "EUR"), "rate codes")
    check(abs(r.json["rate"] - 0.9213) < 0.01, f"EUR rate {r.json['rate']}")
    eq(expect(c.get("/v1/rates/USD"), 200, "base rate").json["rate"], 1.0, "base rate is 1")
    expect(c.get("/v1/rates/XYZ"), 400, "unknown currency")
    expect(c.get("/v1/rates/EURO"), 400, "bad code")
    item = new_item(c, price_cents=10000)
    r = expect(c.get(f"/v1/items/{item['id']}?currency=JPY"), 200, "converted item")
    p = r.json["price"]
    eq(p["currency"], "JPY", "price currency")
    check(abs(p["amount_cents"] - round(10000 * p["rate"])) <= 1, "amount = price * rate")
    expect(c.get(f"/v1/items/{item['id']}?currency=QQQ"), 400, "unknown currency on item")
    r = expect(c.get("/v1/items?limit=3&currency=GBP"), 200, "converted list")
    check(all("price" in x for x in r.json["items"]), "every listed item converted")


def metric(text, name, labels=None):
    for line in text.splitlines():
        if line.startswith("#"):
            continue
        m = re.match(r"([a-zA-Z_:][a-zA-Z0-9_:]*)(\{[^}]*\})? (\S+)$", line)
        if not m or m.group(1) != name:
            continue
        if labels and all(f'{k}="{v}"' in (m.group(2) or "") for k, v in labels.items()) or not labels:
            return float(m.group(3))
    return None


def t_upstream_faults():
    c = E.svc.client
    E.stub.reset()
    wait_for(lambda: not c.get("/v1/rates/EUR").json.get("stale", True), 10, "fresh rates")
    # Upstream answers 503 from now on: once the table is older than the TTL
    # the service answers from it, marked stale, and the breaker opens.
    E.stub.control(mode="error", status=503, count=-1)
    r = wait_for(lambda: (lambda x: x if x.status == 200 and x.json["stale"] else None)(c.get("/v1/rates/EUR")), 10, "stale rate")
    check(r.json["age_ms"] >= 1500, "stale after the TTL")
    m = c.get("/metrics").raw.decode()
    wait_for(lambda: metric(c.get("/metrics").raw.decode(), "inventory_upstream_breaker_open") == 1.0, 10, "breaker open")
    before = E.stub.stats()["requests"]
    time.sleep(0.8)
    eq(E.stub.stats()["requests"], before, "no upstream calls while the breaker is open")
    item = new_item(c)
    r = expect(c.get(f"/v1/items/{item['id']}?currency=EUR"), 200, "item during outage")
    eq(r.json["price"]["stale"], True, "stale conversion")
    r = expect(c.get("/readyz"), 200, "ready during upstream outage")
    eq(r.json["checks"]["rates"], "stale", "readyz reports stale rates")
    # Each kind of bad answer keeps the service on its stale table. Wait for
    # the service to have tried the upstream in each mode (the breaker lets
    # one refresh through per cooldown).
    for mode in ("malformed", "wrong_shape", "partial", "close", "hang", "slow_body"):
        E.stub.control(mode=mode, count=-1, latency_ms=1500 if mode in ("hang", "slow_body") else 0)
        seen = E.stub.stats()["requests"]
        wait_for(lambda: E.stub.stats()["requests"] > seen, 8, f"an upstream call in mode {mode}")
        t0 = time.time()
        r = expect(c.get("/v1/rates/GBP"), 200, f"rate during {mode}")
        check(time.time() - t0 < 1.0, f"{mode}: answered from cache without waiting")
        eq(r.json["stale"], True, f"{mode}: stale")
    m = c.get("/metrics").raw.decode()
    for outcome in ("http_error", "malformed", "transport_error"):
        check((metric(m, "inventory_upstream_requests_total", {"outcome": outcome}) or 0) > 0, f"metric outcome {outcome}")
    # Recovery: after the cooldown the next refresh succeeds and closes it.
    E.stub.control(mode="ok", latency_ms=0, bump=True)
    wait_for(lambda: not c.get("/v1/rates/EUR").json["stale"], 10, "fresh again after recovery")
    wait_for(lambda: metric(c.get("/metrics").raw.decode(), "inventory_upstream_breaker_open") == 0.0, 5, "breaker closed")
    # Latency within budget is fine.
    E.stub.control(mode="ok", latency_ms=200)
    time.sleep(2.0)
    expect(c.get("/v1/rates/EUR"), 200, "slow but in time")
    E.stub.reset()


def t_no_rates_at_all():
    """A fresh database (no stored snapshot) and a dead upstream: rate
    lookups fail with 503, item reads still work, with the price left out."""
    name = "inventory_it_norates_" + rand_suffix(6)
    dbtool("create", ARGS.mysql_url, name)
    stub = Stub()
    stub.control(mode="error", status=500, count=-1)
    svc = None
    try:
        svc = Service(with_database(ARGS.mysql_url, name), f"http://127.0.0.1:{stub.port}")
        c = svc.client
        r = expect(c.get("/v1/rates/EUR"), 503, "rate with no table")
        eq(r.headers.get("retry-after"), "1", "Retry-After on 503")
        item = new_item(c)
        r = expect(c.get(f"/v1/items/{item['id']}?currency=EUR"), 200, "item read degrades")
        eq(r.json["price"], None, "no price")
        check("unavailable" in r.json["price_error"], "price_error")
        r = expect(c.get("/v1/reports/categories?currency=EUR"), 200, "report degrades")
        eq((r.json["degraded"], r.json["currency"]), (True, "USD"), "report in base currency")
        r = expect(c.get("/readyz"), 200, "ready without rates")
        eq(r.json["checks"]["rates"], "unavailable", "rates unavailable")
        # The upstream comes back: the next lookup refreshes.
        stub.control(mode="ok")
        wait_for(lambda: c.get("/v1/rates/EUR").status == 200, 10, "rates after upstream recovery")
        # The snapshot was persisted: a restart with the upstream down still has rates.
        eq(svc.stop(), 0, "exit status")
        stub.control(mode="error", count=-1)
        svc = Service(with_database(ARGS.mysql_url, name), f"http://127.0.0.1:{stub.port}")
        r = expect(svc.client.get("/v1/rates/EUR"), 200, "rate from the stored snapshot")
        check(abs(r.json["rate"] - 0.92) < 0.01, "stored rate")
    finally:
        if svc:
            svc.stop()
        stub.stop()
        if not ARGS.keep_db:
            dbtool("drop", ARGS.mysql_url, name)


def t_metrics():
    c = E.svc.client
    before = metric(c.get("/metrics").raw.decode(), "inventory_http_requests_total", {"route": "health", "code": "200"}) or 0
    for _ in range(5):
        c.get("/healthz")
    r = expect(c.get("/metrics"), 200, "metrics")
    check(r.headers["content-type"].startswith("text/plain"), "metrics content type")
    text = r.raw.decode()
    eq(metric(text, "inventory_http_requests_total", {"route": "health", "code": "200"}), before + 5, "health counter")
    check(metric(text, "inventory_http_request_duration_seconds_count", {"route": "health"}) >= 5, "histogram count")
    check(metric(text, "inventory_db_ready") == 1.0, "db ready gauge")
    for line in text.splitlines():
        check(line == "" or line.startswith("#") or re.match(r"^[a-z_]+(\{[^}]*\})? [0-9.e+-]+$", line), f"metrics line {line!r}")


def raw_exchange(port, data, timeout=5, read_all=True):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        if data:
            s.sendall(data)
        out = b""
        while True:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            out += chunk
            if not read_all:
                break
        return out
    finally:
        s.close()


def t_http_protocol_edges():
    port = E.svc.port
    out = raw_exchange(port, b"GARBAGE\r\n\r\n")
    check(out.startswith(b"HTTP/1.1 400"), f"garbage request: {out[:80]!r}")
    out = raw_exchange(port, b"GET /healthz HTTP/1.1\r\n\r\n")
    check(out.startswith(b"HTTP/1.1 400"), f"missing Host: {out[:80]!r}")
    out = raw_exchange(port, b"GET /healthz HTTP/1.0\r\nUser-Agent: it\r\n\r\n")
    check(out.startswith(b"HTTP/1.1 200") and b"Connection: close" in out, f"HTTP/1.0: {out[:120]!r}")
    # Headers over max_header_bytes: 431, delivered although the rest of the
    # request was never read (the poller drains before closing).
    out = raw_exchange(port, b"GET /healthz HTTP/1.1\r\nHost: x\r\n" + b"X-Big: " + b"a" * 20000 + b"\r\n\r\n")
    check(out.startswith(b"HTTP/1.1 431"), f"huge header: {out[:80]!r}")
    # A body over max_body_bytes is refused from its Content-Length alone.
    out = raw_exchange(port, b"POST /v1/items HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: 200000\r\n\r\n" + b"x" * 200000)
    check(out.startswith(b"HTTP/1.1 413"), f"huge body: {out[:80]!r}")
    # Chunked request body.
    body = json.dumps({"sku": "CH-" + rand_suffix(), "name": "chunky", "category": "c", "price_cents": 3}).encode()
    chunked = b"%x\r\n%s\r\n0\r\n\r\n" % (len(body), body)
    out = raw_exchange(port, b"POST /v1/items HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" + chunked)
    check(out.startswith(b"HTTP/1.1 201"), f"chunked body: {out[:80]!r}")
    # A client that sends half a request line and stalls is dropped after
    # request_timeout_ms without an answer; others are not held up meanwhile.
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall(b"GET /heal")
    t0 = time.time()
    expect(E.svc.client.get("/healthz"), 200, "healthz while a client stalls")
    check(time.time() - t0 < 1, "stalled client does not block others")
    data = s.recv(100)
    check(time.time() - t0 < 6, "stalled client closed after the request timeout")
    eq(data[:12] if data else b"", b"", "no answer to a stalled request")
    s.close()
    # Many idle keep-alive connections at once, then a request on each: the
    # poller keeps them off the workers.
    socks = []
    for _ in range(300):
        socks.append(socket.create_connection(("127.0.0.1", port), timeout=10))
    ok = 0
    for s in socks:
        s.sendall(b"GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n")
    for s in socks:
        if s.recv(4096).startswith(b"HTTP/1.1 200"):
            ok += 1
        s.close()
    eq(ok, 300, "300 concurrent keep-alive clients answered")


def t_keepalive_and_idle():
    c = Client(E.svc.port)
    for i in range(200):
        expect(c.get("/healthz"), 200, f"request {i} on one connection")
    sock = c.conn.sock
    time.sleep(3.6)
    # The idle connection was closed by the server after idle_timeout_ms.
    sock.settimeout(1)
    try:
        got = sock.recv(10)
    except OSError:
        got = b""
    eq(got, b"", "idle connection closed")
    c.close()


def t_mysql_outage():
    c = E.svc.client
    item = new_item(c, quantity=50)
    expect(c.get(f"/v1/items/{item['id']}"), 200, "before outage")
    E.proxy.cut()
    t0 = time.time()
    r = c.get(f"/v1/items/{item['id']}")
    eq(r.status, 503, "read during outage")
    eq(r.headers.get("retry-after"), "1", "Retry-After")
    check(time.time() - t0 < 4, "fails fast")
    expect(c.post(f"/v1/items/{item['id']}/stock", body={"delta": -1}), 503, "write during outage")
    expect(c.get("/healthz"), 200, "alive during outage")
    expect(c.get("/readyz"), 503, "not ready during outage")
    expect(c.get("/v1/rates/EUR"), 200, "cache still serves rates")
    time.sleep(1.0)
    E.proxy.restore()
    wait_for(lambda: c.get(f"/v1/items/{item['id']}").status == 200, 15, "reads recover")
    r = expect(c.post(f"/v1/items/{item['id']}/stock", body={"delta": -1}), 200, "writes recover")
    eq(r.json["quantity"], 49, "the failed write did not apply")
    expect(c.get("/readyz"), 200, "ready again")
    # A network partition (connects hang) is bounded by the timeouts.
    E.proxy.blackhole()
    t0 = time.time()
    r = c.get(f"/v1/items/{item['id']}?x=1")
    check(r.status in (503, 500), f"read during partition: {r.status}")
    check(time.time() - t0 < 8, "partition bounded by db timeouts")
    E.proxy.relay()
    wait_for(lambda: c.get(f"/v1/items/{item['id']}").status == 200, 15, "recovery after partition")


def t_concurrent_mixed_load():
    errors = []
    stop = time.time() + 4
    cat = "load-" + rand_suffix(5)
    ids = [new_item(E.svc.client, category=cat, quantity=1000)["id"] for _ in range(5)]
    counts = {"ok": 0}
    lock = threading.Lock()

    def worker(n):
        cl = Client(E.svc.port)
        rnd = random.Random(n)
        while time.time() < stop:
            op = rnd.random()
            if op < 0.4:
                r = cl.get(f"/v1/items/{rnd.choice(ids)}?currency=EUR")
                want = 200
            elif op < 0.6:
                r = cl.get(f"/v1/items?category={cat}&limit=10")
                want = 200
            elif op < 0.8:
                r = cl.post(f"/v1/items/{rnd.choice(ids)}/stock", body={"delta": rnd.choice([-1, 1])})
                want = 200
            elif op < 0.9:
                r = cl.get("/v1/reports/categories")
                want = 200
            else:
                r = cl.get("/v1/rates/CHF")
                want = 200
            with lock:
                if r.status != want:
                    errors.append((r.status, r.raw[:200]))
                else:
                    counts["ok"] += 1
        cl.close()

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(24)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    eq(errors[:5], [], "errors under mixed load")
    check(counts["ok"] > 100, f"made progress ({counts['ok']} requests)")
    # Quantities stayed consistent with the movement log.
    for i in ids:
        q = E.svc.client.get(f"/v1/items/{i}").json["quantity"]
        mv = E.svc.client.get(f"/v1/items/{i}/movements?limit=200").json["movements"]
        if len(mv) < 200:
            eq(sum(m["delta"] for m in mv), q, f"item {i} movements sum")


def t_startup_without_database():
    """The service starts while MySQL is unreachable, reports not ready, and
    becomes ready once the database appears."""
    name = "inventory_it_late_" + rand_suffix(6)
    dbtool("create", ARGS.mysql_url, name)
    proxy = FaultProxy(*E.mysql_hostport)
    proxy.cut()
    svc = None
    try:
        svc = Service(via_proxy(with_database(ARGS.mysql_url, name), proxy.port), f"http://127.0.0.1:{E.stub.port}", wait_ready=False)
        c = svc.client
        expect(c.get("/healthz"), 200, "alive without database")
        r = expect(c.get("/readyz"), 503, "not ready without database")
        eq(r.json["checks"]["database"], "migrations pending", "readyz reason")
        expect(c.get("/v1/items/1"), 503, "item read without database")
        time.sleep(1)
        proxy.restore()
        wait_for(lambda: c.get("/readyz").status == 200, 20, "ready once the database is up")
        new_item(c)
        logs = svc.logs()
        check(any(l.get("msg") == "database not ready, retrying" for l in logs), "retry logged")
        check(any(l.get("msg") == "database ready" for l in logs), "ready logged")
    finally:
        if svc:
            svc.stop()
        proxy.cut()
        if not ARGS.keep_db:
            dbtool("drop", ARGS.mysql_url, name)


def t_graceful_shutdown():
    """SIGTERM while requests are in flight: they complete, new connections
    are refused, the process exits 0 and logs that it stopped."""
    name = "inventory_it_stop_" + rand_suffix(6)
    dbtool("create", ARGS.mysql_url, name)
    stub = Stub()
    try:
        svc = Service(with_database(ARGS.mysql_url, name), f"http://127.0.0.1:{stub.port}")
        # A request whose body is still arriving when SIGTERM comes: it is
        # answered, with Connection: close, before the process exits.
        body = json.dumps({"sku": "STOP-" + rand_suffix(), "name": "late", "category": "c", "price_cents": 1}).encode()
        s = socket.create_connection(("127.0.0.1", svc.port), timeout=10)
        s.sendall(b"POST /v1/items HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n" % len(body) + body[:10])
        time.sleep(0.3)
        svc.proc.send_signal(signal.SIGTERM)
        time.sleep(0.6)
        s.sendall(body[10:])
        out = b""
        while b"\r\n\r\n" not in out or not out.endswith(b"}"):
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
        s.close()
        check(out.startswith(b"HTTP/1.1 201"), f"in-flight request answered: {out[:100]!r}")
        check(b"Connection: close" in out, f"answer during drain closes the connection: {out[:200]!r}")
        rc = svc.wait_exit()
        eq(rc, 0, "exit status after SIGTERM")
        logs = svc.logs()
        check(any(l.get("msg") == "shutting down" for l in logs), "shutdown logged")
        check(any(l.get("msg") == "stopped" and l.get("abandoned_requests") == 0 for l in logs), "stopped cleanly")
        try:
            socket.create_connection(("127.0.0.1", svc.port), timeout=1).close()
            raise Fail("listener still open after exit")
        except OSError:
            pass
    finally:
        stub.stop()
        if not ARGS.keep_db:
            dbtool("drop", ARGS.mysql_url, name)


def t_config():
    # Invalid settings: exit status 2 and a structured error per problem.
    env = dict(os.environ, INVENTORY_PORT="99999", INVENTORY_LOG_LEVEL="loud", INVENTORY_RATES_URL="ftp://x")
    wrap = shlex.split(ARGS.wrap) if ARGS.wrap else []
    r = subprocess.run(wrap + [ARGS.service], env=env, capture_output=True, text=True, timeout=120)
    eq(r.returncode, 2, "exit status for invalid config")
    problems = [json.loads(l)["problem"] for l in r.stdout.splitlines() if l.startswith("{")]
    eq(len(problems), 3, f"three problems reported: {problems}")
    # A config file, overridden by the environment.
    with tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False) as f:
        f.write("# test config\nport = 1234\nlog_level = \"warn\"\n-- comment\nrates_ttl_ms=5000\n")
        path = f.name
    try:
        env = {k: v for k, v in os.environ.items() if not k.startswith("INVENTORY_")}
        r = subprocess.run(wrap + [ARGS.service, "--config", path, "--check-config"], env=env, capture_output=True, text=True, timeout=120)
        eq(r.returncode, 0, f"valid config file: {r.stdout} {r.stderr}")
        check(path in r.stdout, "config source reported")
        with open(path, "a") as f:
            f.write("this line is wrong\n")
        r = subprocess.run(wrap + [ARGS.service, "--config", path, "--check-config"], env=env, capture_output=True, text=True, timeout=120)
        eq(r.returncode, 2, "malformed config file")
        check("expected key = value" in r.stdout, "malformed line reported")
    finally:
        os.unlink(path)
    r = subprocess.run(wrap + [ARGS.service, "--bogus"], capture_output=True, text=True, timeout=120)
    eq(r.returncode, 2, "unknown argument")
    r = subprocess.run(wrap + [ARGS.service, "--version"], capture_output=True, text=True, timeout=120)
    check(r.stdout.startswith("inventory "), "version")


TESTS = [
    t_health_and_ready,
    t_database_sessions,
    t_create_and_get,
    t_create_errors,
    t_routing,
    t_list_filters_and_pagination,
    t_update,
    t_stock,
    t_stock_concurrency,
    t_report,
    t_rates_and_conversion,
    t_metrics,
    t_http_protocol_edges,
    t_keepalive_and_idle,
    t_concurrent_mixed_load,
    t_upstream_faults,
    t_mysql_outage,
    t_no_rates_at_all,
    t_startup_without_database,
    t_graceful_shutdown,
    t_config,
]


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--service", required=True)
    ap.add_argument("--stub", required=True)
    ap.add_argument("--dbtool", required=True)
    ap.add_argument("--mysql-url", required=True)
    ap.add_argument("--wrap", default="")
    ap.add_argument("--only", default="")
    ap.add_argument("--keep-db", action="store_true")
    ARGS = ap.parse_args()
    random.seed(os.getpid())
    only = set(x for x in ARGS.only.split(",") if x)
    tests = [t for t in TESTS if not only or t.__name__ in only or t.__name__[2:] in only]

    p = urllib.parse.urlsplit(ARGS.mysql_url)
    E.mysql_hostport = (p.hostname, p.port or 3306)
    E.db_name = "inventory_it_" + rand_suffix(6)
    dbtool("create", ARGS.mysql_url, E.db_name)
    E.proxy = FaultProxy(*E.mysql_hostport)
    E.stub = Stub()
    E.svc = None
    status = 0
    try:
        E.svc = Service(via_proxy(with_database(ARGS.mysql_url, E.db_name), E.proxy.port), f"http://127.0.0.1:{E.stub.port}")
        for t in tests:
            start = time.time()
            try:
                t()
                print(f"[PASS] {t.__name__[2:]} ({time.time() - start:.1f}s)", flush=True)
            except Exception as e:  # noqa: BLE001 - report every failure
                status = 1
                detail = str(e) if isinstance(e, Fail) else traceback.format_exc()
                print(f"[FAIL] {t.__name__[2:]} ({time.time() - start:.1f}s): {detail}", flush=True)
            if E.svc.proc.poll() is not None:
                print(f"[FAIL] service exited with {E.svc.proc.returncode}; stderr:\n{E.svc.stderr()[-4000:]}", flush=True)
                status = 1
                break
        if E.svc.proc.poll() is None:
            rc = E.svc.stop()
            if rc != 0:
                print(f"[FAIL] service exit status {rc}; stderr:\n{E.svc.stderr()[-4000:]}", flush=True)
                status = 1
        if MEMORY_REPORTS:
            for err in MEMORY_REPORTS:
                print(f"[FAIL] memory errors reported by the service:\n{err}", flush=True)
            status = 1
        elif ARGS.wrap:
            print("[PASS] no memory errors reported under " + ARGS.wrap.split()[0], flush=True)
    except Fail as e:
        print(f"[FAIL] setup: {e}", flush=True)
        status = 1
    finally:
        if E.svc and E.svc.proc.poll() is None:
            E.svc.stop(force=True)
        E.stub.stop()
        if not ARGS.keep_db:
            try:
                dbtool("drop", ARGS.mysql_url, E.db_name)
            except Fail as e:
                print(f"warning: {e}")
    print("PASS" if status == 0 else "FAILED", flush=True)
    return status


if __name__ == "__main__":
    sys.exit(main())
