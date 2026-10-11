"""String and collection edge cases (manual 2.1, 2.3, 3.3, 3.4, 6.8, 9.2,
9.3, 9.10): every String method on empty strings, multi-byte characters
(2, 3 and 4 byte UTF-8, written raw or as \\u{H} escapes), strings holding
the zero byte (String.from_byte(0)), out of range positions (clamped, never
a panic), separators and patterns that overlap, repeat, pad with wide
characters, to_int / to_float on numbers with surrounding whitespace and
on text that is not a number; comparisons in code point order; lists with
insert / remove at both ends, pop, slices, reverse, sort, contains, copy,
take, clear, map / filter / reduce / sum; Map and Set with thousands of
entries, removal and re-insertion, overwrite, copy and take, empty and
zero-byte keys, maps of lists and maps of maps, lists of maps changed in
place, iteration (whose order is unspecified: the probes print only
order-independent results); and the panics: get on a missing key, pop of
an empty list, an index out of range.

Left out where the manual says nothing (see the report): split / replace
with an empty pattern, count of "", a pad of several characters or none,
case mapping outside the characters every case table agrees on, a sign or
an exponent in to_int / to_float, out of range list slices, sort stability
and NaN in sort, and changing a map while iterating it.
"""
import re

from pf_probe import Probe, ProbeGen
from pf_lang import Bail

PIECES = ['a', 'b', 'ab', 'ba', 'aa', 'Z', 'x1', ' ', '  ', '\t', '\n', ',', '-', ', ', 'é', 'Ö',
          'ñ', '日', '本', '😀', 'Σ', 'д', '42', '-7', '0', '{', '}', '"', '\\', '', 'é日',
          'aé', '\x00', 'ü']
# characters whose upper / lower case every simple case table agrees on
CASE_OK = set('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZéÉöÖñÑüÜσΣдД日本😀 \t,-0123456789x')
WS = ' \t\n\r\x0b\x0c'


def lit_of(s, rng=None):
    """An Iron expression for the string s: a literal, with \\u{H} escapes
    now and then, and the zero byte spliced in with String.from_byte(0)."""
    if '\x00' in s:
        parts = s.split('\x00')
        out = []
        for i, p in enumerate(parts):
            if i:
                out.append('String.from_byte(0)')
            if p or (i == 0 and len(parts) == 1):
                out.append(lit_of(p, rng))
        if not out:
            return '""'
        return '(' + ' + '.join(out) + ')' if len(out) > 1 else out[0]
    o = []
    for ch in s:
        if ch == '\\':
            o.append('\\\\')
        elif ch == '"':
            o.append('\\"')
        elif ch == '{':
            o.append('\\{')
        elif ch == '}':
            o.append('\\}')
        elif ch == '\n':
            o.append('\\n')
        elif ch == '\t':
            o.append('\\t')
        elif ord(ch) > 127 and rng is not None and rng.random() < 0.3:
            o.append('\\u{%x}' % ord(ch))
        else:
            o.append(ch)
    return '"' + ''.join(o) + '"'


def substring(s, a, b):
    n = len(s)
    a = max(0, min(a, n))
    b = max(0, min(b, n))
    return s[a:b] if a < b else ''


def to_int(s):
    t = s.strip(WS)
    if re.fullmatch(r'-?[0-9]+', t):
        v = int(t)
        if -(1 << 63) <= v < (1 << 63):
            return v
    return 0


def to_float(s):
    t = s.strip(WS)
    if re.fullmatch(r'-?[0-9]+(\.[0-9]+)?', t):
        return float(t)
    return 0.0


def pad_left(s, w, ch):
    return ch * (w - len(s)) + s if len(s) < w else s


def pad_right(s, w, ch):
    return s + ch * (w - len(s)) if len(s) < w else s


def show_list(xs):
    return '[' + '|'.join(xs) + ']'


HELPERS = '''func show(xs: [String]) -> String {
    return "[" + "|".join(xs) + "]"
}

func showi(xs: [Int]) -> String {
    var parts: [String] = []
    for x in xs {
        parts.push("{x}")
    }
    return "[" + ",".join(parts) + "]"
}

func pk(n: Int) -> Int {
    return n
}'''


def fmt_float(v):
    from pf_num import fmt_float as f
    return f(v)


class Code(list):
    """Source lines of a probe whose queries (expression, text) wait in
    `pending` for the next println: a statement appended first prints them,
    so the side effects of the queries happen in model order."""

    def __init__(self, gen, out, init=()):
        super().__init__(init)
        self.gen, self.out, self.pending = gen, out, []

    def append(self, line):
        if self.pending:
            self.gen.emit(self, self.out, self.pending)
        list.append(self, line)

    def __iadd__(self, lines):
        for l in lines:
            self.append(l)
        return self


class StrCollGen(ProbeGen):
    NPROBES = (5, 12)

    def __init__(self, rng):
        super().__init__(rng)
        self.need('helpers', HELPERS)

    def rstr(self, case_ok=False, nul=True, maxp=5):
        r = self.r
        pieces = [p for p in PIECES if (nul or '\x00' not in p) and
                  (not case_ok or all(c in CASE_OK for c in p))]
        return ''.join(r.choice(pieces) for _ in range(r.randint(0, maxp)))

    def emit(self, code, out, exprs):
        """println of a tagged line of (expression, text) pairs; the text may
        hold newlines (it splits into several output lines)."""
        if not exprs:
            return
        tag = self.fresh('s')
        list.append(code, f'println("{tag} ' + ' '.join(f'{{{e}}}' for e, _ in exprs) + '")')
        text = f'{tag} ' + ' '.join(t for _, t in exprs)
        out += text.split('\n')
        exprs.clear()

    # ------------------------------------------------------------ strings
    def p_string(self):
        r = self.r
        code, out = [], []
        names = []
        exprs = []
        for _ in range(r.randint(1, 3)):
            s = self.rstr(case_ok=r.random() < 0.5)
            n = self.fresh('w')
            code.append(f'val {n} = {lit_of(s, r)}')
            names.append((n, s))
        for _ in range(r.randint(2, 7)):
            n, s = r.choice(names)
            e = self.str_query(n, s)
            if e:
                exprs.append(e)
        if not exprs:
            return None
        # A line with a zero byte or a newline in it is wrapped in markers
        self.emit(code, out, exprs)
        return Probe(code, out)

    def str_query(self, n, s):
        """One String method applied to n (value s): (expr, text)."""
        r = self.r
        sub = r.choice([self.rstr(maxp=2), s[r.randint(0, len(s)):][:r.randint(0, 3)], ''])
        L = len(s)
        i = r.choice([-5, -1, 0, 1, 2, L - 1, L, L + 1, 100, r.randint(0, L + 2)])
        j = r.choice([-1, 0, 1, L - 1, L, L + 3, i, i + 1, r.randint(0, L + 2)])
        k = r.choice(['len', 'byte_len', 'upper', 'lower', 'trim', 'contains', 'starts_with',
                      'ends_with', 'index_of', 'rindex_of', 'count', 'split', 'chars', 'join',
                      'replace', 'repeat', 'substring', 'char_at', 'index', 'slice', 'pad_left',
                      'pad_right', 'byte_at', 'concat', 'cmp', 'eq', 'to_int', 'to_float'])
        ls = lit_of(sub, r)
        if k == 'len':
            return f'{n}.len()', str(L)
        if k == 'byte_len':
            return f'{n}.byte_len()', str(len(s.encode()))
        if k in ('upper', 'lower'):
            if not all(c in CASE_OK or c in '\x00' for c in s) or '\x00' in s:
                return None
            v = s.upper() if k == 'upper' else s.lower()
            return f'"<" + {n}.{k}() + ">"', f'<{v}>'
        if k == 'trim':
            # whitespace: the ASCII spaces; a zero byte at an end is not one
            if any(c < ' ' and c not in WS and c != '\x00' for c in s):
                return None
            return f'"<" + {n}.trim() + ">"', f'<{s.strip(WS)}>'
        if k in ('contains', 'starts_with', 'ends_with'):
            v = {'contains': sub in s, 'starts_with': s.startswith(sub),
                 'ends_with': s.endswith(sub)}[k]
            return f'{n}.{k}({ls})', 'true' if v else 'false'
        if k == 'index_of':
            return f'{n}.index_of({ls})', str(s.find(sub))
        if k == 'rindex_of':
            return f'{n}.rindex_of({ls})', str(s.rfind(sub))
        if k == 'count':
            if not sub:
                return None
            return f'{n}.count({ls})', str(s.count(sub))
        if k == 'split':
            sep = sub or r.choice([',', 'a', ', ', 'aa', '日'])
            return f'show({n}.split({lit_of(sep, r)}))', show_list(s.split(sep))
        if k == 'chars':
            return f'show({n}.chars())', show_list(list(s))
        if k == 'join':
            parts = [self.rstr(maxp=2) for _ in range(r.randint(0, 3))]
            pl = '[' + ', '.join(lit_of(p, r) for p in parts) + ']'
            if not parts:
                return f'"<" + {n}.join(fill(0, "")) + ">"', '<>'
            return f'"<" + {n}.join({pl}) + ">"', f'<{s.join(parts)}>'
        if k == 'replace':
            if not sub:
                return None
            new = self.rstr(maxp=2)
            return f'"<" + {n}.replace({ls}, {lit_of(new, r)}) + ">"', f'<{s.replace(sub, new)}>'
        if k == 'repeat':
            c = r.choice([-3, -1, 0, 1, 2, 5])
            return f'"<" + {n}.repeat({c}) + ">"', f'<{s * max(0, c)}>'
        if k == 'substring':
            return f'"<" + {n}.substring({i}, {j}) + ">"', f'<{substring(s, i, j)}>'
        if k == 'slice':
            if i < 0 or j < 0:
                return None     # a constant negative bound
            if r.random() < 0.5:
                return f'"<" + {n}[pk({i})..] + ">"', f'<{substring(s, i, L)}>'
            return f'"<" + {n}[pk({i})..pk({j})] + ">"', f'<{substring(s, i, j)}>'
        if k in ('char_at', 'index'):
            v = s[i] if 0 <= i < L else ''
            if k == 'index':
                return f'"<" + {n}[pk({i})] + ">"', f'<{v}>'
            return f'"<" + {n}.char_at({i}) + ">"', f'<{v}>'
        if k in ('pad_left', 'pad_right'):
            ch = r.choice(['0', ' ', '-', 'é', '日', '😀'])
            w = r.choice([-1, 0, L, L + 1, L + 3, 8])
            f = pad_left if k == 'pad_left' else pad_right
            return f'"<" + {n}.{k}({w}, {lit_of(ch)}) + ">"', f'<{f(s, w, ch)}>'
        if k == 'byte_at':
            b = s.encode()
            bi = r.choice([-1, 0, 1, len(b) - 1, len(b), len(b) + 2])
            return f'{n}.byte_at({bi})', str(b[bi] if 0 <= bi < len(b) else -1)
        if k == 'concat':
            t = self.rstr(maxp=2)
            return f'({n} + {lit_of(t, r)}).len()', str(len(s + t))
        if k == 'cmp':
            t = self.rstr(maxp=3)
            op = r.choice(['<', '>', '<=', '>='])
            v = {'<': s < t, '>': s > t, '<=': s <= t, '>=': s >= t}[op]
            return f'{n} {op} {lit_of(t, r)}', 'true' if v else 'false'
        if k == 'eq':
            t = r.choice([s, self.rstr(maxp=3), s + '\x00', s[:-1]])
            return f'{n} == {lit_of(t, r)}', 'true' if s == t else 'false'
        return None

    def p_parse(self):
        r = self.r
        exprs = []
        for _ in range(r.randint(1, 5)):
            c = r.random()
            ws1 = ''.join(r.choice([' ', '\t', '\n', '']) for _ in range(r.randint(0, 2)))
            ws2 = ''.join(r.choice([' ', '\t', '\n', '']) for _ in range(r.randint(0, 2)))
            if c < 0.4:
                body = r.choice(['0', '42', '-7', '007', '-0', '9223372036854775807',
                                 '-9223372036854775808', str(r.randint(-10**12, 10**12))])
            elif c < 0.7:
                body = r.choice(['1.5', '-0.25', '3', '100.125', '0.1', '-2.0'])
            else:
                body = r.choice(['', 'x', '4 2', '3x', '-', '1..2', 'abc', '--1',
                                 '1-', '日', '1\x002'])
            s = ws1 + body + ws2
            if r.random() < 0.5:
                exprs.append((f'{lit_of(s, r)}.to_int()', str(to_int(s))))
            else:
                exprs.append((f'{lit_of(s, r)}.to_float()', fmt_float(to_float(s))))
        code, out = [], []
        self.emit(code, out, exprs)
        return Probe(code, out)

    def p_build(self):
        """A string built in a loop with +=, then measured."""
        r = self.r
        n = self.fresh('b')
        piece = self.rstr(maxp=2) or 'q'
        k = r.randint(0, 300)
        code = [f'var {n} = ""', f'for i in range({k}) {{', f'    {n} += {lit_of(piece, r)}', '}']
        s = piece * k
        out = []
        sub = piece[:1] or 'q'
        self.emit(code, out, [(f'{n}.len()', str(len(s))), (f'{n}.byte_len()', str(len(s.encode()))),
                              (f'{n}.count({lit_of(sub)})', str(s.count(sub))),
                              (f'{n}.rindex_of({lit_of(sub)})', str(s.rfind(sub)))])
        return Probe(code, out)

    def p_print(self):
        """Strings printed whole: escapes, zero bytes and newlines."""
        r = self.r
        s = self.rstr(maxp=6)
        n = self.fresh('p')
        code = [f'val {n} = {lit_of(s, r)}']
        out = []
        if r.random() < 0.5:
            code.append(f'println({n})')
            out += s.split('\n')
        else:
            code.append(f'print({n})')
            code.append('println("|")')
            out += (s + '|').split('\n')
        return Probe(code, out)

    # -------------------------------------------------------------- lists
    def p_list(self):
        r = self.r
        n = self.fresh('xs')
        xs = [r.randint(-5, 9) for _ in range(r.randint(0, 6))]
        out = []
        code = Code(self, out, [f'var {n}: [Int] = [' + ', '.join(map(str, xs)) + ']'])
        exprs = code.pending
        for _ in range(r.randint(2, 8)):
            k = r.choice(['push', 'pop', 'insert0', 'insertn', 'insertmid', 'remove0', 'removen',
                          'reverse', 'sort', 'contains', 'slice', 'set', 'clear', 'copy',
                          'take', 'map', 'filter', 'reduce', 'sum', 'get'])
            L = len(xs)
            if k == 'push':
                v = r.randint(-9, 9)
                code.append(f'{n}.push({v})')
                xs.append(v)
            elif k == 'pop' and L:
                exprs.append((f'{n}.pop()', str(xs.pop())))
            elif k == 'insert0':
                v = r.randint(-9, 9)
                code.append(f'{n}.insert(0, {v})')
                xs.insert(0, v)
            elif k == 'insertn':
                v = r.randint(-9, 9)
                code.append(f'{n}.insert({n}.len(), {v})')
                xs.append(v)
            elif k == 'insertmid' and L:
                i = r.randint(0, L)
                code.append(f'{n}.insert(pk({i}), 7)')
                xs.insert(i, 7)
            elif k == 'remove0' and L:
                exprs.append((f'{n}.remove(0)', str(xs.pop(0))))
            elif k == 'removen' and L:
                exprs.append((f'{n}.remove({n}.len() - 1)', str(xs.pop())))
            elif k == 'reverse':
                code.append(f'{n}.reverse()')
                xs.reverse()
            elif k == 'sort':
                code.append(f'{n}.sort()')
                xs.sort()
            elif k == 'contains':
                v = r.randint(-5, 9)
                exprs.append((f'{n}.contains({v})', 'true' if v in xs else 'false'))
            elif k == 'slice' and L:
                a = r.randint(0, L)
                b = r.randint(a, L)
                if r.random() < 0.5:
                    exprs.append((f'showi({n}[pk({a})..pk({b})])', '[' + ','.join(map(str, xs[a:b])) + ']'))
                else:
                    exprs.append((f'showi({n}[pk({a})..])', '[' + ','.join(map(str, xs[a:])) + ']'))
            elif k == 'set' and L:
                i = r.randint(0, L - 1)
                v = r.randint(-9, 9)
                code.append(f'{n}.set({i}, {v})' if r.random() < 0.5 else f'{n}[{i}] = {v}')
                xs[i] = v
            elif k == 'get' and L:
                i = r.randint(0, L - 1)
                exprs.append((f'{n}.get({i})', str(xs[i])))
            elif k == 'clear' and r.random() < 0.3:
                code.append(f'{n}.clear()')
                xs = []
            elif k == 'copy':
                m = self.fresh('cp')
                code += [f'var {m} = {n}.copy()', f'{m}.push(99)']
                exprs.append((f'{m}.len()', str(L + 1)))
            elif k == 'take' and r.random() < 0.3:
                m = self.fresh('tk')
                code.append(f'val {m} = {n}.take()')
                exprs.append((f'showi({m})', '[' + ','.join(map(str, xs)) + ']'))
                xs = []
            elif k == 'map':
                exprs.append((f'showi({n}.map(func(x: Int) -> Int {{ return x * 2 - 1 }}))',
                              '[' + ','.join(str(x * 2 - 1) for x in xs) + ']'))
            elif k == 'filter':
                exprs.append((f'showi({n}.filter(func(x: Int) -> Bool {{ return x % 2 == 0 }}))',
                              '[' + ','.join(str(x) for x in xs if x % 2 == 0) + ']'))
            elif k == 'reduce':
                acc = 1
                for x in xs:
                    acc = acc * 3 + x
                exprs.append((f'{n}.reduce(1, func(a: Int, x: Int) -> Int {{ return a * 3 + x }})',
                              str(acc)))
            elif k == 'sum':
                exprs.append((f'{n}.sum()', str(sum(xs))))
            if len(exprs) >= 4:
                self.emit(code, out, exprs)
        exprs.append((f'showi({n})', '[' + ','.join(map(str, xs)) + ']'))
        exprs.append((f'{n}.len()', str(len(xs))))
        self.emit(code, out, exprs)
        return Probe(code, out)

    def p_slist(self):
        """A list of strings sorted (code point order), reversed, joined."""
        r = self.r
        n = self.fresh('ss')
        xs = [self.rstr(maxp=3) for _ in range(r.randint(0, 7))]
        code = [f'var {n}: [String] = [' + ', '.join(lit_of(x, r) for x in xs) + ']']
        out = []
        exprs = []
        if r.random() < 0.7:
            code.append(f'{n}.sort()')
            xs.sort()
        if r.random() < 0.3:
            code.append(f'{n}.reverse()')
            xs.reverse()
        v = r.choice(xs) if xs and r.random() < 0.6 else self.rstr(maxp=2)
        exprs.append((f'{n}.contains({lit_of(v, r)})', 'true' if v in xs else 'false'))
        exprs.append((f'show({n})', show_list(xs)))
        self.emit(code, out, exprs)
        return Probe(code, out)

    def p_flist(self):
        r = self.r
        from pf_numedge import flit
        n = self.fresh('fs')
        xs = [r.choice([0.5, -1.25, 3.0, 0.1, 100.0, -0.0, 2.5]) for _ in range(r.randint(0, 6))]
        code = [f'var {n}: [Float] = [' + ', '.join(
            f'(-{flit(-x)})' if str(x).startswith('-') else flit(x) for x in xs) + ']'
            if xs else f'var {n}: [Float] = []']
        out = []
        s = 0.0
        for x in xs:
            s += x
        code.append(f'{n}.sort()')
        ys = sorted(xs)
        self.emit(code, out, [(f'{n}.sum()', fmt_float(s)), (f'{n}.len()', str(len(xs)))] +
                  ([(f'{n}[0]', fmt_float(ys[0])), (f'{n}[{len(ys) - 1}]', fmt_float(ys[-1]))]
                   if ys else []))
        return Probe(code, out)

    # --------------------------------------------------------------- maps
    def key_expr(self, kt, k):
        if kt == 'String':
            return lit_of(k, self.r)
        if kt == 'Bool':
            return 'true' if k else 'false'
        return str(k) if k >= 0 else f'(0 - {-k})' if k != -(1 << 63) else '(0 - 9223372036854775807 - 1)'

    def rkey(self, kt):
        r = self.r
        if kt == 'String':
            return self.rstr(maxp=2)
        if kt == 'Bool':
            return r.random() < 0.5
        return r.choice([0, 1, -1, 2, 7, 1000, -(1 << 63), (1 << 63) - 1, r.randint(-50, 50)])

    def p_map(self):
        r = self.r
        kt = r.choice(['Int', 'String', 'String', 'Bool'])
        m = self.fresh('m')
        d = {}
        out = []
        code = Code(self, out, [f'var {m} = Map[{kt}, Int]()'])
        exprs = code.pending
        for _ in range(r.randint(2, 10)):
            k = self.rkey(kt)
            ke = self.key_expr(kt, k)
            c = r.random()
            if c < 0.4:
                v = r.randint(-100, 100)
                code.append(f'{m}.put({ke}, {v})')
                d[k] = v
            elif c < 0.55:
                exprs.append((f'{m}.remove({ke})', 'true' if k in d else 'false'))
                d.pop(k, None)
            elif c < 0.7:
                exprs.append((f'{m}.get_or({ke}, -1)', str(d.get(k, -1))))
            elif c < 0.8:
                exprs.append((f'{m}.has({ke})', 'true' if k in d else 'false'))
            elif c < 0.87 and k in d:
                exprs.append((f'{m}.get({ke})', str(d[k])))
            elif c < 0.92:
                exprs.append((f'{m}.len()', str(len(d))))
            elif c < 0.95:
                code.append(f'{m}.clear()')
                d = {}
            else:
                cp = self.fresh('mc')
                code += [f'var {cp} = {m}.copy()', f'{cp}.put({ke}, 5000)']
                exprs.append((f'{cp}.len()', str(len(d) + (0 if k in d else 1))))
            if len(exprs) >= 4:
                self.emit(code, out, exprs)
        # order-independent views
        tot = self.fresh('t')
        cnt = self.fresh('c')
        code += [f'var {tot} = 0', f'var {cnt} = 0', f'for (k, v) in {m} {{',
                 f'    {tot} += v', f'    {cnt} += 1', '}']
        exprs.append((tot, str(sum(d.values()))))
        exprs.append((cnt, str(len(d))))
        if kt == 'String':
            ks = self.fresh('ks')
            code += [f'var {ks} = {m}.keys()', f'{ks}.sort()']
            exprs.append((f'show({ks})', show_list(sorted(d))))
        elif kt == 'Int':
            ks = self.fresh('ks')
            code += [f'var {ks} = {m}.keys()', f'{ks}.sort()']
            exprs.append((f'showi({ks})', '[' + ','.join(map(str, sorted(d))) + ']'))
        vs = self.fresh('vs')
        code += [f'var {vs} = {m}.values()', f'{vs}.sort()']
        exprs.append((f'showi({vs})', '[' + ','.join(map(str, sorted(d.values()))) + ']'))
        self.emit(code, out, exprs)
        return Probe(code, out)

    def p_bigmap(self):
        """Thousands of entries, removals and re-insertions."""
        r = self.r
        n = r.randint(500, 5000)
        mod = r.randint(2, 7)
        a, b = r.randint(1, 1000), r.randint(0, 1000)
        m = self.fresh('bm')
        st = self.fresh('st')
        use_set = r.random() < 0.4
        keyf = r.choice(['int', 'str'])
        kt = 'Int' if keyf == 'int' else 'String'

        def key_src(i):
            return f'({i} * {a} + {b}) % {n * 2}' if keyf == 'int' else f'"k{{({i} * {a} + {b}) % {n * 2}}}"'

        def key_val(i):
            v = (i * a + b) % (n * 2)
            return v if keyf == 'int' else f'k{v}'
        code = [f'var {m} = Map[{kt}, Int]()', f'var {st} = Set[{kt}]()',
                f'for i in range({n}) {{', f'    {m}.put({key_src("i")}, i)', f'    {st}.add({key_src("i")})', '}',
                f'for i in range({n}) {{', f'    if i % {mod} == 0 {{',
                f'        {m}.remove({key_src("i")})', f'        {st}.remove({key_src("i")})', '    }', '}',
                f'for i in range({n // 3}) {{', f'    {m}.put({key_src("i * 2")}, i + 7)', '}']
        d = {}
        s = set()
        for i in range(n):
            d[key_val(i)] = i
            s.add(key_val(i))
        for i in range(n):
            if i % mod == 0:
                d.pop(key_val(i), None)
                s.discard(key_val(i))
        for i in range(n // 3):
            d[key_val(i * 2)] = i + 7
        tot = self.fresh('t')
        code += [f'var {tot} = 0', f'for (k, v) in {m} {{', f'    {tot} += v', '}']
        stc = self.fresh('sc')
        code += [f'var {stc} = 0', f'for x in {st} {{', f'    {stc} += 1', '}']
        out = []
        probe_k = r.randint(0, n)
        exprs = [(f'{m}.len()', str(len(d))), (tot, str(sum(d.values()))), (f'{st}.len()', str(len(s))),
                 (stc, str(len(s))), (f'{m}.get_or({key_src(probe_k)}, -1)', str(d.get(key_val(probe_k), -1))),
                 (f'{st}.has({key_src(probe_k)})', 'true' if key_val(probe_k) in s else 'false')]
        if use_set:
            exprs.append((f'{st}.add({key_src(probe_k)})', 'false' if key_val(probe_k) in s else 'true'))
        self.emit(code, out, exprs)
        return Probe(code, out)

    def p_set(self):
        r = self.r
        kt = r.choice(['Int', 'String'])
        st = self.fresh('set')
        s = set()
        out = []
        code = Code(self, out, [f'var {st} = Set[{kt}]()'])
        exprs = code.pending
        for _ in range(r.randint(2, 9)):
            k = self.rkey(kt)
            ke = self.key_expr(kt, k)
            c = r.random()
            if c < 0.5:
                exprs.append((f'{st}.add({ke})', 'false' if k in s else 'true'))
                s.add(k)
            elif c < 0.7:
                exprs.append((f'{st}.remove({ke})', 'true' if k in s else 'false'))
                s.discard(k)
            elif c < 0.9:
                exprs.append((f'{st}.has({ke})', 'true' if k in s else 'false'))
            else:
                tk = self.fresh('tk')
                code.append(f'var {tk} = {st}.take()')
                exprs.append((f'{tk}.len()', str(len(s))))
                exprs.append((f'{st}.len()', '0'))
                s = set()
            if len(exprs) >= 4:
                self.emit(code, out, exprs)
        exprs.append((f'{st}.len()', str(len(s))))
        vs = self.fresh('vs')
        code += [f'var {vs} = {st}.values()', f'{vs}.sort()']
        if kt == 'Int':
            exprs.append((f'showi({vs})', '[' + ','.join(map(str, sorted(s))) + ']'))
        else:
            exprs.append((f'show({vs})', show_list(sorted(s))))
        self.emit(code, out, exprs)
        return Probe(code, out)

    def p_nested(self):
        """Maps of lists, maps of maps and lists of maps."""
        r = self.r
        c = r.random()
        out = []
        if c < 0.35:
            m = self.fresh('ml')
            code = Code(self, out, [f'var {m} = Map[String, [Int]]()'])
            exprs = code.pending
            d = {}
            for _ in range(r.randint(1, 5)):
                k = r.choice(['a', 'b', 'é', ''])
                xs = [r.randint(0, 9) for _ in range(r.randint(0, 3))]
                if xs:
                    code.append(f'{m}.put({lit_of(k)}, [' + ', '.join(map(str, xs)) + '])')
                else:
                    code.append(f'{m}.put({lit_of(k)}, fill(0, 0))')
                d[k] = xs
            k = r.choice(list(d))
            g = self.fresh('g')
            code += [f'var {g} = {m}.get({lit_of(k)})', f'{g}.push(42)']
            exprs += [(f'{m}.get({lit_of(k)}).len()', str(len(d[k]))), (f'{g}.len()', str(len(d[k]) + 1))]
            if r.random() < 0.5:
                code.append(f'{m}.put({lit_of(k)}, {g}.take())')
                d[k] = d[k] + [42]
                exprs.append((f'showi({m}.get({lit_of(k)}))', '[' + ','.join(map(str, d[k])) + ']'))
            self.emit(code, out, exprs)
            return Probe(code, out)
        if c < 0.65:
            m = self.fresh('mm')
            code = [f'var {m} = Map[Int, Map[Int, Int]]()']
            d = {}
            for _ in range(r.randint(1, 4)):
                k = r.randint(0, 3)
                inner = self.fresh('in')
                code.append(f'var {inner} = Map[Int, Int]()')
                dd = {}
                for _ in range(r.randint(0, 3)):
                    a, b = r.randint(0, 5), r.randint(0, 50)
                    code.append(f'{inner}.put({a}, {b})')
                    dd[a] = b
                code.append(f'{m}.put({k}, {inner}.take())' if r.random() < 0.5 else
                            f'{m}.put({k}, {inner}.copy())')
                d[k] = dd
            k = r.choice(list(d))
            a = r.randint(0, 5)
            self.emit(code, out, [(f'{m}.len()', str(len(d))),
                                  (f'{m}.get({k}).len()', str(len(d[k]))),
                                  (f'{m}.get({k}).get_or({a}, -1)', str(d[k].get(a, -1)))])
            return Probe(code, out)
        ls = self.fresh('lm')
        nmaps = r.randint(1, 4)
        code = [f'var {ls}: [Map[String, Int]] = []']
        ds = []
        for _ in range(nmaps):
            code.append(f'{ls}.push(Map[String, Int]())')
            ds.append({})
        for _ in range(r.randint(1, 6)):
            i = r.randint(0, nmaps - 1)
            k = r.choice(['x', 'y', 'ö', ''])
            v = r.randint(0, 9)
            code.append(f'{ls}[{i}].put({lit_of(k)}, {v})')
            ds[i][k] = v
        i = r.randint(0, nmaps - 1)
        cp = self.fresh('lc')
        code += [f'var {cp} = {ls}.copy()', f'{cp}[{i}].put("new", 1)']
        self.emit(code, out, [(f'{ls}[{i}].len()', str(len(ds[i]))),
                              (f'{cp}[{i}].len()', str(len(ds[i]) + (0 if 'new' in ds[i] else 1))),
                              (f'{ls}.len()', str(nmaps))])
        return Probe(code, out)

    # ------------------------------------------------------------- panics
    def p_missing_key(self):
        m = self.fresh('m')
        tag = self.fresh('z')
        return Probe([f'var {m} = Map[String, Int]()', f'{m}.put("a", 1)', f'println("{tag} {{{m}.get("a")}}")',
                      f'println("{{{m}.get("b")}}")'], [f'{tag} 1'], panic='key not found in map')

    def p_pop_empty(self):
        n = self.fresh('xs')
        tag = self.fresh('z')
        return Probe([f'var {n}: [Int] = [1]', f'println("{tag} {{{n}.pop()}}")', f'println("{{{n}.pop()}}")'],
                     [f'{tag} 1'], panic='pop from an empty list')

    def p_oob(self):
        r = self.r
        n = self.fresh('xs')
        tag = self.fresh('z')
        L = r.randint(0, 4)
        i = r.choice([L, L + 1, -1, 100])
        k = r.choice(['index', 'remove', 'insert', 'set'])
        if k == 'insert' and i == L:
            i = L + 1
        code = [f'var {n}: [Int] = [' + ', '.join(str(x) for x in range(L)) + ']',
                f'println("{tag} {{{n}.len()}}")']
        if k == 'index':
            code.append(f'println("{{{n}[pk({i})]}}")')
        elif k == 'remove':
            code.append(f'println("{{{n}.remove(pk({i}))}}")')
        elif k == 'insert':
            code.append(f'{n}.insert(pk({i}), 5)')
        else:
            code.append(f'{n}[pk({i})] = 5')
        return Probe(code, [f'{tag} {L}'], panic='index out of bounds')

    def probe_kinds(self):
        return [(8, self.p_string), (2, self.p_parse), (1, self.p_build), (2, self.p_print),
                (4, self.p_list), (2, self.p_slist), (1, self.p_flist), (4, self.p_map),
                (1, self.p_bigmap), (2, self.p_set), (2, self.p_nested)]

    def panic_kinds(self):
        return [self.p_missing_key, self.p_pop_empty, self.p_oob]


def generate(rng):
    g = StrCollGen(rng)
    g.program()
    return g
