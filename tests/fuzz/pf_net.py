"""The net module (manual 9.11, networking.md "TCP", "UDP", "DNS and
addresses"): loopback TCP servers and clients in one program, UDP pairs,
and IPv4 / IPv6 parsing and formatting.

TCP: a server task accepts connections and echoes length-prefixed frames
(10 ASCII digits, then the payload) until a zero-length frame; clients send
messages of random sizes (0 bytes to 300 KB of binary data with zero
bytes), written whole or in random fragments and read back in random read
sizes, sequentially or from up to 12 concurrent client tasks. Single writes
of up to 2 MB must be written whole within the deadline. Edge cases: a
port nobody listens on (1001), reads and accepts that time out (1004), end
of stream ("" with code 0), a peer that resets with unread data (1002),
invalid sizes and ports (1024), hosts holding a zero byte, reading from an
invalid descriptor (1015).

UDP: datagrams of 0 .. 65507 bytes to a bound receiver, received with
limits above and below their size (truncated, 1018), timeouts (1004),
invalid limits (1024), a datagram too large to send (1018), invalid ports
(1024) and octets outside 0..255 (1008); the same over the IPv6 loopback
(Net.udp_sendto_v6) where there is one.

DNS: Net.lookup_host on address literals, which need no resolver (one
address, the literal), and on the empty name and names holding a zero
byte (1009).

Addresses: IPv4Addr / IPv6Addr parse against the platform inet_pton
(socket.inet_pton in Python), zone identifiers (with the %25 URL form), and
format against inet_ntop; a zero byte, an empty zone or a zone over 63
bytes is 1008.
"""
import socket
import sys

from stdfz_common import StdGen, Probe, lit, qtext, b2s
from pf_iofs import blob, digest

HELPERS = '''func digest(s: String) -> Int {
    var h = 7
    var i = 0
    while i < s.byte_len() {
        h = (h * 31 + s.byte_at(i)) % 1000000007
        i += 1
    }
    return h
}

func blob(n: Int, seed: Int) -> String {
    -- a 997-byte pseudo-random chunk repeated (an append per byte would
    -- be quadratic, #597)
    var x = seed
    var chunk = ""
    for i in range(min(n, 997)) {
        x = (x * 1103515245 + 12345) % 2147483648
        chunk += String.from_byte(x / 65536 % 256)
    }
    if n <= 997 {
        return chunk
    }
    return chunk.repeat(n / 997) + chunk.byte_slice(0, n % 997)
}

func listen_any() -> (TcpListener, Int) {
    var p = 20000 + (Time.now_ns() / 1000) % 40000
    var tries = 0
    while tries < 300 {
        val (l, e) = Net.tcp_listen("127.0.0.1", p)
        if e.code == 0 {
            return (l, p)
        }
        p = 20000 + (p - 20000 + 7919) % 40000
        tries += 1
    }
    return (TcpListener(-1), -1)
}

func udp_any() -> (UdpSocket, Int) {
    var p = 20000 + (Time.now_ns() / 1000 + 17) % 40000
    var tries = 0
    while tries < 300 {
        val (u, e) = Net.udp_bind("127.0.0.1", p)
        if e.code == 0 {
            return (u, p)
        }
        p = 20000 + (p - 20000 + 7919) % 40000
        tries += 1
    }
    return (UdpSocket(-1), -1)
}

func addrs_text(addrs: [Address]) -> String {
    var t = ""
    for a in addrs {
        match a {
            Address.V4(v) -> { t += " v4=" + IPv4Addr.format(v) }
            Address.V6(v) -> { t += " v6=" + IPv6Addr.format(v) }
        }
    }
    return t
}

func udp6_any() -> (UdpSocket, Int) {
    var p = 20000 + (Time.now_ns() / 1000 + 29) % 40000
    var tries = 0
    while tries < 300 {
        val (u, e) = Net.udp_bind("::1", p)
        if e.code == 0 {
            return (u, p)
        }
        p = 20000 + (p - 20000 + 7919) % 40000
        tries += 1
    }
    return (UdpSocket(-1), -1)
}

func write_all(s: TcpSocket, data: String, chunk: Int) -> Int {
    var off = 0
    val n = data.byte_len()
    while off < n {
        var end = n
        if chunk > 0 and off + chunk < n {
            end = off + chunk
        }
        val (w, e) = TcpSocket.write(s, data.byte_slice(off, end), 10000)
        if e.code != 0 {
            return e.code
        }
        if w != end - off {
            return -2
        }
        off = end
    }
    return 0
}

func read_exact(s: TcpSocket, n: Int, maxr: Int) -> (String, Int) {
    var out = ""
    while out.byte_len() < n {
        val (d, e) = TcpSocket.read(s, min(maxr, n - out.byte_len()), 10000)
        if e.code != 0 {
            return (out, e.code)
        }
        if d.byte_len() == 0 {
            return (out, -1)
        }
        out += d
    }
    return (out, 0)
}

func frame(n: Int) -> String {
    return "{n}".pad_left(10, "0")
}

func echo_conn(s: TcpSocket) -> Int {
    var served = 0
    var going = true
    while going {
        val (h, e) = read_exact(s, 10, 10)
        if e != 0 {
            going = false
            served = -100 - served
        } else {
            val n = h.to_int()
            if n == 0 {
                going = false
            } else {
                val (body, e2) = read_exact(s, n, 65536)
                if e2 != 0 {
                    going = false
                    served = -100 - served
                } else {
                    if write_all(s, h + body, 0) != 0 {
                        going = false
                    }
                    served += 1
                }
            }
        }
    }
    TcpSocket.close(s)
    return served
}

func serve(l: TcpListener, conns: Int) -> Int {
    var total = 0
    for i in range(conns) {
        val (c, e) = TcpListener.accept(l, 20000)
        if e.code != 0 {
            return -1000 - e.code
        }
        total += echo_conn(c)
    }
    TcpListener.close(l)
    return total
}

func drain(l: TcpListener, n: Int) -> Int {
    val (c, e) = TcpListener.accept(l, 20000)
    if e.code != 0 {
        return -1
    }
    val (d, e2) = read_exact(c, n, 1048576)
    TcpSocket.close(c)
    TcpListener.close(l)
    return digest(d)
}'''


MAX_DATAGRAM = 9216 if sys.platform == 'darwin' else 65507


def _have_v6():
    try:
        with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as s:
            s.bind(('::1', 0))
        return True
    except OSError:
        return False


HAVE_V6 = _have_v6()


def frame(n):
    return b'%010d' % n


class NetGen(StdGen):
    NPROBES = (3, 7)

    def __init__(self, rng):
        super().__init__(rng)
        for m in ('net', 'time'):
            self.need_import(m)
        self.need('net', HELPERS)

    def msg(self):
        r = self.r
        n = r.choice([0, 1, 2, 9, 10, 100, 1460, 4096, 65535, 65536, 65537, 100000, 300000])
        if n == 0:
            return None
        seed = r.randint(1, 10**6)
        return n, seed

    def p_echo(self):
        r = self.r
        code, out = [], []
        nclients = r.choice([1, 1, 2, 3])
        conc = nclients > 1 and r.random() < 0.5
        code += ['val (l, port) = listen_any()',
                 f'val srv = spawn("srv") {{', f'    return serve(l, {nclients})', '}']
        total = 0
        summaries = []
        for k in range(nclients):
            msgs = [m for m in (self.msg() for _ in range(r.randint(0, 4))) if m]
            if conc:
                msgs = [m for m in msgs if m[0] <= 60000]   # all fit the socket buffers
            body = [f'val (c, e) = Net.tcp_dial("127.0.0.1", port, 5000)', 'var res = "{e.code}"']
            exp = '0'
            for (n, seed) in msgs:
                chunk = r.choice([0, 0, 1, 7, 1000, 4096])
                if n > 20000 and chunk in (1, 7):
                    chunk = 999
                rd = r.choice([1, 3, 10, 512, 65536, 1000000])
                if n + 10 > 20000 and rd < 512:
                    rd = 512
                body += ['{', f'    val m = blob({n}, {seed})',
                         f'    val werr = write_all(c, frame({n}) + m, {chunk})',
                         f'    val (back, rerr) = read_exact(c, {n + 10}, {rd})',
                         '    res += " {werr} {rerr} {back.byte_len()} {digest(back)}"', '}']
                b = frame(n) + blob(n, seed)
                exp += f' 0 0 {n + 10} {digest(b)}'
            body += ['res += " {write_all(c, frame(0), 0)}"', 'TcpSocket.close(c)']
            exp += ' 0'
            total += len(msgs)
            summaries.append((body, exp))
        for k, (body, exp) in enumerate(summaries):
            if conc:
                code += [f'val cl{k} = spawn("c{k}") {{'] + ['    ' + x for x in body] + ['    return res', '}']
            else:
                tag = self.fresh('t')
                code += ['{'] + ['    ' + x for x in body] + [f'    println("{tag} " + res)', '}']
                out.append(f'{tag} {exp}')
        if conc:
            for k, (body, exp) in enumerate(summaries):
                tag = self.fresh('t')
                code.append(f'println("{tag} " + await cl{k})')
                out.append(f'{tag} {exp}')
        self.raw(code, out, ['await srv'], [total])
        return Probe(code, out)

    def p_bigwrite(self):
        r = self.r
        code, out = [], []
        n = r.choice([100000, 1000000, 2000000])
        seed = r.randint(1, 10**6)
        code += ['val (l, port) = listen_any()', f'val dr = spawn("drain") {{',
                 f'    return drain(l, {n})', '}',
                 'val (c, e) = Net.tcp_dial("127.0.0.1", port, 5000)',
                 f'val big = blob({n}, {seed})', 'val (w, we) = TcpSocket.write(c, big, 20000)',
                 'TcpSocket.close(c)']
        self.raw(code, out, ['e.code', 'w', 'we.code', 'await dr'], [0, n, 0, digest(blob(n, seed))])
        return Probe(code, out)

    def p_many(self):
        r = self.r
        code, out = [], []
        n = r.choice([4, 8, 12])
        code += ['val (l, port) = listen_any()', f'val srv = spawn("srv") {{', f'    return serve(l, {n})', '}']
        exps = []
        for k in range(n):
            sz = r.choice([1, 10, 500, 3000])
            seed = r.randint(1, 10**6)
            code += [f'val m{k} = spawn("m{k}") {{', '    val (c, e) = Net.tcp_dial("127.0.0.1", port, 10000)',
                     f'    val werr = write_all(c, frame({sz}) + blob({sz}, {seed}) + frame(0), 0)',
                     f'    val (back, rerr) = read_exact(c, {sz + 10}, 4096)',
                     '    TcpSocket.close(c)',
                     '    return "{e.code} {werr} {rerr} {digest(back)}"', '}']
            exps.append(f'0 0 0 {digest(frame(sz) + blob(sz, seed))}')
        for k in range(n):
            tag = self.fresh('t')
            code.append(f'println("{tag} " + await m{k})')
            out.append(f'{tag} {exps[k]}')
        self.raw(code, out, ['await srv'], [n])
        return Probe(code, out)

    def p_edges(self):
        r = self.r
        code, out = [], []
        k = r.choice(['refused', 'read_timeout', 'accept_timeout', 'eof', 'reset', 'badargs',
                      'badfd', 'badhost'])
        if k == 'refused':
            code += ['val (l, port) = listen_any()', 'TcpListener.close(l)',
                     'val (c, e) = Net.tcp_dial("127.0.0.1", port, 2000)']
            self.raw(code, out, ['e.code'], [1001])
        elif k == 'read_timeout':
            to = r.choice([0, 1, 30])
            code += ['val (l, port) = listen_any()', 'val (c, e) = Net.tcp_dial("127.0.0.1", port, 2000)',
                     'val (sc, ae) = TcpListener.accept(l, 2000)',
                     f'val (d, re) = TcpSocket.read(c, 100, {to})',
                     f'val (d2, re2) = TcpSocket.read(sc, 100, {to})',
                     'TcpSocket.close(c)', 'TcpSocket.close(sc)', 'TcpListener.close(l)']
            self.raw(code, out, ['e.code', 'ae.code', 're.code', 'd.byte_len()', 're2.code'],
                     [0, 0, 1004, 0, 1004])
        elif k == 'accept_timeout':
            to = r.choice([0, 1, 25])
            code += ['val (l, port) = listen_any()', f'val (c, e) = TcpListener.accept(l, {to})',
                     'TcpListener.close(l)']
            self.raw(code, out, ['e.code', 'c.fd'], [1004, -1])
        elif k == 'eof':
            code += ['val (l, port) = listen_any()', 'val (c, e) = Net.tcp_dial("127.0.0.1", port, 2000)',
                     'val (sc, ae) = TcpListener.accept(l, 2000)',
                     'val (w, we) = TcpSocket.write(sc, "bye", 1000)', 'TcpSocket.close(sc)',
                     'val (d, re) = read_exact(c, 3, 100)', 'val (d2, re2) = TcpSocket.read(c, 100, 2000)',
                     'TcpSocket.close(c)', 'TcpListener.close(l)']
            self.raw(code, out, ['w', 'we.code', 'd', 're', 'd2.byte_len()', 're2.code'],
                     [3, 0, 'bye', 0, 0, 0])
        elif k == 'reset':
            code += ['val (l, port) = listen_any()', 'val (c, e) = Net.tcp_dial("127.0.0.1", port, 2000)',
                     'val (sc, ae) = TcpListener.accept(l, 2000)',
                     'val (w, we) = TcpSocket.write(c, "unread data", 1000)', 'Time.sleep(20)',
                     'TcpSocket.close(sc)', 'Time.sleep(20)',
                     'val (d, re) = TcpSocket.read(c, 100, 2000)',
                     'var werr = 0', 'for i in range(50) {', '    if werr == 0 {',
                     '        val (w2, we2) = TcpSocket.write(c, "more", 500)', '        werr = we2.code',
                     '    }', '}', 'TcpSocket.close(c)', 'TcpListener.close(l)']
            self.raw(code, out, ['re.code', 'd.byte_len()', 'werr'], [1002, 0, 1002])
        elif k == 'badargs':
            mx = r.choice([-1, -100, -9223372036854775807])
            port = r.choice([-1, 65536, 70000, 9223372036854775807])
            code += ['val (l, lport) = listen_any()', 'val (c, e) = Net.tcp_dial("127.0.0.1", lport, 2000)',
                     'val (sc, ae) = TcpListener.accept(l, 2000)',
                     f'val (d, re) = TcpSocket.read(c, {mx}, 100)', 'val (d0, re0) = TcpSocket.read(c, 0, 100)',
                     'val (w0, we0) = TcpSocket.write(c, "", 100)',
                     f'val (c2, de) = Net.tcp_dial("127.0.0.1", {port}, 500)',
                     f'val (l2, le) = Net.tcp_listen("127.0.0.1", {port})',
                     'val (big, be) = TcpSocket.read(sc, 9223372036854775806, 10)',
                     'TcpSocket.close(c)', 'TcpSocket.close(sc)', 'TcpListener.close(l)']
            self.raw(code, out, ['re.code', 'd.byte_len()', 're0.code', 'w0', 'we0.code', 'de.code', 'c2.fd',
                                 'le.code', 'be.code'],
                     [1024, 0, 0, 0, 0, 1024, -1, 1024, 1004])
        elif k == 'badfd':
            code += ['val s = TcpSocket(-1)', 'val (d, re) = TcpSocket.read(s, 10, 10)',
                     'val (w, we) = TcpSocket.write(s, "x", 10)', 'TcpSocket.close(s)']
            self.raw(code, out, ['re.code', 'we.code'], [1015, 1015])
        else:
            h = r.choice(['127.0.0.1' + '\x00' + 'evil', '\x00'])
            code += ['val (l, port) = listen_any()',
                     f'val (c, e) = Net.tcp_dial({lit(h)}, port, 500)',
                     f'val (l2, le) = Net.tcp_listen({lit(h)}, 0)', 'TcpListener.close(l)']
            self.raw(code, out, ['e.code', 'le.code'], [1007, 1007])
        return Probe(code, out)

    def p_udp(self):
        r = self.r
        code, out = [], []
        code += ['val (rx, rport) = udp_any()', 'val (tx, te) = Net.udp_bind("127.0.0.1", 0)',
                 'val (lo, pe) = IPv4Addr.parse("127.0.0.1")']
        for _ in range(r.randint(1, 4)):
            k = r.random()
            if k < 0.6:
                # (macOS sends at most net.inet.udp.maxdgram = 9216 bytes
                # by default: larger datagrams fail with 1018 there)
                n = r.choice([0, 1, 100, 1472, 8192, MAX_DATAGRAM])
                seed = r.randint(1, 10**6)
                mx = max(1, r.choice([n, n + 1, 65536, 1000000, n - 1, 1]))
                d = blob(n, seed)
                got = d[:mx]
                trunc = len(d) > mx
                pk = self.fresh('pk')
                code += [f'val (sn{pk}, se{pk}) = Net.udp_sendto_v4(tx, blob({n}, {seed}), lo, rport, 1000)',
                         f'val {pk} = UdpSocket.recvfrom(rx, {mx}, 2000)']
                self.raw(code, out, [f'sn{pk}', f'se{pk}.code', f'{pk}.error.code', f'{pk}.truncated',
                                     f'{pk}.data.byte_len()', f'digest({pk}.data)', f'{pk}.address'],
                         [n, 0, 1018 if trunc else 0, 1 if trunc else 0, len(got), digest(got), '127.0.0.1'])
            elif k < 0.7:
                pk = self.fresh('pk')
                code.append(f'val {pk} = UdpSocket.recvfrom(rx, 100, {r.choice([0, 1, 20])})')
                self.raw(code, out, [f'{pk}.error.code', f'{pk}.data.byte_len()'], [1004, 0])
            elif k < 0.8:
                pk = self.fresh('pk')
                code.append(f'val {pk} = UdpSocket.recvfrom(rx, {r.choice([0, -1, 2147483648])}, 10)')
                self.raw(code, out, [f'{pk}.error.code'], [1024])
            elif k < 0.88:
                pk = self.fresh('pk')
                code.append(f'val (sn{pk}, se{pk}) = Net.udp_sendto_v4(tx, blob(65508, 1), lo, rport, 1000)')
                self.raw(code, out, [f'se{pk}.code'], [1018])
            elif k < 0.95:
                pk = self.fresh('pk')
                port = r.choice([-1, 65536, 70000])
                code.append(f'val (sn{pk}, se{pk}) = Net.udp_sendto_v4(tx, "x", lo, {port}, 100)')
                code.append(f'val (ub{pk}, ue{pk}) = Net.udp_bind("127.0.0.1", {port})')
                self.raw(code, out, [f'se{pk}.code', f'ue{pk}.code'], [1024, 1024])
            else:
                pk = self.fresh('pk')
                o = r.choice([256, -1, 1000])
                code.append(f'val (sn{pk}, se{pk}) = Net.udp_sendto_v4(tx, "x", IPv4Addr(127, 0, 0, {o}), rport, 100)')
                self.raw(code, out, [f'se{pk}.code', f'q(IPv4Addr.format(IPv4Addr({o}, 0, 0, 1)))'], [1008, ''])
        code += ['UdpSocket.close(tx)', 'UdpSocket.close(rx)']
        return Probe(code, out)

    def p_udp6(self):
        """UDP over the IPv6 loopback (where the machine generating the
        program has one)."""
        r = self.r
        code, out = [], []
        code += ['val (rx, re) = Net.udp_bind("::1", 0)', 'val (tx, te) = Net.udp_bind("::1", 0)',
                 'val (lo, pe) = IPv6Addr.parse("::1")']
        if not HAVE_V6:
            code += ['UdpSocket.close(tx)', 'UdpSocket.close(rx)']
            self.raw(code, out, ['pe.code', 'q(IPv6Addr.format(lo))'], [0, '::1'])
            return Probe(code, out)
        # the receiver's port: found by sending to candidate ports is not
        # possible, so bind the receiver on a searched port instead
        code[0] = 'val (rx, rport) = udp6_any()'
        for _ in range(r.randint(1, 3)):
            k = r.random()
            pk = self.fresh('pk')
            if k < 0.7:
                n = r.choice([0, 1, 100, 1452, 8192])
                seed = r.randint(1, 10**6)
                mx = max(1, r.choice([n, n + 1, 65536, n - 1, 1]))
                d = blob(n, seed)
                got = d[:mx]
                trunc = len(d) > mx
                code += [f'val (sn{pk}, se{pk}) = Net.udp_sendto_v6(tx, blob({n}, {seed}), lo, rport, 1000)',
                         f'val {pk} = UdpSocket.recvfrom(rx, {mx}, 2000)']
                self.raw(code, out, [f'sn{pk}', f'se{pk}.code', f'{pk}.error.code', f'{pk}.truncated',
                                     f'{pk}.data.byte_len()', f'digest({pk}.data)', f'{pk}.address'],
                         [n, 0, 1018 if trunc else 0, 1 if trunc else 0, len(got), digest(got), '::1'])
            elif k < 0.85:
                port = r.choice([-1, 65536, 70000])
                code.append(f'val (sn{pk}, se{pk}) = Net.udp_sendto_v6(tx, "x", lo, {port}, 100)')
                code.append(f'val (ub{pk}, ue{pk}) = Net.udp_bind("::1", {port})')
                self.raw(code, out, [f'se{pk}.code', f'ue{pk}.code'], [1024, 1024])
            else:
                # 16 raw bytes make an address; any other length does not
                n = r.choice([0, 4, 15, 17])
                code.append(f'val (sn{pk}, se{pk}) = Net.udp_sendto_v6(tx, "x", IPv6Addr("a".repeat({n}), ""), rport, 100)')
                self.raw(code, out, [f'se{pk}.code'], [1008])
        code += ['UdpSocket.close(tx)', 'UdpSocket.close(rx)']
        return Probe(code, out)

    # ------------------------------------------------------------------ DNS
    def p_lookup(self):
        """Net.lookup_host on names that need no resolver: address
        literals (one address, the literal), the empty name and names
        holding a zero byte (1009)."""
        r = self.r
        code, out = [], []
        for _ in range(r.randint(1, 4)):
            k = r.random()
            a, e = self.fresh('la'), self.fresh('le')
            if k < 0.35:
                s = '.'.join(str(r.choice([0, 1, 10, 127, 192, 255])) for _ in range(4))
                want = [0, 1, ' v4=' + s]
            elif k < 0.6:
                s = r.choice(['::1', '::', '2001:db8::7', '1:2:3:4:5:6:7:8', '::ffff:1.2.3.4',
                              '2001:0db8:0000:0000:0000:0000:0000:0007', 'FE80::ABCD'])
                want = [0, 1, ' v6=' + socket.inet_ntop(socket.AF_INET6, socket.inet_pton(socket.AF_INET6, s))]
            elif k < 0.7:
                s = ''
                want = [1009, 0, '']
            else:
                s = r.choice(['127.0.0.1\x00evil', '\x00', '::1\x00', 'localhost\x00.example',
                              '10.0.0.1\x00', 'a\x00b'])
                want = [1009, 0, '']
            code.append(f'val ({a}, {e}) = Net.lookup_host({lit(s)}, 5000)')
            self.raw(code, out, [f'{e}.code', f'{a}.len()', f'q(addrs_text({a}))'],
                     [want[0], want[1], qtext(want[2].encode())])
        return Probe(code, out)

    # ------------------------------------------------------------ addresses
    V4 = ['127.0.0.1', '0.0.0.0', '255.255.255.255', '256.1.1.1', '1.2.3', '1.2.3.4.5', '01.2.3.4',
          '1.2.3.04', ' 1.2.3.4', '1.2.3.4 ', '1..2.3', '', '1.2.3.4\x00', '\x00', '192.168.1.255',
          '1.2.3.-1', '0x1.2.3.4', '1.2.3.4/24', '999.999.999.999', '000.000.000.000', '10.0.0.1',
          '1.2.3.4\x00junk']
    V6 = ['::1', '::', 'fe80::1', '2001:db8::7', '2001:0db8:0000:0000:0000:0000:0000:0007',
          '::ffff:1.2.3.4', '1:2:3:4:5:6:7:8', '1:2:3:4:5:6:7:8:9', ':::', '1::2::3', 'fe80::1%eth0',
          'fe80::1%25eth0', 'fe80::1%', 'fe80::1%25', '::1%lo', 'gggg::1', '::1\x00', '', '1:2:3:4:5:6:7::',
          'FE80::ABCD', '::ffff:256.1.1.1', '[::1]', 'fe80::1%' + 'z' * 63, 'fe80::1%' + 'z' * 64,
          '0:0:0:0:0:0:0:1', '1:0:0:2:0:0:0:3', 'fe80::1%a%b', 'fe80::1%\x00x']

    @staticmethod
    def v4_model(s):
        if not s or len(s.encode()) >= 16 or '\x00' in s:
            return None
        try:
            return socket.inet_pton(socket.AF_INET, s)
        except OSError:
            return None

    @staticmethod
    def v6_model(s):
        b = s.encode()
        if not b or len(b) >= 46 + 64 or b'\x00' in b:
            return None
        pct = b.find(b'%')
        addr = b if pct < 0 else b[:pct]
        zone = None if pct < 0 else b[pct + 1:]
        if zone is not None and zone.startswith(b'25') and len(zone) >= 2:
            zone = zone[2:]
        if not addr or len(addr) >= 46 or (zone is not None and (len(zone) > 63 or len(zone) == 0)):
            return None
        try:
            raw = socket.inet_pton(socket.AF_INET6, addr.decode('latin-1'))
        except (OSError, UnicodeError, ValueError):
            return None
        return raw, zone or b''

    def p_addr(self):
        r = self.r
        code, out = [], []
        for _ in range(r.randint(2, 6)):
            if r.random() < 0.5:
                s = r.choice(self.V4)
                if r.random() < 0.2:
                    s = '.'.join(str(r.choice([0, 1, 9, 10, 99, 100, 199, 200, 249, 250, 255, 256]))
                                 for _ in range(4))
                v = self.v4_model(s)
                a, e = self.fresh('a'), self.fresh('e')
                code.append(f'val ({a}, {e}) = IPv4Addr.parse({lit(s)})')
                if v is None:
                    self.raw(code, out, [f'{e}.code', f'{a}.a + {a}.b + {a}.c + {a}.d'], [1008, 0])
                else:
                    self.raw(code, out, [f'{e}.code', f'{a}.a', f'{a}.b', f'{a}.c', f'{a}.d',
                                         f'IPv4Addr.format({a})'],
                             [0, v[0], v[1], v[2], v[3], socket.inet_ntop(socket.AF_INET, v)])
            else:
                s = r.choice(self.V6)
                v = self.v6_model(s)
                a, e = self.fresh('a'), self.fresh('e')
                code.append(f'val ({a}, {e}) = IPv6Addr.parse({lit(s)})')
                if v is None:
                    self.raw(code, out, [f'{e}.code', f'{a}.bytes.byte_len()'], [1008, 0])
                else:
                    raw, zone = v
                    txt = socket.inet_ntop(socket.AF_INET6, raw)
                    if zone:
                        txt += '%' + zone.decode('latin-1')
                    self.raw(code, out, [f'{e}.code', f'q({a}.bytes)', f'q({a}.zone)', f'q(IPv6Addr.format({a}))'],
                             [0, qtext(raw), qtext(zone), qtext(txt.encode('latin-1'))])
        return Probe(code, out)

    def probe_kinds(self):
        return [(5, self.p_echo), (1, self.p_bigwrite), (1, self.p_many), (4, self.p_edges),
                (3, self.p_udp), (4, self.p_addr), (1, self.p_udp6), (2, self.p_lookup)]


def generate(rng):
    g = NetGen(rng)
    g.program()
    return g
