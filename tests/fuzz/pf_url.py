"""The url module (manual 9.11, src/stdlib/url.iron): Url.parse on random
and malformed URLs, Url.build and the property parse(build(u)) == u for
every parsed u, the builder, Url.resolve against RFC 3986 section 5 (the
normal and abnormal examples of 5.4 and random references), and the
percent codec on arbitrary bytes (round trips, malformed escapes).

The model is RFC 3986 with the rules url.iron documents:
  - parse: the Appendix B split; a scheme is the text before the first
    ':' when it is ALPHA *(ALPHA / DIGIT / "+" / "-" / ".") and no '/',
    '?' or '#' comes first (anything else is a relative reference, like
    urllib.parse); userinfo runs to the last '@' of the authority; a host
    in brackets ends at the first ']', which must be followed by nothing
    or ':' port (2004 otherwise, and for a missing ']'); a host without
    brackets must not hold ':', '[' or ']' (2003); the port is decimal
    digits up to 65535 (2005), an empty port is the default (-1); the
    fields keep their text as written (no case folding, no decoding);
    2000 for "" and 2009 past 8192 bytes.
  - build: recomposition (5.3) that never changes the parse: the
    authority is written when there is a host, userinfo or port, brackets
    go around a host holding ':', '[' or ']' (unless it is already in
    brackets), a path not starting with '/' gets one after an authority,
    a path starting with "//" without an authority gets an empty one.
  - resolve: 5.2.2 with remove_dot_segments (5.2.4) and merge (5.2.3); an
    empty query or authority counts as undefined (the Url fields cannot
    tell them apart).
  - percent_encode keeps the unreserved bytes (and the sub-delims when
    reserved_ok) and writes %XX in upper case; percent_decode turns %XX
    into the byte and fails with 2007 on a '%' without two hex digits.
"""
import re

from stdfz_common import StdGen, Probe, lit, qtext

ALPHA = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
DIGIT = '0123456789'
UNRES = ALPHA + DIGIT + '-_.~'
SUBD = "!$&'()*+,;="


def valid_scheme(s):
    return bool(s) and s[0] in ALPHA and all(c in ALPHA + DIGIT + '+-.' for c in s)


class UrlErr(Exception):
    def __init__(self, code):
        self.code = code


def parse(s):
    """(scheme, userinfo, host, port, path, query, fragment) or UrlErr."""
    if len(s) == 0:
        raise UrlErr(2000)
    if len(s.encode('utf-8', 'surrogateescape')) > 8192:
        raise UrlErr(2009)
    rest = s
    scheme = userinfo = host = path = query = fragment = ''
    port = -1
    c = rest.find(':')
    if c > 0 and not any(ch in rest[:c] for ch in '/?#') and valid_scheme(rest[:c]):
        scheme = rest[:c]
        rest = rest[c + 1:]
    if rest.startswith('//'):
        rest = rest[2:]
        m = re.search(r'[/?#]', rest)
        e = m.start() if m else len(rest)
        auth, rest = rest[:e], rest[e:]
        hp = auth
        at = auth.rfind('@')
        if at >= 0:
            userinfo, hp = auth[:at], auth[at + 1:]
        pstr = None
        if hp.startswith('['):
            rb = hp.find(']')
            if rb < 0:
                raise UrlErr(2004)
            host = hp[1:rb]
            after = hp[rb + 1:]
            if after:
                if after[0] != ':':
                    raise UrlErr(2004)
                pstr = after[1:]
        else:
            lc = hp.rfind(':')
            if lc >= 0:
                host, pstr = hp[:lc], hp[lc + 1:]
            else:
                host = hp
            if any(ch in host for ch in ':[]'):
                raise UrlErr(2003)
        if pstr:
            if not all(ch in DIGIT for ch in pstr) or int(pstr) > 65535:
                raise UrlErr(2005)
            port = int(pstr)
    m = re.search(r'[?#]', rest)
    e = m.start() if m else len(rest)
    path, after = rest[:e], rest[e:]
    if after.startswith('?'):
        h = after.find('#')
        if h >= 0:
            query, fragment = after[1:h], after[h + 1:]
        else:
            query = after[1:]
    elif after.startswith('#'):
        fragment = after[1:]
    return (scheme, userinfo, host, port, path, query, fragment)


def build(u):
    scheme, userinfo, host, port, path, query, fragment = u
    out = scheme + ':' if scheme else ''
    if host or userinfo or port >= 0:
        out += '//'
        if userinfo:
            out += userinfo + '@'
        if host.startswith('[') and host.endswith(']') and len(host) >= 2:
            out += host
        elif any(ch in host for ch in ':[]'):
            out += '[' + host + ']'
        else:
            out += host
        if port >= 0:
            out += ':' + str(port)
        if path and not path.startswith('/'):
            out += '/'
    elif path.startswith('//'):
        out += '//'
    out += path
    if query:
        out += '?' + query
    if fragment:
        out += '#' + fragment
    return out


def remove_dot_segments(inp):
    out = []
    while inp:
        if inp.startswith('../'):
            inp = inp[3:]
        elif inp.startswith('./'):
            inp = inp[2:]
        elif inp.startswith('/./'):
            inp = '/' + inp[3:]
        elif inp == '/.':
            inp = '/'
        elif inp.startswith('/../'):
            inp = '/' + inp[4:]
            if out:
                out.pop()
        elif inp == '/..':
            inp = '/'
            if out:
                out.pop()
        elif inp in ('.', '..'):
            inp = ''
        else:
            j = inp.find('/', 1)
            if j < 0:
                j = len(inp)
            out.append(inp[:j])
            inp = inp[j:]
    return ''.join(out)


def merge(base, rpath):
    if base[2] and base[4] == '':
        return '/' + rpath
    ls = base[4].rfind('/')
    if ls < 0:
        return rpath
    return base[4][:ls + 1] + rpath


def resolve(base, ref):
    r = parse(ref)
    rs, ru, rh, rp, rpath, rq, rf = r
    if rs:
        t = (rs, ru, rh, rp, remove_dot_segments(rpath), rq)
    elif rh:
        t = (base[0], ru, rh, rp, remove_dot_segments(rpath), rq)
    else:
        if rpath == '':
            tp, tq = base[4], rq if rq else base[5]
        elif rpath.startswith('/'):
            tp, tq = remove_dot_segments(rpath), rq
        else:
            tp, tq = remove_dot_segments(merge(base, rpath)), rq
        t = (base[0], base[1], base[2], base[3], tp, tq)
    return t + (rf,)


def penc(b, reserved_ok):
    out = []
    for x in b:
        c = chr(x)
        if c in UNRES and x < 128 or reserved_ok and c in SUBD and x < 128:
            out.append(c)
        else:
            out.append('%%%02X' % x)
    return ''.join(out)


def pdec(b):
    """bytes -> bytes, or None for 2007."""
    out = bytearray()
    i = 0
    hexd = b'0123456789abcdefABCDEF'
    while i < len(b):
        if b[i] == 37:
            if i + 2 >= len(b) or b[i + 1] not in hexd or b[i + 2] not in hexd:
                return None
            out.append(int(b[i + 1:i + 3], 16))
            i += 3
        else:
            out.append(b[i])
            i += 1
    return bytes(out)


SCHEMES = ['http', 'https', 'HTTP', 'ws', 'wss', 'file', 'mailto', 'a+b.c-d', 'x1', '1x', '',
           'a b', '-a', 'ftp', 'urn', 'hü']
USERS = ['', 'u', 'u:p', 'user:pa:ss', 'a@b', '%40x', ':', 'é', 'u;x=1']
HOSTS = ['example.com', '', '127.0.0.1', 'a', 'EXAMPLE.org', '[::1]', '[fe80::1%25eth0]',
         '[v1.x]', '[::1', '[::1]x', '[]', 'hü.de', 'a:b', 'x]', '[[a]]', 'a[b',
         '日本.jp', 'xn--n3h.com', '[2001:db8::7]']
PORTS = [None, '', '0', '80', '8080', '65535', '65536', '99999', '99999999999999999999999', '-1',
         '8a', ' 80', '080', '00000000000000000000080']
PATHS = ['', '/', '/a/b', 'a/b', '/./x', '/../a', '/é/../x', '//x', 'a:b', '/%41',
         '/a b', '/a/./b/../c/', '/..', '/.', '/a/b/..', '/日/./本/../x', '/;p', '/a?b',
         '/a#b', '/x/y/z/../../..', '/.../a', '/a/..b/./c']
QUERIES = [None, '', 'q', 'a=1&b=2', 'x?y', 'a=%20', '#', 'é=1', '/?@:']
FRAGS = [None, '', 'f', 'x#y', '?q', 'é', '%']
JUNK = list('/?#@:[]%') + ['//', '..', '.', ' ', '\t', 'é', '\x00', '::', '[', ']']

RFC_BASE = 'http://a/b/c/d;p?q'
RFC_REFS = ['g:h', 'g', './g', 'g/', '/g', '//g', '?y', 'g?y', '#s', 'g#s', 'g?y#s', ';x', 'g;x',
            'g;x?y#s', '', '.', './', '..', '../', '../g', '../..', '../../', '../../g',
            '../../../g', '../../../../g', '/./g', '/../g', 'g.', '.g', 'g..', '..g', './../g',
            './g/.', 'g/./h', 'g/../h', 'g;x=1/./y', 'g;x=1/../y', 'g?y/./x', 'g?y/../x',
            'g#s/./x', 'g#s/../x', 'http:g']
BASES = [RFC_BASE, 'http://a', 'http://u@h:81/x/y?z#w', 'https://[::1]:8443/a/b/', 'file:///etc/x',
         'mailto:a@b', 'a/b/c', '/abs/path', 'http://h/é/日/f', 'ws://h/a/b/c/d/e']


class UrlGen(StdGen):
    NPROBES = (6, 14)

    def __init__(self, rng):
        super().__init__(rng)
        self.need_import('url')
        self.need('fields', '''func fields(u: Url) -> String {
    return "<" + q(u.scheme) + "><" + q(u.userinfo) + "><" + q(u.host) + "><{u.port}><" + q(u.path) + "><" + q(u.query) + "><" + q(u.fragment) + ">"
}''')

    def rurl(self):
        r = self.r
        if r.random() < 0.15:
            return r.choice(RFC_REFS + BASES)
        s = ''
        sch = r.choice(SCHEMES)
        if r.random() < 0.75:
            s += sch + ':'
        if r.random() < 0.7:
            s += '//'
            u = r.choice(USERS)
            if u or r.random() < 0.1:
                s += u + '@'
            s += r.choice(HOSTS)
            p = r.choice(PORTS)
            if p is not None:
                s += ':' + p
        s += r.choice(PATHS)
        qq = r.choice(QUERIES)
        if qq is not None:
            s += '?' + qq
        f = r.choice(FRAGS)
        if f is not None:
            s += '#' + f
        for _ in range(r.choice([0, 0, 0, 1, 2, 3])):
            i = r.randint(0, len(s))
            s = s[:i] + r.choice(JUNK) + s[i:]
        if r.random() < 0.03:
            s = 'http://h/' + 'a' * r.choice([8180, 8183, 8184, 8185, 9000])
        return s

    @staticmethod
    def ftext(u):
        return '<%s><%s><%s><%d><%s><%s><%s>' % (qtext(u[0]), qtext(u[1]), qtext(u[2]), u[3],
                                                   qtext(u[4]), qtext(u[5]), qtext(u[6]))

    def p_parse(self):
        r = self.r
        code, out = [], []
        for _ in range(r.randint(1, 4)):
            s = self.rurl()
            n = self.fresh('u')
            e = self.fresh('e')
            code.append(f'val ({n}, {e}) = Url.parse({lit(s)})')
            tag = self.fresh('t')
            try:
                u = parse(s)
                code.append(f'println("{tag} {{{e}.code}} " + fields({n}))')
                out.append(f'{tag} 0 ' + self.ftext(u))
                b = build(u)
                tag2 = self.fresh('t')
                n2, e2 = self.fresh('u'), self.fresh('e')
                code.append(f'val ({n2}, {e2}) = Url.parse(Url.build({n}))')
                code.append(f'println("{tag2} " + q(Url.build({n})) + " {{{e2}.code}} " + fields({n2}))')
                u2 = parse(b) if b else None
                if b == '':
                    out.append(f'{tag2}  2000 <><><><-1><><><>')
                else:
                    out.append(f'{tag2} {qtext(b)} 0 ' + self.ftext(u2))
                    assert u2 == u, (s, u, b, u2)
            except UrlErr as x:
                code.append(f'println("{tag} {{{e}.code}} " + fields({n}))')
                out.append(f'{tag} {x.code} <><><><-1><><><>')
        return Probe(code, out)

    def p_builder(self):
        r = self.r
        u = [r.choice(SCHEMES[:9]), r.choice(USERS), r.choice(HOSTS), r.choice([-1, -1, 0, 80, 65535]),
             r.choice(PATHS), r.choice(QUERIES) or '', r.choice(FRAGS) or '']
        u[2] = u[2].strip('[]') if r.random() < 0.5 else u[2]
        for i in (0, 1, 2, 4):
            if r.random() < 0.3:
                u[i] = ''
        u[4] = u[4].replace('?', '').replace('#', '')
        expr = 'Url.builder()'
        names = ['set_scheme', 'set_userinfo', 'set_host', 'set_port', 'set_path', 'set_query',
                 'set_fragment']
        order = list(range(7))
        r.shuffle(order)
        for i in order:
            if i == 3:
                expr += f'.set_port({u[3]})'
            elif u[i] or r.random() < 0.3:
                expr += f'.{names[i]}({lit(u[i])})'
        code, out = [], []
        self.line(code, out, [expr + '.build()'], [build(tuple(u))])
        return Probe(code, out)

    def p_resolve(self):
        r = self.r
        code, out = [], []
        bs = r.choice(BASES)
        b = parse(bs)
        bn, be = self.fresh('b'), self.fresh('e')
        code.append(f'val ({bn}, {be}) = Url.parse({lit(bs)})')
        for _ in range(r.randint(1, 5)):
            ref = r.choice(RFC_REFS) if r.random() < 0.6 else self.rurl()
            n, e = self.fresh('r'), self.fresh('e')
            code.append(f'val ({n}, {e}) = Url.resolve({bn}, {lit(ref)})')
            tag = self.fresh('t')
            code.append(f'println("{tag} {{{e}.code}} " + q(Url.build({n})))')
            try:
                t = resolve(b, ref)
                out.append(f'{tag} 0 {qtext(build(t))}')
            except UrlErr as x:
                out.append(f'{tag} {x.code} ')
        return Probe(code, out)

    def rbytes(self):
        r = self.r
        k = r.random()
        if k < 0.3:
            return bytes(r.randint(0, 255) for _ in range(r.randint(0, 12)))
        if k < 0.6:
            return ''.join(r.choice(UNRES + SUBD + ' %/?#é日\x00') for _ in range(r.randint(0, 12))).encode()
        return r.choice([b'', b'%', b'%4', b'%41', b'%4g', b'%g4', b'a%', b'%%', b'%2', b'%zz',
                         b'%E6%97%A5', b'%e6%97%a5', b'%00', b'%FF%fe', b'+', b'a b']) + \
            bytes(r.choice(b'%0aF9') for _ in range(r.randint(0, 3)))

    def p_percent(self):
        r = self.r
        code, out = [], []
        for _ in range(r.randint(1, 4)):
            b = self.rbytes()
            ro = r.random() < 0.5
            kind = r.random()
            if kind < 0.4:
                self.line(code, out, [f'Url.percent_encode({lit(b)}, {"true" if ro else "false"})'],
                          [penc(b, ro)])
            elif kind < 0.7:
                d, e = self.fresh('d'), self.fresh('e')
                code.append(f'val ({d}, {e}) = Url.percent_decode(Url.percent_encode({lit(b)}, '
                            f'{"true" if ro else "false"}))')
                tag = self.fresh('t')
                code.append(f'println("{tag} {{{e}.code}} " + q({d}))')
                out.append(f'{tag} 0 {qtext(b)}')
            else:
                d, e = self.fresh('d'), self.fresh('e')
                code.append(f'val ({d}, {e}) = Url.percent_decode({lit(b)})')
                tag = self.fresh('t')
                code.append(f'println("{tag} {{{e}.code}} " + q({d}))')
                v = pdec(b)
                out.append(f'{tag} 2007 ' if v is None else f'{tag} 0 {qtext(v)}')
        return Probe(code, out)

    def p_misc(self):
        r = self.r
        code, out = [], []
        s = r.choice(SCHEMES)
        dp = {'http': 80, 'https': 443, 'ws': 80, 'wss': 443, 'ftp': 21, 'ssh': 22, 'sftp': 22}
        self.raw(code, out, [f'Url.default_port({lit(s)})'], [dp.get(s, -1)])
        return Probe(code, out)

    def probe_kinds(self):
        return [(8, self.p_parse), (3, self.p_builder), (4, self.p_resolve), (4, self.p_percent),
                (1, self.p_misc)]


def generate(rng):
    g = UrlGen(rng)
    g.program()
    return g
