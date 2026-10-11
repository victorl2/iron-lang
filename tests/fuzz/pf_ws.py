"""The websocket module (networking.md "WebSocket and secure WebSocket",
src/stdlib/websocket.iron) against a scripted Python peer that sends
malformed input.

The peer runs in the process that runs the programs (it starts when this
module is imported and publishes its ports in IRONFZ_WS_PORT and
IRONFZ_WS_CTL). A program either connects to it as a client (the URL path
carries the peer's script) or serves a connection it asks the peer to open
(`SERVE port script nonce` on the control port). The script fixes the
handshake (valid, a wrong accept key, a 200 status, a missing Upgrade, an
extension or unsolicited protocol, the response split byte by byte or sent
together with the first frames; on the server side a wrong version, a bad
or missing key) and the frames: text (valid and invalid UTF-8) and binary
messages of 0 bytes to 200 KB, fragmented with pings and pongs between the
fragments, close frames with valid and invalid codes and reasons, and every
framing error (RSV bits, reserved opcodes, fragmented or oversized control
frames, a continuation with nothing to continue, a new message inside a
fragmented one, the wrong mask bit, non-minimal lengths, a 64-bit length
with the top bit set, messages over max_message_bytes, a stream cut in the
middle of a frame). The bytes go out in random chunks.

The model (receive) follows RFC 6455 and the limits websocket.iron
documents: each receive returns the next message or control frame, a ping is
answered with a pong, a close is answered with a close; a framing error is
6002 (and a close 1002), too large 6003 (close 1009), invalid UTF-8 6004
(close 1007), a cut stream 6005. After a failed receive the connection is
failed: later receives and sends give 6005 (the stream would be out of
step), as do all calls after close / abort (the handle is stale: #607).
The peer reports the frames the program sent it (pongs, closes and their
codes, data, pings, and whether every frame was masked as the role
requires), and the program prints that report.
"""
import base64
import hashlib
import json
import os
import random
import socket
import struct
import threading
import time
import zlib

from stdfz_common import StdGen, Probe, lit, b2s
from pf_net import HELPERS as NET_HELPERS  # noqa: F401  (digest / blob)
from pf_iofs import blob, digest

GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'


def accept_key(key):
    return base64.b64encode(hashlib.sha1(key + GUID).digest())


def enc_script(obj):
    return base64.urlsafe_b64encode(zlib.compress(json.dumps(obj, separators=(',', ':')).encode(), 9)).decode().rstrip('=')


def dec_script(s):
    s += '=' * (-len(s) % 4)
    return json.loads(zlib.decompress(base64.urlsafe_b64decode(s)))


def payload_of(p):
    """A payload spec: ['b', n, seed] binary blob, ['t', text] utf-8 text,
    ['x', hex] raw bytes."""
    if p[0] == 'b':
        return blob(p[1], p[2])
    if p[0] == 't':
        return p[1].encode('utf-8')
    if p[0] == 's':
        return payload_of(p[1])[p[2]:p[3]]
    return bytes.fromhex(p[1])


def frame_bytes(f, mask_it, rng):
    """Serialize a frame spec {fin, rsv, op, p, mask?, len126?, len127?}."""
    data = payload_of(f['p'])
    fin = f.get('fin', 1)
    b0 = (0x80 if fin else 0) | (f.get('rsv', 0) << 4) | f['op']
    masked = f.get('mask', mask_it)
    n = len(data)
    if f.get('enc') == 126 or (126 <= n <= 0xffff and f.get('enc') != 127):
        hdr = bytes([b0, (0x80 if masked else 0) | 126]) + struct.pack('>H', n)
    elif f.get('enc') == 127 or n > 0xffff:
        ln = n | (1 << 63) if f.get('top') else n
        hdr = bytes([b0, (0x80 if masked else 0) | 127]) + struct.pack('>Q', ln)
    else:
        hdr = bytes([b0, (0x80 if masked else 0) | n])
    if masked:
        key = bytes(rng.randint(0, 255) for _ in range(4))
        data = bytes(b ^ key[i & 3] for i, b in enumerate(data)) if n < 300000 else data
        hdr += key
    if f.get('cut') is not None:
        return (hdr + data)[:f['cut']]
    return hdr + data


# ------------------------------------------------------------------- peer

class Peer:
    def __init__(self):
        self.reports = {}
        self.cv = threading.Condition()
        self.ws = socket.socket()
        self.ws.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ws.bind(('127.0.0.1', 0))
        self.ws.listen(256)
        self.ctl = socket.socket()
        self.ctl.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ctl.bind(('127.0.0.1', 0))
        self.ctl.listen(256)
        for target in (self.ws_loop, self.ctl_loop):
            threading.Thread(target=target, daemon=True).start()

    def report(self, nonce, text):
        with self.cv:
            self.reports[nonce] = text
            self.cv.notify_all()

    def ws_loop(self):
        while True:
            c, _ = self.ws.accept()
            threading.Thread(target=self.client_session, args=(c,), daemon=True).start()

    def ctl_loop(self):
        while True:
            c, _ = self.ctl.accept()
            threading.Thread(target=self.ctl_session, args=(c,), daemon=True).start()

    @staticmethod
    def read_line(c):
        buf = b''
        while not buf.endswith(b'\n'):
            ch = c.recv(1)
            if not ch:
                break
            buf += ch
        return buf.decode().strip()

    def ctl_session(self, c):
        c.settimeout(30)
        try:
            line = self.read_line(c)
            parts = line.split(' ')
            if parts[0] == 'REPORT':
                nonce = parts[1]
                deadline = time.time() + 20
                with self.cv:
                    while nonce not in self.reports and time.time() < deadline:
                        self.cv.wait(0.2)
                    text = self.reports.pop(nonce, 'NOREPORT')
                c.sendall((text + '\n').encode())
            elif parts[0] == 'SERVE':
                port, script, nonce = int(parts[1]), dec_script(parts[2]), parts[3]
                c.sendall(b'ok\n')
                threading.Thread(target=self.server_session, args=(port, script, nonce),
                                 daemon=True).start()
        except OSError:
            pass
        finally:
            c.close()

    def run_frames(self, c, script, mask_it, record, rng):
        """Send the frames (in random chunks) while a reader records what
        the program sends; returns when the program closed or went away."""
        out = b''.join(frame_bytes(f, mask_it, rng) for f in script['frames'])
        if script.get('prefix'):
            out = script['prefix_bytes'] + out
        cut = any(f.get('cut') is not None for f in script['frames'])
        done = threading.Event()
        state = {'peer_closed': any(f['op'] == 8 for f in script['frames'])}
        wlock = threading.Lock()

        def reader():
            buf = b''
            try:
                while True:
                    while len(buf) < 2:
                        d = c.recv(65536)
                        if not d:
                            return
                        buf += d
                    b0, b1 = buf[0], buf[1]
                    n = b1 & 0x7f
                    pos = 2
                    if n == 126:
                        while len(buf) < 4:
                            d = c.recv(65536)
                            if not d:
                                return
                            buf += d
                        n = struct.unpack('>H', buf[2:4])[0]
                        pos = 4
                    elif n == 127:
                        while len(buf) < 10:
                            d = c.recv(65536)
                            if not d:
                                return
                            buf += d
                        n = struct.unpack('>Q', buf[2:10])[0]
                        pos = 10
                    masked = b1 & 0x80
                    if masked:
                        pos += 4
                    while len(buf) < pos + n:
                        d = c.recv(65536)
                        if not d:
                            return
                        buf += d
                    data = buf[pos:pos + n]
                    if masked:
                        key = buf[pos - 4:pos]
                        data = bytes(b ^ key[i & 3] for i, b in enumerate(data))
                    buf = buf[pos + n:]
                    op = b0 & 0x0f
                    tag = ''
                    if bool(masked) != (not mask_it):
                        tag = '!'
                    if not b0 & 0x80 or b0 & 0x70:
                        tag += '?'
                    if op == 8:
                        code = struct.unpack('>H', data[:2])[0] if len(data) >= 2 else 1005
                        record.append(f'C{code}{tag}')
                        with wlock:
                            if not state['peer_closed'] and not cut:
                                state['peer_closed'] = True
                                try:
                                    c.sendall(frame_bytes({'op': 8, 'p': ['x', data[:2].hex()]}, mask_it, rng))
                                except OSError:
                                    pass
                        return
                    if op == 10:
                        record.append(f'P{n}{tag}')
                    elif op == 9:
                        record.append(f'I{n}{tag}')
                    elif op == 1:
                        record.append(f'T{n}:{digest(data)}{tag}')
                    elif op == 2:
                        record.append(f'B{n}:{digest(data)}{tag}')
                    else:
                        record.append(f'O{op}{tag}')
            except OSError:
                pass
            finally:
                done.set()

        t = threading.Thread(target=reader, daemon=True)
        t.start()
        try:
            i = 0
            while i < len(out):
                k = rng.choice([1, 7, 100, 1500, 65536, len(out)])
                with wlock:
                    c.sendall(out[i:i + k])
                i += k
                if rng.random() < 0.05:
                    time.sleep(0.002)
            if cut:
                c.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        done.wait(25)
        t.join(1)

    def client_session(self, c):
        """The program connected as a client: path /<script>/<nonce>."""
        rng = random.Random()
        record = []
        nonce = None
        c.settimeout(30)
        try:
            head = b''
            while b'\r\n\r\n' not in head:
                d = c.recv(4096)
                if not d:
                    return
                head += d
            line = head.split(b'\r\n')[0].decode()
            path = line.split(' ')[1]
            _, s, nonce = path.split('/')
            script = dec_script(s)
            key = b''
            for h in head.split(b'\r\n')[1:]:
                if h.lower().startswith(b'sec-websocket-key:'):
                    key = h.split(b':', 1)[1].strip()
            hs = script.get('hs', 'ok')
            acc = accept_key(key)
            if hs == 'badkey':
                acc = accept_key(key + b'x')
            status = 'HTTP/1.1 200 OK' if hs == 'status200' else 'HTTP/1.1 101 Switching Protocols'
            lines = [status]
            if hs != 'noupgrade':
                lines.append('Upgrade: websocket')
            lines.append('Connection: Upgrade')
            lines.append('Sec-WebSocket-Accept: ' + acc.decode())
            if hs == 'ext':
                lines.append('Sec-WebSocket-Extensions: permessage-deflate')
            if hs == 'proto':
                lines.append('Sec-WebSocket-Protocol: chat')
            resp = ('\r\n'.join(lines) + '\r\n\r\n').encode()
            if hs == 'slow':
                for b in resp:
                    c.sendall(bytes([b]))
            elif hs == 'glued':
                script['prefix'] = True
                script['prefix_bytes'] = resp
            else:
                c.sendall(resp)
            if hs in ('badkey', 'status200', 'noupgrade', 'ext', 'proto'):
                try:
                    c.recv(1)
                except OSError:
                    pass
                return
            self.run_frames(c, script, False, record, rng)
        except (OSError, ValueError):
            pass
        finally:
            try:
                c.close()
            except OSError:
                pass
            if nonce:
                self.report(nonce, ' '.join(record) or '-')

    def server_session(self, port, script, nonce):
        """The program serves on port: connect, upgrade, run the frames."""
        rng = random.Random()
        record = []
        c = None
        try:
            c = socket.create_connection(('127.0.0.1', port), timeout=30)
            key = base64.b64encode(os.urandom(16))
            hs = script.get('hs', 'ok')
            hdrs = ['GET /x HTTP/1.1', 'Host: 127.0.0.1']
            if hs != 'noupgrade':
                hdrs.append('Upgrade: websocket')
            hdrs.append('Connection: Upgrade')
            if hs == 'badkey':
                hdrs.append('Sec-WebSocket-Key: c2hvcnQ=')
            elif hs != 'nokey':
                hdrs.append('Sec-WebSocket-Key: ' + key.decode())
            hdrs.append('Sec-WebSocket-Version: ' + ('12' if hs == 'version' else '13'))
            c.sendall(('\r\n'.join(hdrs) + '\r\n\r\n').encode())
            head = b''
            while b'\r\n\r\n' not in head:
                d = c.recv(1)
                if not d:
                    break
                head += d
            ok = head.startswith(b'HTTP/1.1 101') and accept_key(key) in head
            record.append('H101' if ok else 'H-')
            if ok:
                self.run_frames(c, script, True, record, rng)
        except OSError:
            record.append('E')
        finally:
            if c:
                try:
                    c.close()
                except OSError:
                    pass
            self.report(nonce, ' '.join(record))


PEER = None
if os.environ.get('IRONFZ_WS_NOPEER') != '1':
    PEER = Peer()
    os.environ['IRONFZ_WS_PORT'] = str(PEER.ws.getsockname()[1])
    os.environ['IRONFZ_WS_CTL'] = str(PEER.ctl.getsockname()[1])


# ------------------------------------------------------------------ model

def utf8_ok(b):
    try:
        b.decode('utf-8')
        return True
    except UnicodeDecodeError:
        return False


VALID_CLOSE = set(range(3000, 5000)) | {1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011,
                                         1012, 1013, 1014}


class WsModel:
    """Iron's side of one connection (is_client: the program is the
    client, so frames it receives must not be masked)."""

    def __init__(self, is_client, max_message):
        self.is_client = is_client
        self.max = max_message
        self.frames = []
        self.pos = 0
        self.frag_op = 0
        self.frag = b''
        self.failed = False
        self.close_sent = False
        self.close_received = False
        self.alive = True       # the handle (close / abort end it)
        self.sent = []          # what the peer records

    def fail(self, err, code=None):
        if code is not None and not self.close_sent:
            self.close_sent = True
            self.sent.append(f'C{code}')
        self.failed = True
        return (err, 0, 0, b'')

    def receive(self):
        """(error, kind, close_code, data)."""
        if not self.alive or self.failed:
            return (6005, 0, 0, b'')
        while True:
            if self.pos >= len(self.frames):
                raise AssertionError('receive past the frames')
            f = self.frames[self.pos]
            self.pos += 1
            data = payload_of(f['p'])
            cut = f.get('cut')
            if cut is not None:
                # the stream ends in this frame: an incomplete header is
                # 6005; a complete one is checked first (a too large
                # length fails before the payload is read)
                n0 = len(data)
                hlen = 2 + (2 if 126 <= n0 <= 0xffff else 8 if n0 > 0xffff else 0)
                if cut < hlen:
                    return self.fail(6005)
                if n0 > self.max - len(self.frag):
                    return self.fail(6003, 1009)
                return self.fail(6005)
            fin, op, n = f.get('fin', 1), f['op'], len(data)
            masked = f.get('mask', not self.is_client)
            control = op & 8
            if (f.get('rsv', 0) or masked != (not self.is_client) or op not in (0, 1, 2, 8, 9, 10) or
                    (control and (not fin or n > 125)) or (op == 0 and self.frag_op == 0) or
                    (op in (1, 2) and self.frag_op != 0)):
                return self.fail(6002, 1002)
            if f.get('top'):
                return self.fail(6002, 1002)
            if f.get('enc') == 126 and n < 126 or f.get('enc') == 127 and n <= 0xffff:
                return self.fail(6002, 1002)
            if not control and (n > self.max or n > self.max - len(self.frag)):
                return self.fail(6003, 1009)
            if control:
                if op == 8:
                    if n == 1:
                        return self.fail(6002, 1002)
                    code, reason = 1005, data
                    if n >= 2:
                        code = struct.unpack('>H', data[:2])[0]
                        reason = data[2:]
                        if code not in VALID_CLOSE:
                            return self.fail(6002, 1002)
                    if not utf8_ok(reason):
                        return self.fail(6004, 1007)
                    self.close_received = True
                    if not self.close_sent:
                        self.close_sent = True
                        self.sent.append(f'C{code}')
                    return (0, 8, code, reason)
                if op == 9 and not self.close_sent:
                    self.sent.append(f'P{n}')
                return (0, op, 0, data)
            if op in (1, 2) and fin:
                if op == 1 and not utf8_ok(data):
                    return self.fail(6004, 1007)
                return (0, op, 0, data)
            if op in (1, 2):
                self.frag_op = op
            self.frag += data
            if not fin:
                continue
            msg, kind = self.frag, self.frag_op
            self.frag, self.frag_op = b'', 0
            if kind == 1 and not utf8_ok(msg):
                return self.fail(6004, 1007)
            return (0, kind, 0, msg)

    def send(self, kind, data):
        """WebSocket.send_text (kind 1), send_bytes (2), ping (9)."""
        if not self.alive:
            return 6005
        if kind == 9 and len(data) > 125:
            return 6006
        if kind == 1 and not utf8_ok(data):
            return 6004
        if self.failed or self.close_sent:
            return 6005
        if kind != 9 and len(data) > self.max:
            return 6003
        self.sent.append({1: 'T', 2: 'B', 9: 'I'}[kind] + (f'{len(data)}:{digest(data)}' if kind != 9
                                                            else str(len(data))))
        return 0

    def close(self, code=1000):
        if not self.alive:
            return 6005
        self.alive = False
        if self.failed:
            if not self.close_sent:
                self.sent.append(f'C{code}')
            return 6005
        if self.close_received:
            return 0
        if not self.close_sent:
            self.close_sent = True
            self.sent.append(f'C{code}')
        return 0

    def is_open(self):
        return self.alive and not self.failed and not self.close_received


# --------------------------------------------------------------- generator

HELPERS = r'''func ws_report(nonce: String) -> String {
    val (c, e) = Net.tcp_dial("127.0.0.1", OS.env_or("IRONFZ_WS_CTL", "1").to_int(), 5000)
    if e.code != 0 {
        return "noctl {e.code}"
    }
    val (w, we) = TcpSocket.write(c, "REPORT " + nonce + "\n", 5000)
    var line = ""
    var going = true
    while going {
        val (d, re) = TcpSocket.read(c, 4096, 30000)
        if re.code != 0 or d.byte_len() == 0 {
            going = false
        } else {
            line += d
            if line.ends_with("\n") {
                going = false
            }
        }
    }
    TcpSocket.close(c)
    return line.trim()
}

func ws_serve(port: Int, script: String, nonce: String) -> Int {
    val (c, e) = Net.tcp_dial("127.0.0.1", OS.env_or("IRONFZ_WS_CTL", "1").to_int(), 5000)
    if e.code != 0 {
        return e.code
    }
    val (w, we) = TcpSocket.write(c, "SERVE {port} " + script + " " + nonce + "\n", 5000)
    val (d, re) = TcpSocket.read(c, 16, 5000)
    TcpSocket.close(c)
    return re.code
}

func show(m: WebSocketMessage) -> String {
    return "{m.error} {m.kind} {m.close_code} {m.data.byte_len()} {digest(m.data)}"
}'''

TEXTS = ['', 'hello', 'café', '日本語', '\U0001F600', 'a' * 125, 'x' * 126]
BAD_UTF8 = ['ff', 'c0af', 'eda080', 'f4908080', 'e282', 'c3', '80', 'f8888080', 'efbfbd' + 'c328']


class WsGen(StdGen):
    NPROBES = (2, 5)

    def __init__(self, rng):
        super().__init__(rng)
        for m in ('websocket', 'net', 'os', 'time', 'http'):
            self.need_import(m)
        self.need('net', NET_HELPERS)
        self.need('ws', HELPERS)

    def rpayload(self, op, maxm):
        r = self.r
        if op == 1:
            if r.random() < 0.12:
                return ['x', r.choice(BAD_UTF8)]
            if r.random() < 0.2:
                n = r.choice([126, 1000, 65536, 70000])
                return ['t', ('é' * (n // 2 + 1))[:n // 2] + 'z']
            return ['t', r.choice(TEXTS)]
        n = r.choice([0, 1, 125, 126, 1000, 65535, 65536, 70000, 200000, maxm, maxm + 1])
        return ['b', min(n, 250000), r.randint(1, 10**6)]

    def rframes(self, maxm):
        """A frame script, a fragment at a time, with occasional errors."""
        r = self.r
        frames = []
        for _ in range(r.randint(0, 6)):
            k = r.random()
            if k < 0.45:
                op = r.choice([1, 2])
                p = self.rpayload(op, maxm)
                if r.random() < 0.3:
                    # fragmented, controls in between
                    data = payload_of(p)
                    pieces = r.randint(2, 4)
                    cuts = sorted(r.randint(0, len(data)) for _ in range(pieces - 1))
                    bounds = list(zip([0] + cuts, cuts + [len(data)]))
                    for i, (a, b) in enumerate(bounds):
                        frames.append({'fin': int(i == len(bounds) - 1), 'op': op if i == 0 else 0,
                                       'p': ['s', p, a, b]})
                        if i < len(bounds) - 1 and r.random() < 0.3:
                            frames.append({'op': r.choice([9, 10]), 'p': ['t', 'ctl']})
                else:
                    frames.append({'op': op, 'p': p})
            elif k < 0.6:
                frames.append({'op': r.choice([9, 10]), 'p': r.choice([['t', ''], ['t', 'ping'], ['b', 125, 3]])})
            elif k < 0.75:
                bad = r.choice(['rsv', 'opcode', 'ctl_frag', 'ctl_big', 'cont', 'newmsg', 'mask',
                                'enc126', 'enc127', 'top', 'cut', 'big'])
                if bad == 'rsv':
                    frames.append({'op': 1, 'rsv': r.choice([1, 2, 4, 7]), 'p': ['t', 'x']})
                elif bad == 'opcode':
                    frames.append({'op': r.choice([3, 4, 5, 6, 7, 11, 12, 13, 14, 15]), 'p': ['t', '']})
                elif bad == 'ctl_frag':
                    frames.append({'op': 9, 'fin': 0, 'p': ['t', 'x']})
                elif bad == 'ctl_big':
                    frames.append({'op': r.choice([8, 9, 10]), 'p': ['b', 126, 1]})
                elif bad == 'cont':
                    frames.append({'op': 0, 'p': ['t', 'orphan']})
                elif bad == 'newmsg':
                    frames.append({'op': 1, 'fin': 0, 'p': ['t', 'a']})
                    frames.append({'op': 2, 'p': ['t', 'b']})
                elif bad == 'mask':
                    frames.append({'op': 2, 'mask': None, 'p': ['t', 'm']})   # filled in later
                elif bad == 'enc126':
                    frames.append({'op': 2, 'enc': 126, 'p': ['b', r.choice([0, 5, 125]), 2]})
                elif bad == 'enc127':
                    frames.append({'op': 2, 'enc': 127, 'p': ['b', r.choice([0, 126, 65535]), 2]})
                elif bad == 'top':
                    frames.append({'op': 2, 'enc': 127, 'top': 1, 'p': ['b', 70000, 2]})
                elif bad == 'cut':
                    f = {'op': 2, 'p': ['b', r.choice([10, 1000]), 4]}
                    f['cut'] = r.choice([1, 2, 5])
                    frames.append(f)
                    break
                else:
                    frames.append({'op': 2, 'p': ['b', maxm + r.choice([1, 100]), 5]})
            elif k < 0.85:
                code = r.choice([1000, 1001, 1003, 1005, 1006, 999, 1004, 1015, 2999, 3000, 4999, 5000, 1012])
                reason = r.choice(['', 'bye', 'été', None])
                if reason is None:
                    pl = ['x', struct.pack('>H', code).hex() + r.choice(BAD_UTF8)]
                elif r.random() < 0.1:
                    pl = r.choice([['x', ''], ['x', '03']])
                else:
                    pl = ['x', struct.pack('>H', code).hex() + reason.encode().hex()]
                frames.append({'op': 8, 'p': pl})
                break
        return frames

    def build(self, is_client):
        r = self.r
        maxm = r.choice([125, 1000, 65536, 1 << 20])
        frames = self.rframes(maxm)
        for f in frames:
            if 'mask' in f and f['mask'] is None:
                f['mask'] = is_client          # the wrong bit for the role
        while is_client and len(enc_script({'hs': 'glued', 'frames': frames})) > 7000:
            frames.pop()                       # the script travels in the URL
        return maxm, frames

    def drive(self, code, out, sock, m, nrecv):
        """Receives and sends between them; then close or abort and the
        calls on the stale handle."""
        r = self.r
        received = 0
        while received < nrecv:
            if r.random() < 0.2:
                k = r.choice([1, 2, 9, 1, 9])
                if k == 1:
                    s = r.choice(TEXTS + [None])
                    data = bytes.fromhex(r.choice(BAD_UTF8)) if s is None else s.encode()
                    expr = f'WebSocket.send_text({sock}, {lit(data)}, 5000)'
                elif k == 2:
                    n = r.choice([0, 10, 1000, m.max, m.max + 1])
                    seed = r.randint(1, 1000)
                    data = blob(n, seed)
                    expr = f'WebSocket.send_bytes({sock}, blob({n}, {seed}), 5000)'
                else:
                    data = b'p' * r.choice([0, 4, 125, 126])
                    expr = f'WebSocket.ping({sock}, {lit(data)}, 5000)'
                self.raw(code, out, [expr], [m.send(k, data)])
                continue
            v = self.fresh('m')
            code.append(f'val {v} = WebSocket.receive({sock}, 10000)')
            e, kind, cc, data = m.receive()
            self.raw(code, out, [f'show({v})'], [f'{e} {kind} {cc} {len(data)} {digest(data)}'])
            received += 1
        # (abort is abrupt: after a failure the peer may still be sending,
        # and the reset can destroy the close frame it has not read)
        if m.failed or r.random() < 0.7:
            self.raw(code, out, [f'WebSocket.close({sock}, 1000, "bye", 5000)'], [m.close()])
        else:
            code.append(f'WebSocket.abort({sock})')
            m.alive = False
        # the handle is stale now
        self.raw(code, out, [f'WebSocket.is_open({sock})', f'WebSocket.send_text({sock}, "late", 100)',
                             f'WebSocket.receive({sock}, 100).error',
                             f'WebSocket.close({sock}, 1000, "", 100)'],
                 ['false', 6005, 6005, 6005])
        code.append(f'WebSocket.abort({sock})')

    def count_receives(self, is_client, maxm, frames):
        """How many receives the frames answer (the last receive may be
        the error), on a scratch model."""
        m = WsModel(is_client, maxm)
        m.frames = frames
        n = 0
        while m.pos < len(frames) and not m.failed:
            m.receive()
            n += 1
        return n

    def p_client(self):
        r = self.r
        code, out = [], []
        hs = r.choice(['ok'] * 6 + ['badkey', 'status200', 'noupgrade', 'ext', 'proto', 'slow', 'glued'])
        maxm, frames = self.build(True)
        if hs in ('badkey', 'status200', 'noupgrade', 'ext', 'proto'):
            frames = []
        script = enc_script({'hs': hs, 'frames': frames})
        nonce, res = self.fresh('nonce'), self.fresh('ws')
        code += [f'val {nonce} = "{{Time.now_ns()}}"',
                 f'val {res} = WebSocket.connect("ws://127.0.0.1:" + OS.env_or("IRONFZ_WS_PORT", "1") + '
                 f'"/{script}/" + {nonce}, "", {maxm}, 10000)']
        ok = hs in ('ok', 'slow', 'glued')
        self.raw(code, out, [f'{res}.error', f'WebSocket.is_open({res}.socket)'],
                 [0 if ok else 6001, b2s(ok)])
        m = WsModel(True, maxm)
        m.frames = frames
        if ok:
            n = self.count_receives(True, maxm, frames)
            self.drive(code, out, f'{res}.socket', m, n)
        self.line(code, out, [f'ws_report({nonce})'], [' '.join(m.sent) or '-'])
        return Probe(code, out)

    def p_server(self):
        r = self.r
        code, out = [], []
        hs = r.choice(['ok'] * 6 + ['badkey', 'nokey', 'version', 'noupgrade'])
        maxm, frames = self.build(False)
        if hs != 'ok':
            frames = []
        script = enc_script({'hs': hs, 'frames': frames})
        nonce, ls, conn, req, up = (self.fresh('nonce'), self.fresh('ls'), self.fresh('cn'),
                                    self.fresh('rq'), self.fresh('up'))
        code += [f'val {nonce} = "{{Time.now_ns()}}"',
                 f'val {ls} = Http.listen("127.0.0.1", 0)',
                 f'val {ls}s = ws_serve(HttpServer.port({ls}.server), "{script}", {nonce})',
                 f'val {conn} = HttpServer.accept({ls}.server, 10000)',
                 f'val {req} = HttpConnection.read_request({conn}.connection, 16384, 1024, 10000)',
                 f'val {up} = HttpConnection.upgrade_websocket({conn}.connection, {req}, {maxm}, 10000)']
        ok = hs == 'ok'
        self.raw(code, out, [f'{ls}.error', f'{ls}s', f'{conn}.error', f'{req}.error', f'{up}.error'],
                 [0, 0, 0, 0, 0 if ok else 6001])
        m = WsModel(False, maxm)
        m.frames = frames
        if ok:
            n = self.count_receives(False, maxm, frames)
            self.drive(code, out, f'{up}.socket', m, n)
        else:
            code.append(f'HttpConnection.close({conn}.connection)')
        code.append(f'HttpServer.close({ls}.server)')
        rep = ('H101 ' + (' '.join(m.sent))).strip() if ok else 'H-'
        self.line(code, out, [f'ws_report({nonce})'], [rep])
        return Probe(code, out)

    def p_args(self):
        r = self.r
        code, out = [], []
        url = r.choice(['', 'http://x/', 'ws://', 'ws://h:0/', 'ws://h:65536/', 'ws://u@h/', 'ws://h#f',
                        'ws://[::1/', 'ws://h:8a/', 'ws://h h/', 'ws://' + 'h' * 1100 + '/',
                        'ws://h/' + chr(0)])
        mx = r.choice([0, -1, 4294967296, 1000])
        to = r.choice([-1, 1000])
        e = 6006 if (mx <= 0 or mx > 4294967295 or to < 0) else 6000
        self.raw(code, out, [f'WebSocket.connect({lit(url)}, "", {mx}, {to}).error'], [e])
        hdr = r.choice(['Host: x', 'Upgrade: y', 'bad header', 'X-Ok: 1\r\nSec-WebSocket-Key: k',
                        'X: \x01'])
        self.raw(code, out, [f'WebSocket.connect("ws://127.0.0.1:1/", {lit(hdr)}, 100, 100).error'], [6006])
        self.raw(code, out, ['WebSocket.is_open(WebSocket(0))', 'WebSocket.receive(WebSocket(12345), 10).error',
                             'WebSocket.send_text(WebSocket(-7), "x", 10)'], ['false', 6005, 6005])
        return Probe(code, out)

    def probe_kinds(self):
        return [(5, self.p_client), (4, self.p_server), (1, self.p_args)]


def generate(rng):
    g = WsGen(rng)
    g.program()
    return g
