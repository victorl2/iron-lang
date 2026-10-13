"""Random Iron programs for the debugger fuzzer (dbg_fuzz.py).

A program is a list of functions whose bodies declare typed bindings,
change some of them, and reach probe lines: `println("@P<n> ...")`. Each
probe line is a breakpoint; at the stop the debugger must show every
binding in scope with the value the generator's model says it holds, and
the Iron expressions attached to the probe must evaluate to what the
program prints on that same line (the probe's println carries them, so
the program's own output is the oracle for the expression values).

Values are modelled as small Python objects (V* classes below) that know
their Iron type, how to write themselves as Iron source and how a
debugger front end shows them (`disp`, the display contract of
src/debug/iron_lldb.py, iron_gdb.py and iron_dap.py).
"""
import math
import struct

MASK64 = (1 << 64) - 1


# ── Iron text forms ──────────────────────────────────────────────────────────

def f32(x):
    try:
        return struct.unpack("f", struct.pack("f", x))[0]
    except OverflowError:   # past Float32's range: it rounds to infinity
        return math.copysign(math.inf, x)


def float_text(v, is_f32=False):
    """Iron's text of a float (src/runtime/iron_fmt.c iron_fmt_float)."""
    if math.isnan(v):
        return "NaN"
    if math.isinf(v):
        return "inf" if v > 0 else "-inf"
    if v == 0:
        return "0"
    for p in range(1, 10 if is_f32 else 18):
        sci = "%.*e" % (p - 1, v)
        back = float(sci)
        if (f32(back) == f32(v)) if is_f32 else back == v:
            break
    neg = sci.startswith("-")
    mant, e = sci.lstrip("-").split("e")
    e = int(e)
    digits = mant.replace(".", "").rstrip("0") or "0"
    n = len(digits)
    out = "-" if neg else ""
    if -6 <= e < 21:
        if e >= 0:
            out += "".join(digits[i] if i < n else "0" for i in range(e + 1))
            if n > e + 1:
                out += "." + digits[e + 1:]
        else:
            out += "0." + "0" * (-e - 1) + digits
    else:
        out += digits[0] + ("." + digits[1:] if n > 1 else "")
        out += "e%s%d" % ("-" if e < 0 else "+", abs(e))
    return out


def float_lit(v, is_f32=False):
    """An Iron float literal for v (no exponent form: the digits written out)."""
    import decimal
    txt = format(decimal.Decimal(float_text(abs(v), is_f32)), "f")
    if "." not in txt:
        txt += ".0"
    return ("-" + txt) if v < 0 else txt


def str_display(s):
    """A String as the debugger shows it: an Iron literal (the escapes of
    manual 1.4, other control characters as \\u{H})."""
    out = []
    for ch in s:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif o < 0x20 or o == 0x7f:
            out.append("\\u{%x}" % o)
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def iron_lit(s):
    """Iron source for the string s (zero bytes through String.from_byte)."""
    parts = s.split("\0")
    lits = []
    for p in parts:
        out = []
        for ch in p:
            o = ord(ch)
            if ch == "\\":
                out.append("\\\\")
            elif ch == '"':
                out.append('\\"')
            elif ch == "{":
                out.append("\\{")
            elif ch == "}":
                out.append("\\}")
            elif ch == "\n":
                out.append("\\n")
            elif ch == "\t":
                out.append("\\t")
            elif o < 0x20 or o == 0x7f or o > 0xffff:
                out.append("\\u{%x}" % o)
            else:
                out.append(ch)
        lits.append('"' + "".join(out) + '"')
    if len(lits) == 1:
        return lits[0]
    return "(" + ' + String.from_byte(0) + '.join(lits) + ")"


# ── Types ────────────────────────────────────────────────────────────────────

INT_TYPES = {"Int": (64, True), "Int8": (8, True), "Int16": (16, True), "Int32": (32, True),
             "Int64": (64, True), "UInt": (64, False), "UInt8": (8, False),
             "UInt16": (16, False), "UInt32": (32, False), "UInt64": (64, False)}


def wrap(v, bits, signed):
    v &= (1 << bits) - 1
    if signed and v >> (bits - 1):
        v -= 1 << bits
    return v


class T:
    """An Iron type: kind plus parameters."""

    def __init__(self, kind, *args, name=None):
        self.kind, self.args, self.name = kind, args, name

    def src(self):
        k = self.kind
        if k in ("int", "float", "bool", "str"):
            return self.name
        if k == "list":
            return "[%s]" % self.args[0].src()
        if k == "map":
            return "Map[%s, %s]" % (self.args[0].src(), self.args[1].src())
        if k == "set":
            return "Set[%s]" % self.args[0].src()
        if k == "opt":
            return self.args[0].src() + "?"
        if k == "rc":
            return "rc " + self.args[0].src()
        if k == "weak":
            return "weak rc " + self.args[0].src()
        return self.name     # enum, obj, iface, generic instance

    def __eq__(self, o):
        return isinstance(o, T) and self.src() == o.src()

    def __hash__(self):
        return hash(self.src())


# ── Values ───────────────────────────────────────────────────────────────────

class V:
    """A modelled value. src(): Iron expression that builds it; disp(): what
    the debugger shows as its value; kids(): [(child name, V)] or None
    when its children are not checked."""

    def kids(self):
        return None


class VInt(V):
    def __init__(self, t, v):
        self.t, self.v = t, v

    def src(self):
        name = self.t.name if self.t else "Int"
        if name == "Int":
            return str(self.v)
        if self.v > (1 << 62):
            parts, v = [], self.v
            while v > 0:
                parts.append(min(v, 1 << 62))
                v -= parts[-1]
            return "(%s)" % " + ".join("%s(%d)" % (name, x) for x in parts)
        return "%s(%d)" % (name, self.v)

    def disp(self):
        return str(self.v)


class VFloat(V):
    def __init__(self, t, v):
        self.t, self.v = t, v

    def src(self):
        if self.t.name == "Float32":
            return "Float32(%s)" % float_lit(self.v, True)
        return float_lit(self.v)

    def disp(self):
        return float_text(self.v, self.t.name == "Float32")


class VBool(V):
    def __init__(self, v):
        self.t, self.v = T("bool", name="Bool"), v

    def src(self):
        return "true" if self.v else "false"

    def disp(self):
        return "true" if self.v else "false"


class VStr(V):
    def __init__(self, v):
        self.t, self.v = T("str", name="String"), v

    def src(self):
        if len(self.v) > 60 and len(set(self.v)) == 1:
            return '%s.repeat(%d)' % (iron_lit(self.v[0]), len(self.v))
        return iron_lit(self.v)

    def disp(self):
        return str_display(self.v)


def list_text(items):
    shown = [x.disp() for x in items[:20]]
    return "[%s%s]" % (", ".join(shown), ", ..." if len(items) > 20 else "")


class VList(V):
    def __init__(self, t, items):
        self.t, self.items = t, items

    def src(self):
        if not self.items:
            return "[]"
        return "[%s]" % ", ".join(x.src() for x in self.items)

    def disp(self):
        return list_text(self.items)

    def kids(self):
        return [("[%d]" % i, x) for i, x in enumerate(self.items)]


class VMap(V):
    def __init__(self, t, d):
        self.t, self.d = t, d     # list of (key V, value V) in insertion order

    def disp(self):
        return ("size", len(self.d))

    def kids(self):
        return ("map", self.d)


class VSet(V):
    def __init__(self, t, items):
        self.t, self.items = t, items

    def disp(self):
        return ("size", len(self.items))

    def kids(self):
        return ("set", self.items)


class VOpt(V):
    def __init__(self, t, inner):
        self.t, self.inner = t, inner

    def src(self):
        return "null" if self.inner is None else self.inner.src()

    def disp(self):
        return "null" if self.inner is None else self.inner.disp()


class VEnum(V):
    def __init__(self, t, variant, payload):
        self.t, self.variant, self.payload = t, variant, payload

    def src(self):
        if not self.payload:
            return "%s.%s" % (self.t.name, self.variant)
        return "%s.%s(%s)" % (self.t.name, self.variant, ", ".join(p.src() for p in self.payload))

    def disp(self):
        if not self.payload:
            return self.variant
        return "%s(%s)" % (self.variant, ", ".join(p.disp() for p in self.payload))

    def kids(self):
        return [("_%d" % i, p) for i, p in enumerate(self.payload)] if self.payload else None


def obj_text(name, fields):
    parts = ["%s = %s" % (k, v.disp()) for k, v in fields[:8]]
    return "%s {%s%s}" % (name, ", ".join(parts), ", ..." if len(fields) > 8 else "")


class VObj(V):
    def __init__(self, t, fields, shown_name=None):
        self.t, self.fields = t, fields     # [(name, V)]
        self.shown = shown_name or t.name

    def src(self):
        return "%s(%s)" % (self.t.name, ", ".join(v.src() for _, v in self.fields))

    def disp(self):
        return obj_text(self.shown, self.fields)

    def kids(self):
        return list(self.fields)


class RcBox:
    """The shared object behind rc handles, with its counts."""

    def __init__(self, obj):
        self.obj, self.strong, self.weak = obj, 0, 0


class VRc(V):
    def __init__(self, t, box):
        self.t, self.box = t, box

    def disp(self):
        return "rc %s (strong=%d, weak=%d)" % (self.box.obj.disp(), self.box.strong, self.box.weak)

    def kids(self):
        return self.box.obj.kids()


class VIface(V):
    def __init__(self, t, obj):
        self.t, self.obj = t, obj

    def disp(self):
        return self.obj.disp()

    def kids(self):
        return self.obj.kids()


class VClosure(V):
    """disp is checked as a pattern; captures as children."""

    def __init__(self, t, line, caps):
        self.t, self.line, self.caps = t, line, caps

    def disp(self):
        return ("closure", self.line)

    def kids(self):
        return list(self.caps)


# ── The program generator ────────────────────────────────────────────────────

WORDS = ["", "a", "ann", "héllo", "wörld ✓", "tab\there", "line\nbreak", 'say "hi"', "back\\slash",
         "zero\0byte", "{braces}", "日本語", "x" * 30, "emoji 🎉", " ", "\0", "end\x01ctl"]


class Binding:
    def __init__(self, name, value, mutable=False, kind="local"):
        self.name, self.value, self.mutable, self.kind = name, value, mutable, kind


class Probe:
    """A probe line: the bindings in scope and their values at the stop
    (one snapshot per hit), and expressions checked by evaluate."""

    def __init__(self, pid, func):
        self.pid, self.func = pid, func
        self.line = None
        self.hits = []        # [ {name: V} ] per hit
        self.exprs = []       # [(iron text, kind)] printed on the probe line
        self.errors = []      # [(iron text, wanted error substring)]


class Gen:
    def __init__(self, rng, features=None):
        self.r = rng
        self.n = 0
        self.tops = []         # top-level declarations (lines)
        self.globals = []      # Binding
        self.funcs = []        # [(name, lines)]
        self.main_lines = []
        self.probes = []
        self.objs = []         # (T, [(field, T)])
        self.enums = []        # (T, [(variant, [T])])
        self.ifaces = []
        self.generics = {}     # instance name -> T
        self.need_json = False
        self.lambda_lines = []

    def fresh(self, p="v"):
        self.n += 1
        return "%s%d" % (p, self.n)

    # Random scalars
    def int_val(self, tname):
        bits, signed = INT_TYPES[tname]
        r = self.r
        lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
        c = r.random()
        if c < 0.3:
            v = r.choice([lo, hi, 0, 1, -1 if signed else 2, lo + 1, hi - 1])
        elif c < 0.7:
            v = r.randint(-100 if signed else 0, 100)
        else:
            v = r.randint(lo, hi)
        if tname in ("Int", "Int64") and v == lo:
            v = lo + 1     # -9223372036854775808 has no literal form
        return VInt(T("int", name=tname), max(lo, min(hi, v)))

    def float_val(self, tname):
        r = self.r
        v = r.choice([0.0, 1.0, 0.5, 0.1, 2.5, 1234.5, 0.001, 100000.0, 3.14159, 1e-7,
                      123456789.125, 1e21, 0.30000000000000004])
        if r.random() < 0.3:
            v = round(r.uniform(-1000, 1000), r.randint(0, 6))
        if r.random() < 0.3:
            v = -v + 0.0   # not -0.0: its literal is written 0.0
        if tname == "Float32":
            v = f32(v)
        return VFloat(T("float", name=tname), v)

    def str_val(self):
        r = self.r
        c = r.random()
        if c < 0.6:
            s = r.choice(WORDS)
        elif c < 0.8:
            s = r.choice(WORDS) + r.choice(WORDS)
        else:
            s = r.choice("abc") * r.randint(50, 300)
        return VStr(s)

    # Declarations of object / enum / interface types
    def obj_type(self, depth=0):
        if self.objs and (self.r.random() < 0.5 or len(self.objs) > 4):
            return self.r.choice(self.objs)[0]
        fields = []
        for i in range(self.r.randint(1, 4)):
            ft = self.leaf_type(allow_obj=depth < 1, depth=depth + 1)
            fields.append(("f%d" % i, ft))
        name = "Obj%d" % (len(self.objs) + 1)
        t = T("obj", name=name)
        self.objs.append((t, fields))
        self.tops.append(["object %s {" % name] + ["    val %s: %s" % (f, ft.src()) for f, ft in fields]
                         + ["}"])
        return t

    def enum_type(self):
        if self.enums and self.r.random() < 0.5:
            return self.r.choice(self.enums)[0]
        name = "En%d" % (len(self.enums) + 1)
        variants = []
        payload = self.r.random() < 0.7
        for i in range(self.r.randint(1, 4)):
            ps = [self.scalar_type() for _ in range(self.r.randint(0, 2))] if payload else []
            variants.append(("Va%d" % i, ps))
        t = T("enum", name=name)
        t.variants = variants
        self.enums.append((t, variants))
        lines = ["enum %s {" % name]
        for v, ps in variants:
            lines.append("    %s%s," % (v, "(%s)" % ", ".join(p.src() for p in ps) if ps else ""))
        self.tops.append(lines + ["}"])
        return t

    def scalar_type(self):
        r = self.r.random()
        if r < 0.45:
            return T("int", name=self.r.choice(list(INT_TYPES)))
        if r < 0.6:
            return T("float", name=self.r.choice(["Float", "Float32"]))
        if r < 0.7:
            return T("bool", name="Bool")
        return T("str", name="String")

    def leaf_type(self, allow_obj=True, depth=0):
        r = self.r.random()
        if r < 0.6 or depth > 2:
            return self.scalar_type()
        if r < 0.75:
            return T("list", self.scalar_type())
        if r < 0.85:
            return T("opt", self.scalar_type())
        if r < 0.92 or not allow_obj:
            return self.enum_type()
        return self.obj_type(depth)

    def value_of(self, t, depth=0):
        k = t.kind
        if k == "int":
            return self.int_val(t.name)
        if k == "float":
            return self.float_val(t.name)
        if k == "bool":
            return VBool(self.r.random() < 0.5)
        if k == "str":
            return self.str_val()
        if k == "list":
            n = self.r.choice([0, 1, 2, 3, 5, 21, 25]) if depth == 0 else self.r.randint(0, 3)
            return VList(t, [self.value_of(t.args[0], depth + 1) for _ in range(n)])
        if k == "opt":
            if self.r.random() < 0.35:
                return VOpt(t, None)
            inner = self.value_of(t.args[0], depth + 1)
            while isinstance(inner, VStr) and ("\0" in inner.v or len(inner.v) > 60):
                inner = self.value_of(t.args[0], depth + 1)   # (#B2: no concat into a T?)
            return VOpt(t, inner)
        if k == "enum":
            variants = dict(self.enum_variants(t))
            v = self.r.choice(list(variants))
            return VEnum(t, v, [self.value_of(p, depth + 1) for p in variants[v]])
        if k == "obj":
            fields = dict((x.name, f) for x, f in self.objs)[t.name]
            return VObj(t, [(f, self.value_of(ft, depth + 1)) for f, ft in fields])
        raise ValueError(k)

    def enum_variants(self, t):
        for et, vs in self.enums:
            if et.name == t.name:
                return vs
        raise KeyError(t.name)


# ── Statements: Iron lines plus their effect on the modelled environment ────

class Env:
    """name -> V for the bindings in scope, in declaration order."""

    def __init__(self, parent=None):
        self.vars = {}
        self.parent = parent

    def all(self):
        out = dict(self.parent.all()) if self.parent else {}
        out.update(self.vars)
        return out

    def get(self, n):
        e = self
        while e is not None:
            if n in e.vars:
                return e.vars[n]
            e = e.parent
        raise KeyError(n)

    def set(self, n, v):
        e = self
        while e is not None:
            if n in e.vars:
                e.vars[n] = v
                return
            e = e.parent
        raise KeyError(n)


class St:
    def __init__(self, lines, apply=None):
        self.lines, self.fn = lines, apply

    def emit(self, ind):
        return [ind + l for l in self.lines]

    def apply(self, env, ctx):
        if self.fn:
            self.fn(env, ctx)


class Loop(St):
    def __init__(self, var, count, body):
        self.var, self.count, self.body = var, count, body

    def emit(self, ind):
        out = [ind + "for %s in range(%d) {" % (self.var, self.count)]
        for s in self.body:
            out += s.emit(ind + "    ")
        return out + [ind + "}"]

    def apply(self, env, ctx):
        for i in range(self.count):
            inner = Env(env)
            inner.vars[self.var] = VInt(T("int", name="Int"), i)
            for s in self.body:
                s.apply(inner, ctx)


def snapshot(env):
    """The display of every binding (taken at the stop: rc counts change)."""
    return {n: shot(v) for n, v in env.all().items()}


def shot(v):
    d = v.disp()
    k = v.kids()
    if isinstance(k, tuple):           # map / set
        kind, items = k
        if kind == "map":
            k = ("map", [(key.disp(), shot(val)) for key, val in items])
        else:
            k = ("set", sorted(x.disp() for x in items))
    elif k is not None:
        k = [(n, shot(c)) for n, c in k]
    return (d, k)


class ProbeSt(St):
    def __init__(self, probe, exprs):
        self.probe, self.exprs = probe, exprs

    def emit(self, ind):
        # One line: the breakpoint stops before it runs, so the bindings
        # still hold the values the expressions print.
        parts = "".join("@E%d<{%s}>@" % (i, e.src) for i, e in enumerate(self.exprs))
        return [ind + 'println("@P%d %s")' % (self.probe.pid, parts)]

    def apply(self, env, ctx):
        self.probe.hits.append(snapshot(env))
        vals = []
        for e in self.exprs:
            vals.append(e.value(env))
        self.probe.expr_hits.append(vals)


# ── Expressions with a model (for the evaluate campaign) ───────────────────

class EvalErr(Exception):
    pass


class Ex:
    """src: Iron text; t: T; ev(env) -> python value or raises EvalErr."""

    def __init__(self, src, t, ev, literal=False):
        self.src, self.t, self.ev, self.literal = src, t, ev, literal

    def value(self, env):
        try:
            return ("ok", self.ev(env))
        except EvalErr as e:
            return ("err", str(e))

    def text(self, v):
        """The text the program prints for a value of this expression."""
        k = self.t.kind
        if k == "bool":
            return "true" if v else "false"
        if k == "float":
            return float_text(v, self.t.name == "Float32")
        if k == "opt":
            return "null" if v is None else Ex(self.src, self.t.args[0], None).text(v)
        return str(v)

    def disp(self, v):
        """What evaluate should answer for a value of this expression."""
        if self.t.kind == "str":
            return str_display(v)
        if self.t.kind == "opt" and v is not None:
            return Ex(self.src, self.t.args[0], None).disp(v)
        return self.text(v)


def tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


class ExprGen:
    """Random typed Iron expressions over the bindings in scope."""

    def __init__(self, rng, scope):
        self.r = rng
        self.scope = scope       # name -> V (values at generation time: types only matter)

    def of_kind(self, pred):
        return [(n, v) for n, v in self.scope.items() if pred(v)]

    def int_expr(self, tname, depth=0):
        r = self.r
        bits, signed = INT_TYPES[tname]
        vars_ = self.of_kind(lambda v: isinstance(v, VInt) and v.t.name == tname)
        t = T("int", name=tname)
        if depth > 2 or r.random() < 0.3:
            if vars_ and r.random() < 0.8:
                n, _ = r.choice(vars_)
                return Ex(n, t, lambda env, n=n: env.get(n).v)
            if tname == "Int":
                # Int from other places: lengths, list elements, fields
                alt = self.int_from_collections()
                if alt is not None:
                    return alt
            lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
            c = r.choice([0, 1, 2, 3, 7, 10, 100, hi, lo + 1 if signed else 255])
            c = max(lo, min(hi, c))
            return Ex(VInt(t, c).src() if tname != "Int" or c >= 0 else "(%d)" % c, t,
                      lambda env, c=c: c, literal=True)
        a = self.int_expr(tname, depth + 1)
        b = self.int_expr(tname, depth + 1)
        if a.literal and b.literal:
            b = self.int_expr(tname, 3)
            if b.literal and vars_:
                n, _ = r.choice(vars_)
                b = Ex(n, t, lambda env, n=n: env.get(n).v)
            elif b.literal:
                return a
        op = r.choice(["+", "-", "*", "/", "%"])

        def ev(env, a=a, b=b, op=op):
            x, y = a.ev(env), b.ev(env)
            if op in "/%":
                if y == 0:
                    raise EvalErr("integer division by zero")
                q = tdiv(x, y)
                v = q if op == "/" else x - q * y
            else:
                v = {"+": x + y, "-": x - y, "*": x * y}[op]
            return wrap(v, bits, signed)
        return Ex("(%s %s %s)" % (a.src, op, b.src), t, ev)

    def int_from_collections(self):
        r = self.r
        cands = []
        for n, v in self.scope.items():
            if isinstance(v, VList):
                cands.append(Ex("%s.len()" % n, T("int", name="Int"),
                                lambda env, n=n: len(env.get(n).items)))
                if v.t.args[0].kind == "int" and v.t.args[0].name == "Int" and v.items:
                    i = r.randint(0, len(v.items) - 1)
                    cands.append(Ex("%s[%d]" % (n, i), T("int", name="Int"),
                                    lambda env, n=n, i=i: idx(env.get(n).items, i).v))
            if isinstance(v, VStr):
                cands.append(Ex("%s.len()" % n, T("int", name="Int"),
                                lambda env, n=n: len(env.get(n).v)))
            if isinstance(v, (VMap, VSet)):
                attr = "d" if isinstance(v, VMap) else "items"
                cands.append(Ex("%s.len()" % n, T("int", name="Int"),
                                lambda env, n=n, attr=attr: len(getattr(env.get(n), attr))))
            if isinstance(v, VObj):
                for f, fv in v.fields:
                    if isinstance(fv, VInt) and fv.t.name == "Int":
                        cands.append(Ex("%s.%s" % (n, f), T("int", name="Int"),
                                        lambda env, n=n, f=f: dict(env.get(n).fields)[f].v))
            if isinstance(v, VMap) and v.t.args[1].kind == "int" and v.t.args[1].name == "Int" \
                    and v.d:
                key = r.choice(v.d)[0]
                cands.append(Ex("%s.get(%s)" % (n, key.src()), T("int", name="Int"),
                                lambda env, n=n, k=key: mapget(env.get(n), k).v))
                cands.append(Ex("%s.get_or(%s, %d)" % (n, key.src(), -1), T("int", name="Int"),
                                lambda env, n=n, k=key: mapget(env.get(n), k, VInt(None, -1)).v))
        return r.choice(cands) if cands else None

    def float_expr(self, tname, depth=0):
        r = self.r
        t = T("float", name=tname)
        is32 = tname == "Float32"
        rnd = f32 if is32 else (lambda x: x)
        vars_ = self.of_kind(lambda v: isinstance(v, VFloat) and v.t.name == tname)
        if depth > 2 or r.random() < 0.3:
            if vars_ and r.random() < 0.8:
                n, _ = r.choice(vars_)
                return Ex(n, t, lambda env, n=n: env.get(n).v)
            c = r.choice([0.0, 0.5, 1.0, 2.0, 0.1, 3.25, 1000.0])
            return Ex(VFloat(t, rnd(c)).src(), t, lambda env, c=c: rnd(c), literal=True)
        a, b = self.float_expr(tname, depth + 1), self.float_expr(tname, depth + 1)
        if a.literal and b.literal:
            if not vars_:
                return a
            n, _ = r.choice(vars_)
            b = Ex(n, t, lambda env, n=n: env.get(n).v)
        op = r.choice(["+", "-", "*", "/", "%"])

        def ev(env, a=a, b=b, op=op):
            x, y = a.ev(env), b.ev(env)
            try:
                if op == "/":
                    v = x / y if y != 0 else (math.copysign(math.inf, x) * math.copysign(1, y)
                                              if x != 0 and not math.isnan(x) else math.nan)
                elif op == "%":
                    v = math.fmod(x, y) if y != 0 and not math.isinf(x) else math.nan
                else:
                    v = {"+": x + y, "-": x - y, "*": x * y}[op]
            except OverflowError:
                v = math.inf
            return rnd(v)
        return Ex("(%s %s %s)" % (a.src, op, b.src), t, ev)

    def str_expr(self, depth=0):
        r = self.r
        t = T("str", name="String")
        vars_ = self.of_kind(lambda v: isinstance(v, VStr))
        c = r.random()
        if depth > 1 or c < 0.35:
            if vars_ and r.random() < 0.8:
                n, _ = r.choice(vars_)
                return Ex(n, t, lambda env, n=n: env.get(n).v)
            s = r.choice(WORDS[:12])
            return Ex(iron_lit(s), t, lambda env, s=s: s, literal=True)
        if c < 0.7 or not vars_:
            a, b = self.str_expr(depth + 1), self.str_expr(depth + 1)
            return Ex("(%s + %s)" % (a.src, b.src), t, lambda env, a=a, b=b: a.ev(env) + b.ev(env))
        n, v = r.choice(vars_)
        i = r.randint(-1, len(v.v) + 1)
        src_i = str(i) if i >= 0 else "(%d)" % i
        return Ex("%s[%s]" % (n, src_i), t,
                  lambda env, n=n, i=i: env.get(n).v[i] if 0 <= i < len(env.get(n).v) else "")

    def bool_expr(self, depth=0):
        r = self.r
        t = T("bool", name="Bool")
        c = r.random()
        bools = self.of_kind(lambda v: isinstance(v, VBool))
        if depth > 1 or c < 0.2:
            if bools and r.random() < 0.7:
                n, _ = r.choice(bools)
                return Ex(n, t, lambda env, n=n: env.get(n).v)
            return Ex(r.choice(["true", "false"]), t, None, literal=True) \
                if False else self.compare()
        if c < 0.45:
            a, b = self.bool_expr(depth + 1), self.bool_expr(depth + 1)
            op = r.choice(["and", "or"])
            if op == "and":
                return Ex("(%s and %s)" % (a.src, b.src), t,
                          lambda env, a=a, b=b: a.ev(env) and b.ev(env))
            return Ex("(%s or %s)" % (a.src, b.src), t, lambda env, a=a, b=b: a.ev(env) or b.ev(env))
        if c < 0.55:
            a = self.bool_expr(depth + 1)
            return Ex("(not %s)" % a.src, t, lambda env, a=a: not a.ev(env))
        return self.compare()

    def compare(self):
        r = self.r
        t = T("bool", name="Bool")
        ops = {"==": lambda x, y: x == y, "!=": lambda x, y: x != y, "<": lambda x, y: x < y,
               ">": lambda x, y: x > y, "<=": lambda x, y: x <= y, ">=": lambda x, y: x >= y}
        kinds = []
        ints = sorted({v.t.name for v in self.scope.values() if isinstance(v, VInt)})
        floats = sorted({v.t.name for v in self.scope.values() if isinstance(v, VFloat)})
        kinds += [("int", n) for n in ints] + [("int", "Int")]
        kinds += [("float", n) for n in floats]
        if self.of_kind(lambda v: isinstance(v, VStr)):
            kinds.append(("str", None))
        extra = self.special_bools()
        if extra and r.random() < 0.4:
            return r.choice(extra)
        k, n = r.choice(kinds)
        if k == "int":
            a, b = self.int_expr(n, 1), self.int_expr(n, 1)
        elif k == "float":
            a, b = self.float_expr(n, 1), self.float_expr(n, 1)
        else:
            a, b = self.str_expr(1), self.str_expr(1)
        op = r.choice(list(ops))
        if a.literal and b.literal:
            return Ex("true", t, lambda env: True, literal=True)
        return Ex("(%s %s %s)" % (a.src, op, b.src), t,
                  lambda env, a=a, b=b, op=op: ops[op](a.ev(env), b.ev(env)))

    def special_bools(self):
        r = self.r
        t = T("bool", name="Bool")
        out = []
        for n, v in self.scope.items():
            if isinstance(v, VOpt):
                out.append(Ex("(%s == null)" % n, t, lambda env, n=n: env.get(n).inner is None))
                out.append(Ex("(%s != null)" % n, t, lambda env, n=n: env.get(n).inner is not None))
            if isinstance(v, VList):
                out.append(Ex("(%s.len() == 0)" % n, t, lambda env, n=n: not env.get(n).items))
                if v.items and v.t.args[0].kind in ("int", "str"):
                    probe = r.choice(v.items)
                    if r.random() < 0.3:
                        probe = self_value_like(probe, r)
                    out.append(Ex("%s.contains(%s)" % (n, probe.src()), t,
                                  lambda env, n=n, p=probe: any(same(x, p) for x in env.get(n).items)))
            if isinstance(v, VStr):
                sub = r.choice(["", "a", "é", "x", v.v[:2], v.v[-1:]])
                m = r.choice(["contains", "starts_with", "ends_with"])
                f = {"contains": lambda s, x: x in s, "starts_with": lambda s, x: s.startswith(x),
                     "ends_with": lambda s, x: s.endswith(x)}[m]
                out.append(Ex("%s.%s(%s)" % (n, m, iron_lit(sub)), t,
                              lambda env, n=n, f=f, sub=sub: f(env.get(n).v, sub)))
            if isinstance(v, (VMap, VSet)):
                items = [k for k, _ in v.d] if isinstance(v, VMap) else v.items
                kt = v.t.args[0]
                if items and r.random() < 0.6:
                    key = r.choice(items)
                else:
                    key = Gen(r).value_of(kt)
                if isinstance(v, VMap):
                    out.append(Ex("%s.has(%s)" % (n, key.src()), t,
                                  lambda env, n=n, k=key: any(same(x, k) for x, _ in env.get(n).d)))
                    # a value is not a key: has() looks at keys only
                    if v.d and v.t.args[1] == kt:
                        val = r.choice(v.d)[1]
                        out.append(Ex("%s.has(%s)" % (n, val.src()), t,
                                      lambda env, n=n, k=val: any(same(x, k) for x, _ in env.get(n).d)))
                else:
                    out.append(Ex("%s.has(%s)" % (n, key.src()), t,
                                  lambda env, n=n, k=key: any(same(x, k) for x in env.get(n).items)))
            if isinstance(v, VEnum):
                units = [vn for vn, ps in self_enum_variants(v) if not ps]
                if units and all(p.kind in ("int", "float", "bool", "str")
                                 for _, ps in self_enum_variants(v) for p in ps):
                    u = r.choice(units)
                    out.append(Ex("(%s == %s.%s)" % (n, v.t.name, u), t,
                                  lambda env, n=n, u=u: env.get(n).variant == u))
        return out

    def any_expr(self):
        r = self.r
        c = r.random()
        ints = sorted({v.t.name for v in self.scope.values() if isinstance(v, VInt)}) or ["Int"]
        floats = sorted({v.t.name for v in self.scope.values() if isinstance(v, VFloat)})
        if c < 0.35:
            return self.int_expr(r.choice(ints))
        if c < 0.5 and floats:
            return self.float_expr(r.choice(floats))
        if c < 0.65:
            return self.str_expr()
        if c < 0.75:
            alt = self.int_from_collections()
            if alt is not None:
                return alt
        return self.bool_expr()

    def error_exprs(self):
        """Expressions meant to fail at the stop (Ex whose model raises);
        value_program keeps those that fail at every hit, with the error."""
        out = []
        r = self.r
        t_int = T("int", name="Int")
        ints = self.of_kind(lambda v: isinstance(v, VInt))
        if ints:
            n, v = r.choice(ints)

            def div0(env):
                raise EvalErr("integer division by zero")
            out.append(Ex("%s / 0" % n, v.t, div0))
            out.append(Ex("%s %% (%s - %s)" % (n, n, n), v.t, div0))
        for n, v in self.scope.items():
            if isinstance(v, VList) and v.t.args[0].kind in ("int", "str", "bool"):
                i = len(v.items) + r.randint(0, 3)
                out.append(Ex("%s[%d]" % (n, i), v.t.args[0],
                              lambda env, n=n, i=i: idx(env.get(n).items, i).v))
            if isinstance(v, VMap) and v.t.args[0].kind == "str" and \
                    v.t.args[1].kind in ("int", "str", "bool"):
                missing = VStr("no such key \u2603")
                out.append(Ex("%s.get(%s)" % (n, missing.src()), v.t.args[1],
                              lambda env, n=n, k=missing: mapget(env.get(n), k).v))
        return out


def idx(items, i):
    if not 0 <= i < len(items):
        raise EvalErr("index out of bounds (index %d, bound %d)" % (i, len(items)))
    return items[i]


def same(a, b):
    return type(a) is type(b) and a.disp() == b.disp()


def mapget(m, k, default=None):
    for key, val in m.d:
        if same(key, k):
            return val
    if default is not None:
        return default
    raise EvalErr("key not found in map")


def self_value_like(v, r):
    if isinstance(v, VInt):
        return VInt(v.t, max(0, min(v.v + 1, (1 << 7) - 1)) if not INT_TYPES[v.t.name][1]
                    else wrap(v.v + 1, INT_TYPES[v.t.name][0], True))
    if isinstance(v, VStr):
        return VStr(v.v + "?")
    return v


_ENUMS = {}


def self_enum_variants(v):
    return v.t.variants


# ── Whole programs ───────────────────────────────────────────────────────────

class ValueProgram(Gen):
    """Functions with typed bindings and probes (values and eval areas)."""

    def __init__(self, rng, nexprs=0):
        Gen.__init__(self, rng)
        self.nexprs = nexprs
        self.body_of = {}         # function name -> (params, [St])
        self.calls = []           # (function name, [arg V]) in main, in order
        self.iface_made = False
        self.extra_names = {}     # function -> bindings no probe sees

    # bindings of every kind: (lines, apply, value-for-typing)
    def decl(self, env_types, mutable_ok=True):
        r = self.r
        name = self.fresh(r.choice(["x", "n", "s", "v", "item", "long", "count", "p"]))
        c = r.random()
        if c < 0.45:
            t = self.leaf_type()
            v = self.value_of(t)
            mut = mutable_ok and r.random() < 0.3 and t.kind in ("int", "float", "str", "list", "bool")
            kw = "var" if mut else "val"
            if t.kind == "list" and not mut and r.random() < 0.5:
                kw = "val"
            lines = ["%s %s: %s = %s" % (kw, name, t.src(), v.src())]
            return lines, (lambda env, ctx, v=v: env.vars.__setitem__(name, v)), (name, v, mut)
        if c < 0.55:
            kt = r.choice([T("int", name="Int"), T("str", name="String"), T("int", name="Int32")])
            vt = self.leaf_type(allow_obj=False)
            pairs = [(self.value_of(kt), self.value_of(vt)) for _ in range(r.choice([0, 1, 2, 3, 6]))]
            lines = ["var %s = Map[%s, %s]()" % (name, kt.src(), vt.src())]
            lines += ["%s.put(%s, %s)" % (name, k.src(), v.src()) for k, v in pairs]

            def ap(env, ctx, pairs=pairs, t=T("map", kt, vt)):
                d = []
                for k, v in pairs:
                    d = [(a, b) for a, b in d if not same(a, k)] + [(k, v)]
                env.vars[name] = VMap(t, d)
            return lines, ap, (name, VMap(T("map", kt, vt), [(k, v) for k, v in pairs]), False)
        if c < 0.62:
            kt = r.choice([T("int", name="Int"), T("str", name="String"), T("int", name="UInt8")])
            items = [self.value_of(kt) for _ in range(r.choice([0, 1, 2, 4]))]
            lines = ["var %s = Set[%s]()" % (name, kt.src())]
            lines += ["%s.add(%s)" % (name, x.src()) for x in items]

            def ap(env, ctx, items=items, t=T("set", kt)):
                s = []
                for x in items:
                    if not any(same(x, y) for y in s):
                        s.append(x)
                env.vars[name] = VSet(t, s)
            return lines, ap, (name, VSet(T("set", kt), items), False)
        if c < 0.72:
            t = self.obj_type()
            v = self.value_of(t)
            return (["val %s = %s" % (name, v.src())],
                    lambda env, ctx, v=v: env.vars.__setitem__(name, v), (name, v, False))
        if c < 0.8:
            t = self.obj_type()
            obj = self.value_of(t)
            n2, nw = self.fresh("r"), self.fresh("w")
            copies = r.randint(0, 2)
            weak = r.random() < 0.5
            lines = ["val %s = rc %s" % (name, obj.src())]
            for i in range(copies):
                lines.append("val %s_%d = %s" % (n2, i, name))
            if weak:
                lines.append("val %s = %s.downgrade()" % (nw, name))

            def ap(env, ctx, obj=obj, copies=copies, weak=weak, t=T("rc", t)):
                box = RcBox(obj)
                box.strong = 1 + copies
                box.weak = 1 if weak else 0
                env.vars[name] = VRc(t, box)
                for i in range(copies):
                    env.vars["%s_%d" % (n2, i)] = VRc(t, box)
                if weak:
                    env.vars[nw] = VRc(T("weak", t.args[0]), box)
            return lines, ap, (name, VRc(T("rc", t), RcBox(obj)), False)
        if c < 0.83:
            # a heap value, or a checked pointer to a local object
            t = self.obj_type()
            v = self.value_of(t)
            if r.random() < 0.5:
                return (["val %s = heap %s" % (name, v.src()), "leak %s" % name],
                        lambda env, ctx, v=v: env.vars.__setitem__(name, v), (name, v, False))
            o = self.fresh("o")

            def ap(env, ctx, v=v, o=o):
                env.vars[o] = v
                env.vars[name] = v
            return (["var %s = %s" % (o, v.src()), "val %s: *%s = &%s" % (name, t.name, o)],
                    ap, (name, v, False))
        if c < 0.86:
            # an interface value
            t, impl = self.iface_type()
            v = self.value_of(impl)
            return (["val %s: %s = %s" % (name, t.name, v.src())],
                    lambda env, ctx, v=v, t=t: env.vars.__setitem__(name, VIface(t, v)),
                    (name, VIface(t, v), False))
        if c < 0.92:
            # a generic instance
            arg = self.scalar_type()
            if "Gen" not in [x[0] for x in self.tops if x]:
                pass
            self.need_generic()
            inner = self.value_of(arg)
            t = T("obj", name="Gbox[%s]" % arg.src())
            v = VObj(t, [("item", inner)], shown_name="Gbox[%s]" % arg.src())
            return (["val %s = Gbox[%s](%s)" % (name, arg.src(), inner.src())],
                    lambda env, ctx, v=v: env.vars.__setitem__(name, v), (name, v, False))
        # a tuple taken apart
        t1, t2 = self.scalar_type(), self.scalar_type()
        a, b = self.value_of(t1), self.value_of(t2)
        n1, n2 = name, self.fresh("t")

        def ap(env, ctx, a=a, b=b):
            env.vars[n1] = a
            env.vars[n2] = b
        return (["val (%s, %s) = pair_%s_%s(%s, %s)" % (n1, n2, self.tname(t1), self.tname(t2),
                                                      a.src(), b.src())]
                + self.need_pair(t1, t2), ap, (n1, a, False))

    def tname(self, t):
        return t.src().replace("[", "_").replace("]", "_").replace("?", "_opt")

    def need_pair(self, t1, t2):
        key = "pair_%s_%s" % (self.tname(t1), self.tname(t2))
        if not any(l and l[0].startswith("func %s(" % key) for l in self.tops):
            self.tops.append(["func %s(a: %s, b: %s) -> (%s, %s) {" % (key, t1.src(), t2.src(),
                                                                     t1.src(), t2.src()),
                              "    return (a, b)", "}"])
        return []

    def need_generic(self):
        if not any(l and l[0] == "object Gbox[T] {" for l in self.tops):
            self.tops.append(["object Gbox[T] {", "    val item: T", "}"])

    def iface_type(self):
        if not self.iface_made:
            self.iface_made = True
            self.tops.append(["interface Named {", "    readonly func name() -> String", "}"])
            for k in range(2):
                fields = [("f%d" % i, self.scalar_type()) for i in range(self.r.randint(1, 3))]
                tn = "Impl%d" % k
                self.objs.append((T("obj", name=tn), fields))
                self.tops.append(["object %s impl Named {" % tn] +
                                 ["    val %s: %s" % (f, ft.src()) for f, ft in fields] +
                                 ["    readonly func name() -> String {",
                                  '        return "%s"' % tn, "    }", "}"])
        impl = T("obj", name="Impl%d" % self.r.randint(0, 1))
        return T("iface", name="Named"), impl

    def mutation(self, name, v):
        """A change to a var binding: (lines, apply)."""
        r = self.r
        if isinstance(v, VInt):
            bits, signed = INT_TYPES[v.t.name]
            c = r.randint(1, 100)
            op = r.choice(["+", "*", "-"])

            def ap(env, ctx):
                x = env.get(name).v
                y = {"+": x + c, "*": x * c, "-": x - c}[op]
                env.set(name, VInt(v.t, wrap(y, bits, signed)))
            return ["%s = %s %s %d" % (name, name, op, c)], ap
        if isinstance(v, VFloat):
            def ap(env, ctx):
                x = env.get(name).v * 2.0 + 0.5
                env.set(name, VFloat(v.t, f32(x) if v.t.name == "Float32" else x))
            return ["%s = %s * 2.0 + 0.5" % (name, name)], ap
        if isinstance(v, VBool):
            return (["%s = not %s" % (name, name)],
                    lambda env, ctx: env.set(name, VBool(not env.get(name).v)))
        if isinstance(v, VStr):
            add = r.choice(WORDS)
            return (["%s = %s + %s" % (name, name, iron_lit(add))],
                    lambda env, ctx: env.set(name, VStr(env.get(name).v + add)))
        if isinstance(v, VList):
            x = self.value_of(v.t.args[0])
            return (["%s.push(%s)" % (name, x.src())],
                    lambda env, ctx: env.set(name, VList(v.t, env.get(name).items + [x])))
        return None

    def probe(self, func, scope):
        p = Probe(len(self.probes), func)
        p.expr_hits = []
        eg = ExprGen(self.r, scope)
        exprs = []
        for _ in range(self.nexprs):
            try:
                exprs.append(eg.any_expr())
            except (IndexError, KeyError):
                pass
        if self.nexprs:
            exprs += eg.error_exprs()
            p.errors = [("nosuch_binding_%d" % self.r.randint(0, 99), "no variable `nosuch_binding_")]
        p.exprs = exprs
        self.probes.append(p)
        return ProbeSt(p, exprs)

    def function(self, fname, params):
        """params: [(name, V, is_var)]; returns [St]."""
        r = self.r
        sts = []
        scope = {n: v for n, v, _ in params}
        mutables = [(n, v) for n, v, var in params if var]
        for _ in range(r.randint(2, 7)):
            lines, ap, (n, v, mut) = self.decl(scope)
            sts.append(St(lines, ap))
            scope[n] = v
            if isinstance(v, VRc):
                scope = None or scope
            if mut:
                mutables.append((n, v))
            if r.random() < 0.25:
                sts.append(self.probe(fname, dict(scope)))
        for n, v in mutables:
            if r.random() < 0.7:
                m = self.mutation(n, v)
                if m:
                    sts.append(St(*m))
        if mutables and r.random() < 0.4:
            n, v = r.choice(mutables)
            m = self.mutation(n, v)
            if m:
                lv = self.fresh("i")
                inner = dict(scope)
                inner[lv] = VInt(T("int", name="Int"), 0)
                sts.append(Loop(lv, r.randint(1, 3), [St(*m), self.probe(fname, inner)]))
        sts.append(self.probe(fname, dict(scope)))
        # keep every binding used: no "never used" noise in the build
        return sts

    def generate(self):
        r = self.r
        nf = r.randint(1, 3)
        for k in range(nf):
            fname = "fn%d" % k
            params = []
            for i in range(r.randint(0, 3)):
                t = self.leaf_type(allow_obj=False)
                is_var = t.kind in ("int", "str") and r.random() < 0.3
                params.append(("a%d" % i, self.value_of(t), is_var, t))
            sts = self.function(fname, [(n, v, var) for n, v, var, _ in params])
            self.body_of[fname] = ([(n, t, var) for n, v, var, t in params], sts)
            for _ in range(r.choice([1, 1, 2])):
                args = [self.value_of(t) for _, _, _, t in params]
                self.calls.append((fname, args))
        main_sts = self.function("main", [])
        self.body_of["main"] = ([], main_sts)

    def render(self):
        lines = []
        for t in self.tops:
            lines += t + [""]
        self.func_ranges = {}
        for fname, (params, sts) in self.body_of.items():
            if fname == "main":
                continue
            ps = ", ".join(("var " if var else "") + "%s: %s" % (n, t.src()) for n, t, var in params)
            start = len(lines) + 1
            lines.append("func %s(%s) {" % (fname, ps))
            for s in sts:
                lines += s.emit("    ")
            lines.append("}")
            lines.append("")
            self.func_ranges[fname] = (start, len(lines))
        lines.append("func main() {")
        start = len(lines)
        pre = []
        for i, (fname, args) in enumerate(self.calls):
            params = self.body_of[fname][0]
            argsrc = []
            for (pn, pt, var), a in zip(params, args):
                if var:
                    vn = "arg%d_%s" % (i, pn)
                    self.extra_names.setdefault("main", set()).add(vn)
                    pre.append("    var %s: %s = %s" % (vn, pt.src(), a.src()))
                    argsrc.append(vn)
                else:
                    argsrc.append(a.src())
            pre.append("    %s(%s)" % (fname, ", ".join(argsrc)))
        main_body = []
        for s in self.body_of["main"][1]:
            main_body += s.emit("    ")
        lines += pre + main_body + ["}"]
        self.func_ranges["main"] = (start, len(lines))
        text = "\n".join(lines) + "\n"
        # probe line numbers
        for i, l in enumerate(lines):
            if '@P' in l and l.strip().startswith('println("@P'):
                pid = int(l.split("@P")[1].split()[0].rstrip('")'))
                self.probes[pid].line = i + 1
        return text

    def run_model(self):
        """Fill each probe's hits by running the model in call order."""
        for p in self.probes:
            p.hits, p.expr_hits = [], []
        order = []
        for i, (fname, args) in enumerate(self.calls):
            params, sts = self.body_of[fname]
            env = Env()
            for (pn, pt, var), a in zip(params, args):
                env.vars[pn] = a
            for s in sts:
                s.apply(env, None)
            # var arguments: main's copies (arg<i>_<name>) are not probed
        env = Env()
        for s in self.body_of["main"][1]:
            s.apply(env, None)


def value_program(rng, nexprs=0):
    g = ValueProgram(rng, nexprs)
    g.generate()
    g.run_model()
    # An expression that fails at some hit cannot be printed (the program
    # would panic): evaluate checks its error instead.
    for p in g.probes:
        keep = []
        for i, e in enumerate(p.exprs):
            errs = [h[i][1] for h in p.expr_hits if h[i][0] == "err"]
            if errs:
                if len(set(errs)) == 1 and len(errs) == len(p.expr_hits):
                    p.errors.append((e.src, errs[0]))
            else:
                keep.append(e)
        p.exprs[:] = keep
    text = g.render()
    g.run_model()
    return g, text
