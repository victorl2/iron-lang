"""json module programs (docs/json.md): random Json trees built in Iron with
the constructors, then changed with set / push / insert / set_at / remove /
remove_at / clear (through the bindings of their containers, which share
them: one container can sit in two places), serialized compact, pretty and
with format(indent, sort_keys), parsed back and compared (equals and the
compact text as a fixed point), copied (a copy shares the containers,
clone() is deep), read with get / at / find / has / pointer / path / the
typed accessors, written again with a JsonWriter (field shortcuts, value,
raw, reuse after finish) and walked with a JsonReader into a JsonWriter.
Also deep nesting at the parser's depth limit, and the edges of Int and
Float. Strings hold escapes, control characters, non-ASCII text, a zero
byte and bytes that are not UTF-8 (written as U+FFFD).

The model is a Python mirror of the tree with the module's rules: objects
keep insertion order, set replaces the last member with the key, floats
are written in their shortest round trip form with a .0 on whole values
and an exponent outside -7..20 (src/stdlib/iron_json.c json_fmt_float),
strings escape " \\ and control characters (\\n \\r \\t \\b \\f, \\u00xx).
"""
from pf_seg import Seg, SegProgram, iron_lit, ind

INT_MAX, INT_MIN = (1 << 63) - 1, -(1 << 63)

HELPERS = '''func show(j: Json?) -> String {
    if j == null {
        return "none"
    }
    return j.to_string()
}

func opt_int(v: Int?) -> String {
    if v == null {
        return "none"
    }
    return "{v}"
}

func opt_str(v: String?) -> String {
    if v == null {
        return "none"
    }
    return v
}

func echo(var r: JsonReader, w: JsonWriter) {
    match r.peek() {
        JsonKind.Object -> {
            w.begin_object()
            if r.begin_object() {
                var k = r.next_key()
                while k != null {
                    w.key(k)
                    echo(r, w)
                    k = r.next_key()
                }
            }
            w.end_object()
        }
        JsonKind.Array -> {
            w.begin_array()
            if r.begin_array() {
                while r.next_item() {
                    echo(r, w)
                }
            }
            w.end_array()
        }
        JsonKind.Int -> w.int(r.int_or(0))
        JsonKind.Float -> w.float(r.float_or(0.0))
        JsonKind.String -> w.string(r.string_or(""))
        JsonKind.Bool -> w.bool(r.bool_or(false))
        JsonKind.Null -> {
            val n = r.null()
            w.null()
        }
    }
}
'''


# ---------------------------------------------------------------- model

class JArr:
    def __init__(self):
        self.items = []
        self.name = None


class JObj:
    def __init__(self):
        self.keys = []
        self.vals = []
        self.name = None

    def index(self, k):
        for i in range(len(self.keys) - 1, -1, -1):
            if self.keys[i] == k:
                return i
        return -1


class JStr:
    """A string as UTF-8 bytes (it may hold bytes that are not UTF-8)."""
    __slots__ = ('b',)

    def __init__(self, b):
        self.b = b

    def __eq__(self, o):
        return isinstance(o, JStr) and o.b == self.b

    def __hash__(self):
        return hash(self.b)


def fmt_float(v):
    if v != v or v in (float('inf'), float('-inf')):
        return 'null'
    if v == 0.0:
        return '-0.0' if str(v).startswith('-') else '0.0'
    r = repr(abs(v))
    if 'e' in r:
        mant, ex = r.split('e')
        ex = int(ex)
    else:
        mant, ex = r, 0
    if '.' in mant:
        ip, fp = mant.split('.')
    else:
        ip, fp = mant, ''
    digits = (ip + fp).lstrip('0')
    # decimal exponent of the first digit
    if ip.strip('0'):
        e = len(ip.lstrip('0')) - 1 + ex
    else:
        lead = len(fp) - len(fp.lstrip('0'))
        e = -lead - 1 + ex
    digits = digits.rstrip('0') or '0'
    n = len(digits)
    s = '-' if v < 0 else ''
    if -7 <= e < 21:
        if e >= 0:
            s += ''.join(digits[i] if i < n else '0' for i in range(e + 1)) + '.'
            s += digits[e + 1:] if n > e + 1 else '0'
        else:
            s += '0.' + '0' * (-e - 1) + digits
    else:
        s += digits[0] + ('.' + digits[1:] if n > 1 else '') + f'e{"-" if e < 0 else "+"}{abs(e)}'
    return s


def quote(js):
    out = ['"']
    t = js.b.decode('utf-8', errors='replace')
    for ch in t:
        o = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == '\\':
            out.append('\\\\')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\r':
            out.append('\\r')
        elif ch == '\t':
            out.append('\\t')
        elif ch == '\b':
            out.append('\\b')
        elif ch == '\f':
            out.append('\\f')
        elif o < 0x20:
            out.append('\\u%04x' % o)
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)


def ser(v, indent=0, sort=False, depth=0):
    """Iron's serialization of model value v."""
    nl = (lambda d: '\n' + ' ' * (indent * d)) if indent > 0 else (lambda d: '')
    if v is None:
        return 'null'
    if v is True:
        return 'true'
    if v is False:
        return 'false'
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return fmt_float(v)
    if isinstance(v, JStr):
        return quote(v)
    if isinstance(v, JArr):
        if not v.items:
            return '[]'
        return '[' + ','.join(nl(depth + 1) + ser(x, indent, sort, depth + 1) for x in v.items) + nl(depth) + ']'
    if isinstance(v, JObj):
        pairs = list(zip(v.keys, v.vals))
        if sort:
            seen, ps = set(), []
            for k in sorted(set(v.keys), key=lambda k: k.b):
                ps.append((k, v.vals[v.index(k)]))
            pairs = ps
        if not pairs:
            return '{}'
        sep = ': ' if indent > 0 else ':'
        return '{' + ','.join(nl(depth + 1) + quote(k) + sep + ser(x, indent, sort, depth + 1)
                              for k, x in pairs) + nl(depth) + '}'
    raise TypeError(v)


def deep_eq(a, b):
    """Json.equals on two model values (after a reparse of a's text)."""
    if isinstance(a, bool) or isinstance(b, bool) or a is None or b is None:
        return a is b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return float(a) == float(b) if (isinstance(a, float) or isinstance(b, float)) else a == b
    if isinstance(a, JStr) and isinstance(b, JStr):
        return a.b == b.b
    if isinstance(a, JArr) and isinstance(b, JArr):
        return len(a.items) == len(b.items) and all(deep_eq(x, y) for x, y in zip(a.items, b.items))
    if isinstance(a, JObj) and isinstance(b, JObj):
        if len(set(a.keys)) != len(set(b.keys)):
            return False
        for k in set(a.keys):
            j = b.index(k)
            if j < 0 or not deep_eq(a.vals[a.index(k)], b.vals[j]):
                return False
        return True
    return False


def reparse(v):
    """The model value Json.parse(ser(v)) gives: invalid UTF-8 turned into
    U+FFFD, everything else equal."""
    if isinstance(v, JStr):
        return JStr(v.b.decode('utf-8', errors='replace').encode('utf-8'))
    if isinstance(v, JArr):
        a = JArr()
        a.items = [reparse(x) for x in v.items]
        return a
    if isinstance(v, JObj):
        o = JObj()
        o.keys = [reparse(k) for k in v.keys]
        o.vals = [reparse(x) for x in v.vals]
        return o
    if isinstance(v, float) and v.is_integer() is False:
        return v
    return v


def fix_utf8(js):
    return JStr(js.b.decode('utf-8', errors='replace').encode('utf-8'))


def clone(v):
    if isinstance(v, JArr):
        a = JArr()
        a.items = [clone(x) for x in v.items]
        return a
    if isinstance(v, JObj):
        o = JObj()
        o.keys = list(v.keys)
        o.vals = [clone(x) for x in v.vals]
        return o
    return v


# --------------------------------------------------------------- strings

WORDS = ['', 'a', 'id', 'name', 'value', 'key with space', 'quote"q', 'back\\slash', 'nl\nx',
         'tab\tx', 'ctl\x01\x1f', 'del\x7f', 'é', 'émoji 😀', '中文', 'slash/x', 'tilde~0',
         'dot.key', '0', '12', 'a/b~c', '\U0010ffff', 'cr\rx', 'bs\bff\fx',
         'a long string value that is not stored inline in the Json value at all']


def iron_bytes(js):
    """An Iron expression for the string with bytes js.b."""
    b = js.b
    parts = []
    cur = bytearray()
    try:
        t = b.decode('utf-8')
        if '\x00' not in t:
            return iron_lit(t)
    except UnicodeDecodeError:
        pass
    # split into decodable runs and single bytes
    i = 0
    while i < len(b):
        for n in (4, 3, 2, 1):
            chunk = b[i:i + n]
            try:
                t = chunk.decode('utf-8')
                if len(t) == 1 and t != '\x00':
                    cur += chunk
                    i += n
                    break
            except UnicodeDecodeError:
                continue
        else:
            if cur:
                parts.append(iron_lit(cur.decode('utf-8')))
                cur = bytearray()
            parts.append(f'String.from_byte({b[i]})')
            i += 1
    if cur:
        parts.append(iron_lit(cur.decode('utf-8')))
    return ' + '.join(parts) if parts else '""'


class JsonGen(SegProgram):
    HELPERS = HELPERS
    IMPORTS = ('json',)
    NSEGS = (1, 4)

    def add_segment(self):
        r = self.r
        pick = r.choices(['tree', 'writer', 'deep', 'numbers', 'parse'], [6, 2, 1, 1, 1.5])[0]
        self.segs.append(getattr(self, 'seg_' + pick)())

    # ------------------------------------------------------- generation
    def rand_str(self):
        r = self.r
        c = r.random()
        if c < 0.6:
            return JStr(r.choice(WORDS).encode('utf-8'))
        if c < 0.7:
            return JStr(bytes([r.choice([0x00, 0xff, 0xc3, 0x80, 0xed, 0xa0])]) + b'x')
        n = r.randint(0, 30)
        chars = [r.choice(['x', ' ', '"', '\\', '\n', 'é', '中', '\U0001f600', '\x02', '/'])
                 for _ in range(n)]
        return JStr(''.join(chars).encode('utf-8'))

    def rand_float(self):
        r = self.r
        c = r.random()
        if c < 0.3:
            return r.choice([0.0, -0.0, 0.1, 0.5, 1.0, -2.5, 1e21, 1e20, 1e-7, 1e-8, 2.5e-8,
                             123456789.125, 1.7976931348623157e308, 5e-324, 1e300, -1e-300,
                             0.30000000000000004, 9007199254740993.0, 100.0, 1e16])
        if c < 0.7:
            return round(r.uniform(-1e4, 1e4), r.randint(0, 6))
        return r.uniform(-1, 1) * 10 ** r.randint(-30, 30)

    def leaf(self):
        r = self.r
        c = r.random()
        if c < 0.08:
            return None
        if c < 0.18:
            return r.random() < 0.5
        if c < 0.45:
            if r.random() < 0.2:
                return r.choice([0, -1, INT_MAX, INT_MIN, INT_MAX - 1, 1 << 53, -(1 << 53) - 1])
            return r.randint(-1000, 1000)
        if c < 0.6:
            return self.rand_float()
        return self.rand_str()

    def tree(self, depth, budget, pool):
        """A random model value; containers go into pool (for sharing)."""
        r = self.r
        if depth <= 0 or budget[0] <= 0 or r.random() < 0.3:
            budget[0] -= 1
            return self.leaf()
        if pool and r.random() < 0.08:
            return r.choice(pool)       # shared container
        budget[0] -= 1
        if r.random() < 0.5:
            a = JArr()
            for _ in range(r.randint(0, 5)):
                a.items.append(self.tree(depth - 1, budget, pool))
            pool.append(a)
            return a
        o = JObj()
        for _ in range(r.randint(0, 5)):
            k = self.rand_str()
            v = self.tree(depth - 1, budget, pool)
            i = o.index(k)
            if i >= 0:
                o.vals[i] = v
            else:
                o.keys.append(k)
                o.vals.append(v)
        pool.append(o)
        return o

    # ---------------------------------------------- Iron text for a tree
    def leaf_src(self, v):
        if v is None:
            return 'Json.null()'
        if v is True or v is False:
            return f'Json.bool({"true" if v else "false"})'
        if isinstance(v, int):
            if v == INT_MIN:
                return 'Json.int(-9223372036854775807 - 1)'
            return f'Json.int({v})'
        if isinstance(v, float):
            rp = repr(v)
            if 'e' not in rp and 'inf' not in rp and 'nan' not in rp and '.' in rp:
                return f'Json.float({rp})'
            return f'Json.float({iron_lit(rp)}.to_float())'
        return f'Json.str({iron_bytes(v)})'

    def build(self, v, lines, made):
        """Statements building v; returns an expression for it. Each
        container gets one binding (shared containers reuse it)."""
        if not isinstance(v, (JArr, JObj)):
            return self.leaf_src(v)
        if id(v) in made:
            return v.name
        made.add(id(v))
        v.name = self.fresh('j')
        if isinstance(v, JArr):
            if v.items and self.r.random() < 0.25 and all(isinstance(x, int) and not isinstance(x, bool)
                                                           and x != INT_MIN for x in v.items):
                lines.append(f'val {v.name} = Json.ints([{", ".join(map(str, v.items))}])')
                return v.name
            lines.append(f'val {v.name} = Json.array()')
            for x in v.items:
                e = self.build(x, lines, made)
                lines.append(f'{v.name}.push({e})')
            return v.name
        lines.append(f'val {v.name} = Json.obj()')
        for k, x in zip(v.keys, v.vals):
            e = self.build(x, lines, made)
            lines.append(f'{v.name}.set({iron_bytes(k)}, {e})')
        return v.name

    def containers(self, v, acc, seen):
        if isinstance(v, (JArr, JObj)) and id(v) not in seen:
            seen.add(id(v))
            acc.append(v)
            for x in (v.items if isinstance(v, JArr) else v.vals):
                self.containers(x, acc, seen)
        return acc

    def reaches(self, v, target, seen=None):
        seen = seen if seen is not None else set()
        if v is target:
            return True
        if not isinstance(v, (JArr, JObj)) or id(v) in seen:
            return False
        seen.add(id(v))
        return any(self.reaches(x, target, seen) for x in (v.items if isinstance(v, JArr) else v.vals))

    # ------------------------------------------------------------- tree
    def seg_tree(self):
        r = self.r
        pool = []
        root = self.tree(r.randint(1, 4), [r.randint(3, 30)], pool)
        if not isinstance(root, (JArr, JObj)):
            o = JObj()
            o.keys.append(JStr(b'v'))
            o.vals.append(root)
            root = o
        lines, out, made = [], [], set()
        rn = self.build(root, lines, made)
        # mutations through container bindings
        for _ in range(r.randint(0, 6)):
            cs = self.containers(root, [], set())
            c = r.choice(cs)
            if c.name is None or id(c) not in made:
                continue
            if isinstance(c, JArr):
                op = r.choice(['push', 'insert', 'set_at', 'remove_at', 'clear', 'push'])
                if op in ('insert',) and True:
                    i = r.randint(0, len(c.items))
                elif op in ('set_at',):
                    if not c.items:
                        op = 'push'
                    i = r.randint(0, max(0, len(c.items) - 1))
                else:
                    i = r.randint(-1, len(c.items))
                if op in ('push', 'insert', 'set_at'):
                    nv = self.tree(1, [3], [])
                    if isinstance(nv, (JArr, JObj)):
                        nv = self.leaf()
                    e = self.leaf_src(nv)
                if op == 'push':
                    lines.append(f'{c.name}.push({e})')
                    c.items.append(nv)
                elif op == 'insert':
                    lines.append(f'{c.name}.insert({i}, {e})')
                    c.items.insert(i, nv)
                elif op == 'set_at':
                    lines.append(f'{c.name}.set_at({i}, {e})')
                    c.items[i] = nv
                elif op == 'remove_at':
                    lines.append(f'println("removed {{show({c.name}.remove_at({i}))}}")')
                    if 0 <= i < len(c.items):
                        out.append('removed ' + ser(c.items.pop(i)))
                    else:
                        out.append('removed none')
                else:
                    lines.append(f'{c.name}.clear()')
                    c.items.clear()
            else:
                op = r.choice(['set', 'set', 'remove', 'clear', 'share'])
                if op == 'share':
                    # put a container that does not contain c into c
                    cands = [x for x in cs if x.name and id(x) in made and not self.reaches(x, c)]
                    if not cands:
                        op = 'set'
                    else:
                        x = r.choice(cands)
                        k = self.rand_str()
                        lines.append(f'{c.name}.set({iron_bytes(k)}, {x.name})')
                        i = c.index(k)
                        if i >= 0:
                            c.vals[i] = x
                        else:
                            c.keys.append(k)
                            c.vals.append(x)
                        continue
                if op == 'set':
                    k = r.choice(c.keys) if c.keys and r.random() < 0.5 else self.rand_str()
                    nv = self.leaf()
                    lines.append(f'{c.name}.set({iron_bytes(k)}, {self.leaf_src(nv)})')
                    i = c.index(k)
                    if i >= 0:
                        c.vals[i] = nv
                    else:
                        c.keys.append(k)
                        c.vals.append(nv)
                elif op == 'remove':
                    k = r.choice(c.keys) if c.keys and r.random() < 0.7 else self.rand_str()
                    lines.append(f'println("remove {{{c.name}.remove({iron_bytes(k)})}}")')
                    found = k in c.keys
                    while k in c.keys:
                        i = c.index(k)
                        c.keys.pop(i)
                        c.vals.pop(i)
                    out.append(f'remove {"true" if found else "false"}')
                else:
                    lines.append(f'{c.name}.clear()')
                    c.keys.clear()
                    c.vals.clear()
        text = ser(root)
        t = self.fresh('t')
        lines += [f'val {t} = {rn}.to_string()', f'println({t})']
        out.append(text)
        if r.random() < 0.5:
            k = r.choice([0, 1, 2, 4, 3])
            srt = r.random() < 0.5
            lines.append(f'println({rn}.format({k}, {"true" if srt else "false"}))')
            out += ser(root, k, srt).split('\n')
        if r.random() < 0.3:
            lines.append(f'println({rn}.pretty())')
            out += ser(root, 2).split('\n')
        # parse it back
        b = self.fresh('b')
        lines += [f'val {b} = Json.parse({t})',
                  f'println("back {{{b}.error}} {{{b}.value.equals({rn})}} {{{b}.value.to_string() == {t}}}")']
        rp = reparse(root)
        out.append(f'back 0 {"true" if deep_eq(rp, root) else "false"} true')
        # interpolation is to_string
        eq = self.fresh('eq')
        lines += [f'val {eq} = "{{{rn}}}" == {t}', f'println("{{{eq}}}")']
        out.append('true')
        # accessors
        lines_a, out_a = self.accessors(root, rn)
        lines += lines_a
        out += out_a
        # copies share, clone is deep
        if r.random() < 0.6:
            cp, cl = self.fresh('cp'), self.fresh('cl')
            lines += [f'val {cp} = {rn}', f'val {cl} = {rn}.clone()']
            snap = clone(root)
            nv = self.leaf()
            if isinstance(root, JArr):
                lines.append(f'{cp}.push({self.leaf_src(nv)})')
                root.items.append(nv)
            else:
                k = self.rand_str()
                lines.append(f'{cp}.set({iron_bytes(k)}, {self.leaf_src(nv)})')
                i = root.index(k)
                if i >= 0:
                    root.vals[i] = nv
                else:
                    root.keys.append(k)
                    root.vals.append(nv)
            lines += [f'println({rn}.to_string())', f'println({cl}.to_string())',
                      f'println("{{{cl}.equals({rn})}}")']
            out += [ser(root), ser(snap), 'true' if deep_eq(snap, root) else 'false']
        # walk it with a reader into a writer
        if r.random() < 0.5:
            rd, w = self.fresh('rd'), self.fresh('w')
            k = r.choice([0, 0, 2])
            lines += [f'var {rd} = JsonReader({rn}.to_string())',
                      f'val {w} = {"JsonWriter.compact()" if k == 0 else f"JsonWriter.pretty({k})"}',
                      f'echo({rd}, {w})', f'println({w}.finish())']
            out += ser(reparse(root), k).split('\n')
        return Seg(lines, out)

    def accessors(self, root, rn):
        r = self.r
        lines, out = [], []
        paths = []

        def walk(v, p):
            paths.append((list(p), v))
            if len(paths) > 40:
                return
            if isinstance(v, JArr):
                for i, x in enumerate(v.items):
                    walk(x, p + [str(i)])
            elif isinstance(v, JObj):
                for k, x in zip(v.keys, v.vals):
                    if v.index(k) == v.keys.index(k) or True:
                        walk(x, p + [k.b])
        walk(root, [])
        for _ in range(r.randint(1, 4)):
            p, v = r.choice(paths)
            toks = [t if isinstance(t, bytes) else t.encode() for t in p]
            if r.random() < 0.2:
                toks = toks + [b'missing']
            ptr = ''.join('/' + t.decode('utf-8', 'replace').replace('~', '~0').replace('/', '~1')
                          for t in toks)
            if any(b'\x00' in t or not self.utf8(t) for t in toks):
                continue
            lines.append(f'println("ptr {{show({rn}.pointer({iron_lit(ptr)}))}}")')
            out.append('ptr ' + self.show(self.lookup(root, toks)))
            if all(b'.' not in t for t in toks) and toks and r.random() < 0.5:
                pth = '.'.join(t.decode() for t in toks)
                lines.append(f'println("path {{show({rn}.path({iron_lit(pth)}))}}")')
                # path("") is the value itself
                out.append('path ' + self.show(root if pth == '' else
                                               self.lookup(root, [t for t in pth.encode().split(b'.')])))
        if isinstance(root, JObj):
            for _ in range(r.randint(0, 3)):
                k = r.choice(root.keys) if root.keys and r.random() < 0.7 else self.rand_str()
                if not self.utf8(k.b) or b'\x00' in k.b:
                    continue
                kl = iron_lit(k.b.decode())
                i = root.index(k)
                v = root.vals[i] if i >= 0 else None
                gi = v if (isinstance(v, int) and not isinstance(v, bool)) else None
                gs = v.b.decode('utf-8', 'replace') if isinstance(v, JStr) else None
                lines.append(f'println("get {{{rn}.has({kl})}} {{show({rn}.find({kl}))}} '
                             f'{{opt_int({rn}.get_int({kl}))}} {{{rn}.get({kl}).type_name()}}")')
                out.append(f'get {"true" if i >= 0 else "false"} {self.show(v) if i >= 0 else "none"} '
                           f'{gi if gi is not None else "none"} {self.type_name(v) if i >= 0 else "null"}')
                if gs is not None and not any(c in gs for c in '\n\r\v\f\x1c\x1d\x1e\x85\u2028\u2029'):
                    lines.append(f'println("str " + opt_str({rn}.get_string({kl})))')
                    out.append('str ' + gs)
        if isinstance(root, JArr) and root.items:
            for _ in range(r.randint(0, 3)):
                i = r.randint(-len(root.items) - 1, len(root.items))
                j = i + len(root.items) if i < 0 else i
                v = root.items[j] if 0 <= j < len(root.items) else None
                lines.append(f'println("at {{{rn}.at({i}).to_string()}} {{{rn}.len()}}")')
                out.append(f'at {ser(v) if 0 <= j < len(root.items) else "null"} {len(root.items)}')
        return lines, out

    @staticmethod
    def utf8(b):
        try:
            b.decode('utf-8')
            return True
        except UnicodeDecodeError:
            return False

    def show(self, v):
        return 'none' if v is NOTFOUND else ser(v)

    @staticmethod
    def type_name(v):
        if v is None:
            return 'null'
        if isinstance(v, bool):
            return 'bool'
        if isinstance(v, int):
            return 'int'
        if isinstance(v, float):
            return 'float'
        if isinstance(v, JStr):
            return 'string'
        if isinstance(v, JArr):
            return 'array'
        return 'object'

    def lookup(self, v, toks):
        for t in toks:
            if isinstance(v, JObj):
                i = v.index(JStr(t))
                if i < 0:
                    return NOTFOUND
                v = v.vals[i]
            elif isinstance(v, JArr):
                s = t.decode('utf-8', 'replace')
                if not s or len(t) > 18 or (len(t) > 1 and s[0] == '0') or not all('0' <= c <= '9' for c in s):
                    return NOTFOUND
                k = int(s)
                if k >= len(v.items):
                    return NOTFOUND
                v = v.items[k]
            else:
                return NOTFOUND
        return v

    # ----------------------------------------------------------- writer
    def seg_writer(self):
        r = self.r
        pool = []
        root = self.tree(r.randint(1, 4), [r.randint(2, 20)], pool)
        k = r.choice([0, 0, 1, 2, 4])
        w = self.fresh('w')
        lines = [f'val {w} = {"JsonWriter.compact()" if k == 0 else f"JsonWriter.pretty({k})"}']
        made = set()
        pre = []

        def emit(v, key=None):
            if key is not None and not isinstance(v, (JArr, JObj)) and r.random() < 0.6:
                kl = iron_bytes(key)
                if v is None:
                    lines.append(f'{w}.field_null({kl})')
                    return
                if isinstance(v, bool):
                    lines.append(f'{w}.field_bool({kl}, {"true" if v else "false"})')
                    return
                if isinstance(v, int) and v != INT_MIN:
                    lines.append(f'{w}.field_int({kl}, {v})')
                    return
                if isinstance(v, JStr):
                    lines.append(f'{w}.field_string({kl}, {iron_bytes(v)})')
                    return
            if key is not None:
                lines.append(f'{w}.key({iron_bytes(key)})')
            c = r.random()
            if c < 0.15:
                # a whole tree, through value()
                e = self.build(v, pre, made)
                lines.append(f'{w}.value({e})')
                return
            if c < 0.2 and k == 0:
                lines.append(f'{w}.raw({iron_lit(ser(reparse(v)))})')
                return
            if isinstance(v, JArr):
                lines.append(f'{w}.begin_array()')
                for x in v.items:
                    emit(x)
                lines.append(f'{w}.end_array()')
            elif isinstance(v, JObj):
                lines.append(f'{w}.begin_object()')
                for kk, x in zip(v.keys, v.vals):
                    emit(x, kk)
                lines.append(f'{w}.end_object()')
            elif v is None:
                lines.append(f'{w}.null()')
            elif isinstance(v, bool):
                lines.append(f'{w}.bool({"true" if v else "false"})')
            elif isinstance(v, int):
                lines.append(f'{w}.int({v if v != INT_MIN else "-9223372036854775807 - 1"})')
            elif isinstance(v, float):
                lines.append(f'{w}.value({self.leaf_src(v)})')
            else:
                lines.append(f'{w}.string({iron_bytes(v)})')
        emit(root)
        t = self.fresh('t')
        lines += [f'val {t} = {w}.finish()', f'println({t})']
        out = ser(root, k).split('\n')
        # the writer is empty again after finish
        lines += [f'{w}.int(7)', f'println("{{{w}.finish()}} {{{w}.len()}}")']
        out.append('7 0')
        return Seg(pre + lines, out)

    # ------------------------------------------------------------- deep
    def seg_deep(self):
        r = self.r
        lim = r.choice([1, 3, 64, 512])
        d = max(1, lim + r.choice([-1, 0, 1]))
        kind = r.choice(['[', '{"k":'])
        close = ']' if kind == '[' else '}'
        t = self.fresh('t')
        res = self.fresh('res')
        lines = [f'val {t} = {iron_lit(kind)}.repeat({d}) + "1" + {iron_lit(close)}.repeat({d})']
        if lim == 512 and r.random() < 0.5:
            lines.append(f'val {res} = Json.parse({t})')
        else:
            lines.append(f'val {res} = Json.parse_with({t}, JsonLimits({lim}, -1))')
        lines.append(f'println("deep {{{res}.error}}")')
        err = 4001 if d > lim else 0
        out = [f'deep {err}']
        if err == 0:
            lines.append(f'println("same {{{res}.value.to_string() == {t}}}")')
            out.append('same true')
        # a tree built deeper than the parser allows still serializes
        n = r.choice([10, 100, 600, 2000])
        cur = self.fresh('cur')
        i = self.fresh('i')
        nx = self.fresh('nx')
        lines += [f'var {cur} = Json.int(0)', f'for {i} in range({n}) {{',
                  f'    val {nx} = Json.array()', f'    {nx}.push({cur})', f'    {cur} = {nx}', '}',
                  f'val {self.fresh("s")} = {cur}.to_string()',
                  f'println("built {{{cur}.to_string().byte_len()}}")']
        out.append(f'built {2 * n + 1}')
        return Seg(lines, out)

    # ---------------------------------------------------------- numbers
    def seg_numbers(self):
        r = self.r
        lines, out = [], []
        cases = []
        for _ in range(r.randint(2, 6)):
            c = r.random()
            if c < 0.3:
                v = r.choice([INT_MAX, INT_MIN, INT_MAX + 1, INT_MIN - 1, 10 ** 19, -(10 ** 19), 10 ** 30,
                              1 << 53, (1 << 53) + 1, 0, -1])
                cases.append(str(v))
            elif c < 0.6:
                cases.append(repr(self.rand_float()).replace('inf', '1e999'))
            else:
                cases.append(r.choice(['1e400', '-1e400', '1.5e308', '2e-400', '0.1e1', '-0', '-0.0', '1E5',
                                       '1e+2', '123456789012345678901234567890.5'] + list(self.MALFORMED)))
        for txt in cases:
            res = self.fresh('res')
            lines += [f'val {res} = Json.parse({iron_lit(txt)})',
                      f'println("num {{{res}.error}} {{{res}.value.type_name()}} {{{res}.value.to_string()}}")']
            out.append('num ' + self.num_expect(txt))
        return Seg(lines, out)

    # malformed numbers: 4005 when a number starts but is not well formed,
    # 4000 when the text is not a number at all or a number has junk after it
    MALFORMED = {'01': 4005, '1.': 4005, '.5': 4000, '-': 4005, '1e': 4005, '0x10': 4000,
                 '1_0': 4000, '+1': 4000, '-01': 4005, '00': 4005, '1e-': 4005, '--1': 4005,
                 '1.e3': 4005, '1e5.5': 4000}

    @classmethod
    def num_expect(cls, txt):
        if txt in cls.MALFORMED:
            return f'{cls.MALFORMED[txt]} null null'
        import re
        if re.fullmatch(r'-?(0|[1-9][0-9]*)', txt):
            v = int(txt)
            if INT_MIN <= v <= INT_MAX:
                return f'0 int {v}'
            return f'0 float {fmt_float(float(v))}'
        v = float(txt)
        if v in (float('inf'), float('-inf')):
            return '4005 null null'
        return f'0 float {fmt_float(v)}'

    # ------------------------------------------------------------ parse
    def seg_parse(self):
        """Text with duplicate keys, whitespace and escapes, parsed and
        compared with the model."""
        r = self.r
        pool = []
        root = self.tree(r.randint(1, 3), [r.randint(2, 15)], pool)
        text = ser(reparse(root), r.choice([0, 0, 2]))
        lines, out = [], []
        if isinstance(root, JObj) and root.keys and r.random() < 0.5:
            # a duplicate key: lookups and sorted output take the last one
            k = root.keys[0]
            dup = ser(reparse(self.leaf()))
            text = text[:-1] + (',' if len(text) > 2 else '') + quote(fix_utf8(k)) + ':' + dup + '}'
            res = self.fresh('res')
            lines += [f'val {res} = Json.parse({iron_lit(text)})',
                      f'println("dup {{{res}.error}} {{{res}.value.len()}} {{{res}.value.distinct_keys()}} '
                      f'{{show({res}.value.find({iron_lit(fix_utf8(k).b.decode())}))}}")',
                      f'println({res}.value.format(0, true))']
            rr = reparse(root)
            rr.keys.append(fix_utf8(k))
            import json as pj
            dv = pj.loads(dup)
            dmodel = self.from_py(dv)
            rr.vals.append(dmodel)
            out += [f'dup 0 {len(rr.keys)} {len(set(rr.keys))} {ser(dmodel)}', ser(rr, 0, True)]
            return Seg(lines, out)
        res = self.fresh('res')
        lines += [f'val {res} = Json.parse({iron_lit(text)})',
                  f'println("parse {{{res}.error}} {{{res}.value.to_string()}}")']
        out.append(f'parse 0 {ser(reparse(root))}')
        return Seg(lines, out)

    def from_py(self, v):
        if isinstance(v, str):
            return JStr(v.encode('utf-8'))
        if isinstance(v, list):
            a = JArr()
            a.items = [self.from_py(x) for x in v]
            return a
        if isinstance(v, dict):
            o = JObj()
            for k, x in v.items():
                o.keys.append(JStr(k.encode()))
                o.vals.append(self.from_py(x))
            return o
        return v


class _NotFound:
    pass


NOTFOUND = _NotFound()


def generate(rng):
    g = JsonGen(rng)
    g.program()
    return g
