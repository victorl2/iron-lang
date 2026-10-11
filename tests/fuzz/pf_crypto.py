"""crypto and encoding programs (docs/crypto.md): random inputs through
every digest (one shot, the shortcuts and a streaming Hasher fed in random
chunks, finished twice), HMAC (one shot and streaming, keys shorter and
longer than the block), HKDF and PBKDF2 (lengths and costs at and past
their limits), AES-256-GCM and ChaCha20-Poly1305 seal / open and encrypt /
decrypt with a flipped byte, a wrong key, nonce or associated data and a
message shorter than its tag, constant_time_equal, xor, hex and base64
round trips and decoders fed corrupt text, and digests computed by many
threads at once. Binary inputs are built with Hex.decode, so every program
also runs the hex decoder.

Expected values come from Python: hashlib, hmac and base64, and the
`cryptography` package for the AEAD ciphertexts when it is installed
(without it the program checks the round trip and the errors only). The
base64 decoder model follows the rules of src/stdlib/iron_encoding.c:
only the alphabet, '=' only as trailing padding, a lone final character is
a length error, padding when present (always for Base64.decode) must
complete the last quantum.
"""
import base64
import hashlib
import hmac as pyhmac
from pf_seg import Seg, SegProgram, iron_lit

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
    HAVE_AEAD = True
except Exception:   # noqa: BLE001 (any import problem: no cross-check)
    HAVE_AEAD = False

ALGS = {   # HashAlgorithm variant: (hashlib name, digest size, block size)
    'Sha1': ('sha1', 20), 'Sha224': ('sha224', 28), 'Sha256': ('sha256', 32),
    'Sha384': ('sha384', 48), 'Sha512': ('sha512', 64), 'Sha3_256': ('sha3_256', 32),
    'Sha3_512': ('sha3_512', 64), 'Md5': ('md5', 16),
}
SHORT = {'Sha1': 'sha1', 'Sha224': 'sha224', 'Sha256': 'sha256', 'Sha384': 'sha384',
         'Sha512': 'sha512', 'Sha3_256': 'sha3_256', 'Sha3_512': 'sha3_512', 'Md5': 'md5'}

HELPERS = '''func bytes_of(hex: String) -> String {
    return Hex.decode(hex).data
}
'''


def hkdf(name, ikm, salt, info, length):
    size = hashlib.new(name).digest_size
    if not salt:
        salt = b'\x00' * size
    prk = pyhmac.new(salt, ikm, name).digest()
    out, t, i = b'', b'', 1
    while len(out) < length:
        t = pyhmac.new(prk, t + info + bytes([i]), name).digest()
        out += t
        i += 1
    return out[:length]


def b64_decode_model(text, url, require_pad):
    """(error, data, message) as iron_encoding.c decodes text."""
    alpha = ('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789' +
             ('-_' if url else '+/'))
    b = text
    what = 'base64url' if url else 'base64'
    data_len, pad = len(b), 0
    while data_len > 0 and b[data_len - 1] == '=' and pad < 2:
        data_len -= 1
        pad += 1
    for i in range(data_len):
        if b[i] not in alpha:
            if b[i] == '=':
                return 9102, b'', f'invalid {what} padding at byte {i}'
            return 9100, b'', f'invalid {what} character at byte {i}'
    rem = data_len % 4
    if rem == 1:
        return 9101, b'', f'invalid {what} length'
    if pad > 0 or require_pad:
        want = 0 if rem == 0 else 4 - rem
        if pad != want:
            return 9102, b'', f'invalid {what} padding at byte {data_len}'
    vals = [alpha.index(c) for c in b[:data_len]]
    out = bytearray()
    i = 0
    while i + 4 <= data_len:
        v = (vals[i] << 18) | (vals[i + 1] << 12) | (vals[i + 2] << 6) | vals[i + 3]
        out += bytes([(v >> 16) & 255, (v >> 8) & 255, v & 255])
        i += 4
    if rem == 2:
        v = (vals[i] << 18) | (vals[i + 1] << 12)
        out.append((v >> 16) & 255)
    elif rem == 3:
        v = (vals[i] << 18) | (vals[i + 1] << 12) | (vals[i + 2] << 6)
        out += bytes([(v >> 16) & 255, (v >> 8) & 255])
    return 0, bytes(out), ''


def hex_decode_model(text):
    for i, c in enumerate(text):
        if c not in '0123456789abcdefABCDEF':
            return 9100, b'', f'invalid hex character at byte {i}'
    if len(text) % 2:
        return 9101, b'', 'invalid hex length'
    return 0, bytes.fromhex(text), ''


class CryptoGen(SegProgram):
    HELPERS = HELPERS
    IMPORTS = ('crypto', 'encoding')
    NSEGS = (2, 5)

    def add_segment(self):
        r = self.r
        pick = r.choices(['hash', 'hmac', 'hasher', 'hkdf', 'pbkdf2', 'aead', 'encrypt', 'encoding',
                          'misc', 'threads'], [3, 2, 3, 2, 1.5, 3, 2, 3, 1, 1])[0]
        self.segs.append(getattr(self, 'seg_' + pick)())

    def rand_bytes(self, lo=0, hi=200):
        r = self.r
        c = r.random()
        n = r.randint(lo, hi)
        if c < 0.3:
            return bytes(r.choice(b'abcdefgh xyz') for _ in range(n))
        if c < 0.4:
            return bytes([r.choice([0, 255])]) * n
        return bytes(r.getrandbits(8) for _ in range(n))

    def lit(self, b):
        """An Iron expression for bytes b."""
        try:
            t = b.decode('ascii')
            if len(t) <= 1000 and all(32 <= ord(c) < 127 for c in t) and self.r.random() < 0.7:
                return iron_lit(t)
        except UnicodeDecodeError:
            pass
        h = b.hex()
        if self.r.random() < 0.3:
            h = h.upper()
        if len(h) > 3000:
            # literals are at most 4095 bytes: join pieces
            parts = [h[i:i + 2000] for i in range(0, len(h), 2000)]
            return ' + '.join(f'bytes_of("{p}")' for p in parts)
        return f'bytes_of("{h}")'

    def bind(self, lines, b, p='d'):
        n = self.fresh(p)
        lines.append(f'val {n} = {self.lit(b)}')
        return n

    # ------------------------------------------------------------ hash
    def seg_hash(self):
        r = self.r
        lines, out = [], []
        data = self.rand_bytes(0, r.choice([10, 100, 1000, 5000]))
        d = self.bind(lines, data)
        for alg in r.sample(list(ALGS), r.randint(1, 4)):
            name, size = ALGS[alg]
            dig = hashlib.new(name, data).hexdigest()
            if r.random() < 0.5:
                lines.append(f'println(Hex.encode(Crypto.hash(HashAlgorithm.{alg}, {d})))')
            else:
                lines.append(f'println(Hex.encode(Crypto.{SHORT[alg]}({d})))')
            out.append(dig)
            lines.append(f'println("{{Crypto.hash_size(HashAlgorithm.{alg})}} {{{d}.byte_len()}}")')
            out.append(f'{size} {len(data)}')
        return Seg(lines, out)

    # ------------------------------------------------------------ hmac
    def seg_hmac(self):
        r = self.r
        lines, out = [], []
        key = self.rand_bytes(0, r.choice([16, 64, 65, 200]))
        data = self.rand_bytes(0, 300)
        k = self.bind(lines, key, 'k')
        d = self.bind(lines, data)
        for alg in r.sample(list(ALGS), r.randint(1, 3)):
            name, size = ALGS[alg]
            mac = pyhmac.new(key, data, name).digest()
            m = self.fresh('m')
            lines += [f'val {m} = Crypto.hmac(HashAlgorithm.{alg}, {k}, {d})', f'println(Hex.encode({m}))']
            out.append(mac.hex())
            other = bytearray(mac)
            if other:
                other[r.randrange(len(other))] ^= 1 << r.randrange(8)
            lines.append(f'println("{{Crypto.constant_time_equal({m}, Crypto.hmac(HashAlgorithm.{alg}, {k}, {d}))}} '
                         f'{{Crypto.constant_time_equal({m}, {self.lit(bytes(other))})}} '
                         f'{{Crypto.constant_time_equal({m}, {self.lit(mac[:-1])})}}")')
            out.append('true false false')
        return Seg(lines, out)

    # ---------------------------------------------------------- hasher
    def chunks(self, n):
        r = self.r
        cuts = sorted(r.sample(range(1, n), min(n - 1, r.randint(0, 6)))) if n > 1 else []
        bounds = [0] + cuts + [n]
        return [(bounds[i], bounds[i + 1]) for i in range(len(bounds) - 1)]

    def seg_hasher(self):
        r = self.r
        lines, out = [], []
        alg = r.choice(list(ALGS))
        name, size = ALGS[alg]
        mac = r.random() < 0.4
        key = self.rand_bytes(0, 100) if mac else b''
        h = self.fresh('h')
        if mac:
            k = self.bind(lines, key, 'k')
            lines.append(f'var {h} = Hasher.hmac(HashAlgorithm.{alg}, {k})')
        else:
            lines.append(f'var {h} = Hasher.new(HashAlgorithm.{alg})')
        for msg in range(r.randint(1, 3)):
            data = self.rand_bytes(0, r.choice([0, 50, 500, 3000]))
            d = self.bind(lines, data)
            for a, b in self.chunks(len(data)):
                lines.append(f'{h}.update({d}.byte_slice({a}, {b}))')
            if r.random() < 0.2:
                lines.append(f'{h}.update("")')
            want = (pyhmac.new(key, data, name).digest() if mac else hashlib.new(name, data).digest())
            f = self.fresh('f')
            one = f'Crypto.hmac(HashAlgorithm.{alg}, {k}, {d})' if mac else f'Crypto.hash(HashAlgorithm.{alg}, {d})'
            lines += [f'val {f} = {h}.finish()', f'println("{{Hex.encode({f})}} {{{f} == {one}}}")']
            out.append(f'{want.hex()} true')
        return Seg(lines, out)

    # ------------------------------------------------------------ hkdf
    def seg_hkdf(self):
        r = self.r
        lines, out = [], []
        alg = r.choice(list(ALGS))
        name, size = ALGS[alg]
        ikm = self.rand_bytes(0 if r.random() < 0.1 else 1, 80)
        salt = self.rand_bytes(0, 40) if r.random() < 0.7 else b''
        info = self.rand_bytes(0, 40)
        c = r.random()
        if c < 0.1:
            length = 0
        elif c < 0.2:
            length = 255 * size + r.choice([0, 1])
        else:
            length = r.randint(1, 3 * size)
        res = self.fresh('res')
        lines.append(f'val {res} = Crypto.hkdf(HashAlgorithm.{alg}, {self.lit(ikm)}, {self.lit(salt)}, '
                     f'{self.lit(info)}, {length})')
        lines.append(f'println("{{{res}.error}} {{Hex.encode({res}.data)}} {{{res}.error_message}}")')
        if length < 1 or length > 255 * size:
            out.append('9005  HKDF length must be 1 to 255 * the hash size')
        elif not ikm:
            out.append('9002  HKDF input key material is empty')
        else:
            out.append(f'0 {hkdf(name, ikm, salt, info, length).hex()} ')
        return Seg(lines, out)

    # ---------------------------------------------------------- pbkdf2
    def seg_pbkdf2(self):
        r = self.r
        lines, out = [], []
        alg = r.choice(list(ALGS))
        name, size = ALGS[alg]
        pw = self.rand_bytes(0, 60)
        salt = self.rand_bytes(0, 30)
        it = r.choice([0, 1, 2, 10, 100, 1000, 2500])
        length = r.choice([0, 1, size, size + 1, 3 * size, 100])
        res = self.fresh('res')
        lines.append(f'val {res} = Crypto.pbkdf2(HashAlgorithm.{alg}, {self.lit(pw)}, {self.lit(salt)}, {it}, {length})')
        lines.append(f'println("{{{res}.error}} {{Hex.encode({res}.data)}}")')
        if it < 1:
            out.append('9006 ')
        elif length < 1:
            out.append('9005 ')
        else:
            out.append(f'0 {hashlib.pbkdf2_hmac(name, pw, salt, it, length).hex()}')
        return Seg(lines, out)

    # ------------------------------------------------------------ aead
    def seg_aead(self):
        r = self.r
        lines, out = [], []
        alg = r.choice(['Aes256Gcm', 'ChaCha20Poly1305'])
        key = self.rand_bytes(32, 32) if r.random() < 0.9 else self.rand_bytes(0, 40)
        nonce = self.rand_bytes(12, 12) if r.random() < 0.9 else self.rand_bytes(0, 20)
        pt = self.rand_bytes(0, r.choice([0, 20, 300, 2000]))
        aad = self.rand_bytes(0, 50) if r.random() < 0.6 else b''
        k, n, p, a = (self.bind(lines, key, 'k'), self.bind(lines, nonce, 'n'),
                      self.bind(lines, pt, 'p'), self.bind(lines, aad, 'a'))
        A = f'AeadAlgorithm.{alg}'
        s = self.fresh('s')
        lines.append(f'val {s} = Crypto.seal({A}, {k}, {n}, {p}, {a})')
        if len(key) != 32:
            lines.append(f'println("{{{s}.error}} {{{s}.data.byte_len()}}")')
            out.append('9002 0')
            return Seg(lines, out)
        if len(nonce) != 12:
            lines.append(f'println("{{{s}.error}} {{{s}.data.byte_len()}}")')
            out.append('9003 0')
            return Seg(lines, out)
        if HAVE_AEAD:
            ct = (AESGCM(key) if alg == 'Aes256Gcm' else ChaCha20Poly1305(key)).encrypt(nonce, pt, aad or None)
            lines.append(f'println("{{{s}.error}} {{Hex.encode({s}.data)}}")')
            out.append(f'0 {ct.hex()}')
        else:
            lines.append(f'println("{{{s}.error}} {{{s}.data.byte_len()}}")')
            out.append(f'0 {len(pt) + 16}')
        o = self.fresh('o')
        lines += [f'val {o} = Crypto.open({A}, {k}, {n}, {s}.data, {a})',
                  f'println("{{{o}.error}} {{{o}.data == {p}}}")']
        out.append('0 true')
        # tampering
        total = len(pt) + 16
        for _ in range(r.randint(1, 3)):
            kind = r.choice(['flip', 'aad', 'nonce', 'key', 'short'])
            t = self.fresh('t')
            if kind == 'flip':
                mask = bytearray(total)
                mask[r.randrange(total)] = 1 << r.randrange(8)
                lines.append(f'val {t} = Crypto.open({A}, {k}, {n}, Crypto.xor({s}.data, {self.lit(bytes(mask))}), {a})')
            elif kind == 'aad':
                lines.append(f'val {t} = Crypto.open({A}, {k}, {n}, {s}.data, {a} + "x")')
            elif kind == 'nonce':
                nn = bytearray(nonce)
                nn[r.randrange(12)] ^= 0x80
                lines.append(f'val {t} = Crypto.open({A}, {k}, {self.lit(bytes(nn))}, {s}.data, {a})')
            elif kind == 'key':
                kk = bytearray(key)
                kk[r.randrange(32)] ^= 0x01
                lines.append(f'val {t} = Crypto.open({A}, {self.lit(bytes(kk))}, {n}, {s}.data, {a})')
            else:
                cut = r.randint(0, 15)
                lines.append(f'val {t} = Crypto.open({A}, {k}, {n}, {s}.data.byte_slice(0, {cut}), {a})')
            lines.append(f'println("{{{t}.error}} {{{t}.data.byte_len()}}")')
            out.append('9004 0')
        return Seg(lines, out)

    def seg_encrypt(self):
        r = self.r
        lines, out = [], []
        alg = r.choice(['Aes256Gcm', 'ChaCha20Poly1305'])
        A = f'AeadAlgorithm.{alg}'
        key = self.rand_bytes(32, 32)
        pt = self.rand_bytes(0, 500)
        aad = self.rand_bytes(0, 30)
        k, p, a = self.bind(lines, key, 'k'), self.bind(lines, pt, 'p'), self.bind(lines, aad, 'a')
        e1, e2 = self.fresh('e'), self.fresh('e')
        lines += [f'val {e1} = Crypto.encrypt({A}, {k}, {p}, {a})', f'val {e2} = Crypto.encrypt({A}, {k}, {p}, {a})',
                  f'println("{{{e1}.error}} {{{e1}.data.byte_len()}} {{{e1}.data != {e2}.data}}")']
        out.append(f'0 {12 + len(pt) + 16} true')
        dd = self.fresh('dd')
        lines += [f'val {dd} = Crypto.decrypt({A}, {k}, {e1}.data, {a})', f'println("{{{dd}.error}} {{{dd}.data == {p}}}")']
        out.append('0 true')
        if HAVE_AEAD:
            # the nonce travels first: open with it gives the same plaintext
            o = self.fresh('o')
            lines += [f'val {o} = Crypto.open({A}, {k}, {e1}.data.byte_slice(0, 12), {e1}.data.byte_slice(12, {e1}.data.byte_len()), {a})',
                      f'println("{{{o}.error}} {{{o}.data == {p}}}")']
            out.append('0 true')
        t = self.fresh('t')
        cut = r.randint(0, 27)
        lines += [f'val {t} = Crypto.decrypt({A}, {k}, {e1}.data.byte_slice(0, {cut}), {a})',
                  f'println("{{{t}.error}}")']
        out.append('9004')
        mask = bytearray(12 + len(pt) + 16)
        mask[r.randrange(len(mask))] = 0x40
        t2 = self.fresh('t')
        lines += [f'val {t2} = Crypto.decrypt({A}, {k}, Crypto.xor({e1}.data, {self.lit(bytes(mask))}), {a})',
                  f'println("{{{t2}.error}} {{{t2}.data.byte_len()}}")']
        out.append('9004 0')
        return Seg(lines, out)

    # -------------------------------------------------------- encoding
    def corrupt(self, text, alpha):
        r = self.r
        c = r.random()
        s = list(text)
        if c < 0.25 and s:
            s[r.randrange(len(s))] = r.choice([' ', '\n', '*', '=', '-', '_', '+', '/', 'g', 'G', '.'])
        elif c < 0.45:
            s.insert(r.randint(0, len(s)), r.choice(['=', ' ', '==', 'A', '!']))
        elif c < 0.65 and s:
            del s[r.randrange(len(s))]
        elif c < 0.8:
            s += ['='] * r.randint(1, 3)
        else:
            while s and s[-1] == '=':
                s.pop()
        return ''.join(s)

    def seg_encoding(self):
        r = self.r
        lines, out = [], []
        data = self.rand_bytes(0, r.choice([0, 3, 10, 100, 700]))
        d = self.bind(lines, data)
        hx = data.hex()
        b64 = base64.b64encode(data).decode()
        url = base64.urlsafe_b64encode(data).decode().rstrip('=')
        lines += [f'println(Hex.encode({d}))', f'println(Base64.encode({d}))', f'println(Base64.url_encode({d}))']
        out += [hx, b64, url]
        for enc, txt in (('Hex', hx), ('Base64', b64), ('Base64url', url), ('Base64url', b64.replace('+', '-').replace('/', '_'))):
            fn = {'Hex': 'Hex.decode', 'Base64': 'Base64.decode', 'Base64url': 'Base64.url_decode'}[enc]
            if r.random() < 0.6:
                t = txt
                if enc == 'Hex' and r.random() < 0.5:
                    t = t.upper()
            else:
                t = self.corrupt(txt, None)
            if enc == 'Hex':
                err, got, msg = hex_decode_model(t)
            else:
                err, got, msg = b64_decode_model(t, enc == 'Base64url', enc == 'Base64')
            res = self.fresh('res')
            lines += [f'val {res} = {fn}({iron_lit(t)})',
                      f'println("{{{res}.error}} {{Hex.encode({res}.data)}} {{{res}.error_message}}")']
            out.append(f'{err} {got.hex()} {msg}')
        return Seg(lines, out)

    # ------------------------------------------------------------ misc
    def seg_misc(self):
        r = self.r
        lines, out = [], []
        a = self.rand_bytes(0, 40)
        k = self.rand_bytes(0, 10)
        x = bytes(b ^ k[i % len(k)] for i, b in enumerate(a)) if k else a
        lines.append(f'println(Hex.encode(Crypto.xor({self.lit(a)}, {self.lit(k)})))')
        out.append(x.hex())
        for _ in range(r.randint(1, 3)):
            n = r.randint(0, 64)
            lines.append(f'println("{{Crypto.random_bytes({n}).byte_len()}} {{Crypto.random_token({n}).byte_len()}}")')
            out.append(f'{n} {len(base64.urlsafe_b64encode(bytes(n)).decode().rstrip("="))}')
        lo = r.randint(-100, 100)
        hi = lo + r.randint(1, 50)
        v = self.fresh('v')
        lines += [f'val {v} = Crypto.random_int({lo}, {hi})', f'println("{{{v} >= {lo} and {v} < {hi}}}")']
        out.append('true')
        for alg in ('Aes256Gcm', 'ChaCha20Poly1305'):
            lines.append(f'println("{{Crypto.aead_key_size(AeadAlgorithm.{alg})}} {{Crypto.aead_nonce_size(AeadAlgorithm.{alg})}} {{Crypto.aead_tag_size(AeadAlgorithm.{alg})}}")')
            out.append('32 12 16')
        return Seg(lines, out)

    # --------------------------------------------------------- threads
    def seg_threads(self):
        """Digests and MACs from a parallel for and spawns (OpenSSL state
        used from many threads at once)."""
        r = self.r
        lines, out = [], []
        alg = r.choice(list(ALGS))
        name, size = ALGS[alg]
        n = r.choice([4, 16, 50])
        m = self.fresh('m')
        i = self.fresh('i')
        g = self.fresh('g')
        base = self.rand_bytes(0, 50)
        b = self.bind(lines, base, 'b')
        lines += [f'val {m} = Mutex(0)', f'for {i} in range({n}) parallel {{',
                  f'    var {self.fresh("h")} = Hasher.new(HashAlgorithm.{alg})',
                  f'    val {self.fresh("x")} = Crypto.hash(HashAlgorithm.{alg}, {b} + "{{{i}}}")',
                  f'    val {self.fresh("y")} = Crypto.hmac(HashAlgorithm.{alg}, "k{{{i}}}", {b})',
                  f'    val {g} = {m}.lock()',
                  f'    {g}.set({g}.get() + Crypto.hash(HashAlgorithm.{alg}, {b} + "{{{i}}}").byte_at(0))', '}']
        tot = sum(hashlib.new(name, base + str(j).encode()).digest()[0] for j in range(n))
        hs = []
        for t in range(r.randint(1, 3)):
            h = self.fresh('h')
            data = self.rand_bytes(0, 300)
            lines += [f'val {h} = spawn("hash{t}") {{', f'    var {self.fresh("hh")} = Hasher.hmac(HashAlgorithm.{alg}, "key{t}")',
                      f'    return Hex.encode(Crypto.hmac(HashAlgorithm.{alg}, "key{t}", {self.lit(data)}))', '}']
            hs.append((h, pyhmac.new(f'key{t}'.encode(), data, name).hexdigest()))
        for h, want in hs:
            lines.append(f'println(await {h})')
            out.append(want)
        lines += ['{', f'    val {g} = {m}.lock()', f'    println("sum {{{g}.get()}}")', '}']
        out.append(f'sum {tot}')
        return Seg(lines, out)


def generate(rng):
    g = CryptoGen(rng)
    g.program()
    return g
