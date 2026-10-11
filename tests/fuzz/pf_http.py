"""http programs (docs/networking.md): a server and its clients in one
program. The server listens on an ephemeral port of 127.0.0.1 and runs
in a task that accepts connections until the listener is closed, with one
handler task per connection serving requests while they ask for keep
alive. A handler answers with the status and the reply headers the path
asks for and an echo of what it parsed (method, path, query, body length
and checksum, a request header); /big/N answers N bytes, /slow/MS waits
first, and a request it cannot read gets 400. The clients are Http.request
(every method, custom headers, bodies of 0 bytes to 1 MiB, response size
limits, timeouts, forbidden framing headers), Http.get, Http.post_json,
HttpClient sessions (keep alive, several requests), concurrent spawned
clients, and raw TCP clients that send chunked bodies split at random,
HTTP/1.0 requests (accepted, Host optional),
requests broken in ways the manual says are rejected (a missing or second
Host, two Content-Length, Content-Length with chunked, a folded
header, a control character, a bad header name or method, a bad chunk
size, an oversized header) and connections that send nothing.

Every line is deterministic: clients run one after another (concurrent
ones print sorted results), the server's own count is printed once it
has stopped. A hang is caught by the run timeout.
"""
import random as _random
from pf_seg import Seg, SegProgram, iron_lit

HELPERS = '''func checksum(s: String) -> Int {
    var t = 0
    for i in range(s.byte_len()) {
        t = (t * 31 + s.byte_at(i)) % 1000003
    }
    return t
}

func status_of(path: String) -> Int {
    if path.starts_with("/s/") {
        return path.substring(3, path.len()).to_int()
    }
    return 200
}

func handle(c: HttpConnection) -> Int {
    var running = true
    var n = 0
    while running {
        val req = HttpConnection.read_request(c, 8192, 2097152, 3000)
        if req.error != 0 {
            if req.error != 1004 or n == 0 {
                val bad = Http.text_response(400, "bad")
                val s = HttpConnection.send_response(c, bad, 2000)
            }
            running = false
        } else {
            n += 1
            if req.path.starts_with("/slow/") {
                Time.sleep(req.path.substring(6, req.path.len()).to_int())
            }
            var body = "{req.method} {req.path} q={req.query} n={req.body.byte_len()} c={checksum(req.body)} x={Http.header(req.headers, "x-echo")}"
            if req.path.starts_with("/big/") {
                body = "z".repeat(req.path.substring(5, req.path.len()).to_int())
            }
            val st = status_of(req.path)
            val resp = Http.response(st, "Content-Type: text/plain\\u{d}\\nX-Reply: r{st}", body)
            -- /bye answers and ends the session (Connection: close)
            val keep = req.keep_alive and not req.path.starts_with("/bye")
            val s = HttpConnection.send_response_keep_alive(c, resp, keep, 3000)
            running = keep and s == 0
        }
    }
    HttpConnection.close(c)
    return n
}

func raw_exchange(port: Int, request: String, timeout: Int) -> String {
    val (sock, de) = Net.tcp_dial("127.0.0.1", port, 2000)
    if de.code != 0 {
        return "dial {de.code}"
    }
    if request.byte_len() > 0 {
        val (w, we) = TcpSocket.write(sock, request, 2000)
    }
    var got = ""
    var reading = true
    while reading {
        val (part, re) = TcpSocket.read(sock, 65536, timeout)
        if re.code != 0 or part.byte_len() == 0 {
            reading = false
        } else {
            got = got + part
        }
    }
    TcpSocket.close(sock)
    if got.byte_len() == 0 {
        return "none"
    }
    val head_end = got.index_of("\\u{d}\\n\\u{d}\\n")
    val line = got.split("\\u{d}\\n")[0]
    val parts = line.split(" ")
    if parts.len() < 2 or head_end < 0 {
        return "garbled"
    }
    val body = got.byte_slice(got.split("\\u{d}\\n\\u{d}\\n")[0].byte_len() + 4, got.byte_len())
    return "{parts[1]} {body}"
}
'''

STATUSES = [200, 200, 201, 202, 400, 401, 403, 404, 409, 418, 500, 503]
REASON_SAFE = True


def checksum(b):
    t = 0
    for x in b:
        t = (t * 31 + x) % 1000003
    return t


class HttpGen(SegProgram):
    HELPERS = HELPERS
    IMPORTS = ('http', 'net', 'time')
    NSEGS = (1, 1)

    def program(self):
        r = self.r
        lines, out = [], []
        lines += ['val l = Http.listen("127.0.0.1", 0)', 'if l.error != 0 {',
                  '    println("listen failed {l.error} {l.error_message}")', '    return', '}',
                  'val port = HttpServer.port(l.server)', 'val srv = l.server',
                  'val done = Channel[Int](1024)',
                  'val server = spawn("server") {', '    var conns = 0', '    var open = true',
                  '    while open {', '        val acc = HttpServer.accept(srv, 20000)',
                  '        if acc.error != 0 {', '            open = false', '        } else {',
                  '            conns += 1', '            val c = acc.connection',
                  '            spawn("conn") {', '                val n = handle(c)',
                  '                done.send(n)', '                return n', '            }',
                  '        }', '    }', '    return conns', '}',
                  'val base = "http://127.0.0.1:{port}"']
        self.conns = 0          # connections the clients open
        self.reqs = 0           # requests the server reads successfully
        for _ in range(r.randint(2, 8)):
            kind = r.choices(['request', 'get', 'post', 'session', 'parallel', 'raw', 'broken',
                              'silent', 'limit', 'forbidden', 'timeout'],
                             [5, 1, 1, 2, 2, 2, 3, 1, 1, 1, 1])[0]
            l2, o2 = getattr(self, 'c_' + kind)()
            lines += l2
            out += o2
        lines += ['HttpServer.close(srv)', 'val conns = await server',
                  'var finished = 0', 'for i in range(conns) {', '    finished += done.recv()', '}',
                  'println("server {conns} {finished}")']
        out.append(f'server {self.conns} {self.reqs}')
        self.segs = [Seg(lines, out)]

    # -------------------------------------------------------- pieces
    def path(self):
        r = self.r
        c = r.random()
        if c < 0.4:
            st = r.choice(STATUSES)
            p = f'/s/{st}'
        else:
            st = 200
            p = r.choice(['/', '/a', '/a/b/c', '/api/items/42', '/x-y_z.~', '/%41%20b'])
        q = r.choice(['', '', 'x=1', 'a=b&c=d', 'k=%20v', 'empty='])
        return p, q, st

    def body(self):
        r = self.r
        n = r.choice([0, 0, 1, 10, 100, 3000, 70000, 1 << 20])
        if n <= 10:
            s = ''.join(r.choice('abc xyz{}"\\é') for _ in range(n))
        else:
            s = ''.join(r.choice('abcdefgh') for _ in range(min(n, 64))) * (n // 64 + 1)
            s = s[:n]
        return s

    def body_expr(self, s):
        if len(s) <= 1000:
            return iron_lit(s)
        unit = s[:64]
        assert s == (unit * (len(s) // 64 + 1))[:len(s)]
        return f'{iron_lit(unit)}.repeat({len(s) // 64 + 1}).byte_slice(0, {len(s)})'

    def echo(self, method, p, q, b, xecho):
        bb = b.encode('utf-8')
        return f'{method} {p} q={q} n={len(bb)} c={checksum(bb)} x={xecho}'

    # ------------------------------------------------------- clients
    def c_request(self):
        r = self.r
        method = r.choice(['GET', 'POST', 'PUT', 'DELETE', 'PATCH', 'OPTIONS', 'HEAD'])
        p, q, st = self.path()
        b = self.body() if method not in ('GET', 'HEAD') or r.random() < 0.2 else ''
        xe = r.choice(['', 'hello', 'a b c', 'x' * 100])
        hdr = f'X-Echo: {xe}' if xe else ''
        if r.random() < 0.3:
            hdr = (hdr + '\\u{d}\\n' if hdr else '') + 'Accept: text/plain'
        v = self.fresh('r')
        url = p + ('?' + q if q else '')
        lines = [f'val {v} = Http.request("{method}", base + {iron_lit(url)}, "{hdr}", {self.body_expr(b)}, 4194304, 10000)',
                 f'println("{{{v}.error}} {{{v}.status}} {{Http.header({v}.headers, "x-reply")}} {{{v}.body}}")']
        self.conns += 1
        self.reqs += 1
        body = '' if method == 'HEAD' else self.echo(method, p, q, b, xe)
        return lines, [f'0 {st} r{st} {body}']

    def c_get(self):
        p, q, st = self.path()
        v = self.fresh('r')
        url = p + ('?' + q if q else '')
        self.conns += 1
        self.reqs += 1
        return ([f'val {v} = Http.get(base + {iron_lit(url)}, 10000)',
                 f'println("{{{v}.error}} {{{v}.status}} {{{v}.body}}")'],
                [f'0 {st} {self.echo("GET", p, q, "", "")}'])

    def c_post(self):
        r = self.r
        p, q, st = self.path()
        b = r.choice(['{}', '{"a":1}', '[1,2,3]', '{"k":"' + 'v' * 200 + '"}'])
        v = self.fresh('r')
        url = p + ('?' + q if q else '')
        self.conns += 1
        self.reqs += 1
        return ([f'val {v} = Http.post_json(base + {iron_lit(url)}, {iron_lit(b)}, 10000)',
                 f'println("{{{v}.error}} {{{v}.status}} {{{v}.body}}")'],
                [f'0 {st} {self.echo("POST", p, q, b, "")}'])

    def c_session(self):
        r = self.r
        cl = self.fresh('cl')
        lines = [f'val {cl} = HttpClient.open(base, "", false, 1, 30000)',
                 f'println("session {{{cl}.error}}")']
        out = ['session 0']
        bye = False
        for _ in range(r.randint(1, 5)):
            method = r.choice(['GET', 'POST', 'PUT', 'DELETE'])
            p, q, st = self.path()
            if bye:
                # the server closed the last connection: a new one opens
                self.conns += 1
                bye = False
            if r.random() < 0.2:
                p, st = '/bye', 200
                bye = True
            b = self.body() if method != 'GET' else ''
            if len(b) > 100000:
                b = b[:5000]
            v = self.fresh('r')
            url = p + ('?' + q if q else '')
            lines += [f'val {v} = HttpClient.request({cl}.client, "{method}", {iron_lit(url)}, "X-Echo: s", {self.body_expr(b)}, 4194304, 10000)',
                      f'println("{{{v}.error}} {{{v}.status}} {{{v}.body}}")']
            out.append(f'0 {st} {self.echo(method, p, q, b, "s")}')
            self.reqs += 1
        lines.append(f'HttpClient.close({cl}.client)')
        self.conns += 1
        return lines, out

    def c_parallel(self):
        r = self.r
        k = r.randint(2, 6)
        hs = []
        lines, outs = [], []
        for t in range(k):
            h = self.fresh('h')
            p, q, st = self.path()
            b = self.body()
            if len(b) > 100000:
                b = b[:3000]
            url = p + ('?' + q if q else '')
            lines += [f'val {h} = spawn("client{t}") {{',
                      f'    val rr = Http.request("PUT", base + {iron_lit(url)}, "X-Echo: t{t}", {self.body_expr(b)}, 4194304, 10000)',
                      f'    return "{{rr.error}} {{rr.status}} {{rr.body}}"', '}']
            hs.append(h)
            outs.append(f'0 {st} {self.echo("PUT", p, q, b, f"t{t}")}')
            self.conns += 1
            self.reqs += 1
        got = self.fresh('got')
        lines.append(f'var {got}: [String] = []')
        for h in hs:
            lines.append(f'{got}.push(await {h})')
        x = self.fresh('x')
        lines += [f'{got}.sort()', f'for {x} in {got} {{', f'    println({x})', '}']
        return lines, sorted(outs)

    def raw_valid(self, chunked):
        r = self.r
        p, q, st = self.path()
        b = self.body()
        if len(b) > 100000:
            b = b[:20000]
        bb = b.encode()
        target = p + ('?' + q if q else '')
        head = f'POST {target} HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Echo: raw\r\nConnection: close\r\n'
        if chunked:
            pieces = []
            i = 0
            while i < len(bb):
                n = r.randint(1, max(1, min(len(bb) - i, r.choice([1, 7, 100, 5000]))))
                ext = r.choice(['', '', ';ext=1'])
                pieces.append(f'{n:x}{ext}\r\n'.encode() + bb[i:i + n] + b'\r\n')
                i += n
            trailer = r.choice([b'', b'X-Trailer: t\r\n'])
            body_bytes = b''.join(pieces) + b'0\r\n' + trailer + b'\r\n'
            req = head.encode() + b'Transfer-Encoding: chunked\r\n\r\n' + body_bytes
        else:
            req = head.encode() + f'Content-Length: {len(bb)}\r\n\r\n'.encode() + bb
        return req, f'{st} {self.echo("POST", p, q, b, "raw")}'

    def raw_lit(self, req):
        """An Iron expression for raw request bytes (ASCII and UTF-8);
        literals are at most 4095 bytes, so a long one is joined from
        pieces."""
        s = req.decode('utf-8')
        if len(s) <= 1000:
            return iron_lit(s)
        return ' + '.join(iron_lit(s[i:i + 1000]) for i in range(0, len(s), 1000))

    def c_raw(self):
        req, want = self.raw_valid(self.r.random() < 0.6)
        v = self.fresh('raw')
        self.conns += 1
        self.reqs += 1
        return ([f'val {v} = raw_exchange(port, {self.raw_lit(req)}, 5000)', f'println({v})'], [want])

    def c_broken(self):
        r = self.r
        p, q, st = self.path()
        ok = f'GET {p} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n'
        kind = r.choice(['http10', 'nohost', 'twohost', 'twocl', 'clte', 'fold', 'ctl', 'name',
                         'method', 'chunk', 'bigheader', 'target'])
        if kind == 'http10':
            req = f'GET {p} HTTP/1.0\r\n' + r.choice(['Host: 127.0.0.1\r\n', '']) + '\r\n'
        elif kind == 'nohost':
            req = f'GET {p} HTTP/1.1\r\nConnection: close\r\n\r\n'
        elif kind == 'twohost':
            req = ok + 'Host: example.com\r\n\r\n'
        elif kind == 'twocl':
            req = ok.replace('GET', 'POST') + 'Content-Length: 3\r\nContent-Length: 4\r\n\r\nabcd'
        elif kind == 'clte':
            req = ok.replace('GET', 'POST') + 'Content-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n'
        elif kind == 'fold':
            req = ok + 'X-A: one\r\n  two\r\n\r\n'
        elif kind == 'ctl':
            req = ok + 'X-A: a\x01b\r\n\r\n'
        elif kind == 'name':
            req = ok + r.choice(['Bad Name: x', 'Bad:Name x', ': empty', 'X(y): z']) + '\r\n\r\n'
            if 'Bad:Name' in req:
                req = ok + 'Ba d: x\r\n\r\n'
        elif kind == 'method':
            req = r.choice(['G T', 'GE(T', '']) + f' {p} HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n'
        elif kind == 'chunk':
            req = ok.replace('GET', 'POST') + 'Transfer-Encoding: chunked\r\n\r\n' + r.choice(['zz', '-1', '']) + '\r\nabc\r\n0\r\n\r\n'
        elif kind == 'target':
            req = f'GET {r.choice(["nopath", "/a b", "/x" + chr(1)])} HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n'
        else:
            req = ok + 'X-Big: ' + 'h' * 9000 + '\r\n\r\n'
        v = self.fresh('raw')
        self.conns += 1
        want = '400 bad'
        if kind == 'http10':
            # HTTP/1.0 is accepted (Host optional, no keep alive by default)
            self.reqs += 1
            want = f'{st} {self.echo("GET", p, "", "", "")}'
        return ([f'val {v} = raw_exchange(port, {self.raw_lit(req.encode())}, 5000)', f'println({v})'],
                [want])

    def c_silent(self):
        """A connection that sends nothing (or half a request) and waits:
        the server's read times out and it answers 400."""
        r = self.r
        req = r.choice(['', 'GET / HTTP/1.1\r\nHost: 1'])
        v = self.fresh('raw')
        self.conns += 1
        return ([f'val {v} = raw_exchange(port, {iron_lit(req)}, 8000)', f'println({v})'], ['400 bad'])

    def c_limit(self):
        """A response larger than the client's limit is an error."""
        r = self.r
        n = r.choice([100, 5000, 100000])
        lim = n - r.randint(1, 50)
        v = self.fresh('r')
        self.conns += 2
        self.reqs += 2
        return ([f'val {v} = Http.request("GET", base + "/big/{n}", "", "", {lim}, 10000)',
                 f'val {v}ok = Http.request("GET", base + "/big/{n}", "", "", {n}, 10000)',
                 f'println("{{{v}.error != 0}} {{{v}.body.byte_len()}} {{{v}ok.error}} {{{v}ok.body.byte_len()}}")'],
                [f'true 0 0 {n}'])

    def c_forbidden(self):
        """Framing headers the client owns are rejected before sending."""
        r = self.r
        h = r.choice(['Host: x', 'Connection: keep-alive', 'Content-Length: 5', 'Transfer-Encoding: chunked',
                      'X-A: b\\u{d}\\nHost: y'])
        v = self.fresh('r')
        return ([f'val {v} = Http.request("POST", base + "/f", "{h}", "body", 1024, 5000)',
                 f'println("{{{v}.error != 0}} {{{v}.status}}")'], ['true 0'])

    def c_timeout(self):
        """The client gives up before a slow handler answers."""
        v = self.fresh('r')
        self.conns += 1
        self.reqs += 1
        return ([f'val {v} = Http.request("GET", base + "/slow/800", "", "", 1024, 200)',
                 f'println("{{{v}.error}} {{{v}.status}}")'], ['1004 0'])


def generate(rng):
    g = HttpGen(rng)
    g.program()
    return g
