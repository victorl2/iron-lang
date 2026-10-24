"""gdb pretty printers for Iron values in programs built with `iron build --debug`.

Load with `source <iron lib>/debug/iron_gdb.py` (the VS Code launch
configuration in the guide does it through setupCommands).

    enum            Green, Ok(5), Rect(2, 3), Empty
    interface       the implementing type and its value
    rc T, weak rc   rc Point {x = 3, y = 4} (strong=2, weak=0)
    rc [T]          rc [2] = {"a", "b"} (strong=1, weak=0)
    closure         func __lambda_0 at main.iron:9; children: the captures
    String          "hello"
    [T]             [3] = {1, 2, 3}
    Map[K, V]       Map[2] = {["a"] = 1, ["b"] = 2}
    Set[T]          Set[2] = {1, 2}
    T?              the value, or null

The `locals` command lists the selected frame's Iron bindings without the
compiler's temporaries (see iron_variables).
"""
import re

import gdb
import gdb.printing


# Break on panic: every Iron panic (a failed assert, an index out of
# bounds, a missing map key, out of memory) prints its message and ends in
# the C library's abort(). With `break abort` (iron debug and iron dap set
# it), a stop there selects the Iron frame that panicked. A --debug build
# stops earlier for the panics of its checks, on a trap in the Iron
# function itself (iron_trap_frame).

PANIC_FUNCTIONS = ("abort", "__abort", "raise", "__pthread_kill", "pthread_kill",
                   "__pthread_kill_implementation", "gsignal")


def iron_panic_frame():
    """The Iron frame a panic stopped in, or None."""
    try:
        f = gdb.newest_frame()
    except gdb.error:
        return None
    top = []
    g = f
    while g is not None and len(top) < 4:
        top.append(g.name() or "")
        g = g.older()
    if not any(n.replace("__GI_", "") in PANIC_FUNCTIONS for n in top):
        return None
    while f is not None:
        sal = f.find_sal()
        if sal.symtab is not None and sal.symtab.filename.endswith(".iron"):
            return f
        f = f.older()
    return None


def iron_trap_message():
    """The panic message a --debug build's debug trap left, or ""."""
    try:
        return gdb.parse_and_eval("(const char *)iron_debug_panic_message").string()
    except (gdb.error, UnicodeDecodeError):
        return ""


def iron_trap_frame(event):
    """The Iron frame of a stop on a --debug build's debug trap (#388): a
    check that failed under a debugger runs a breakpoint instruction in
    the Iron function, before the panic. None for any other stop."""
    # gdb reports a SIGTRAP that hit no breakpoint as a plain StopEvent.
    if isinstance(event, gdb.BreakpointEvent) or \
            (isinstance(event, gdb.SignalEvent) and event.stop_signal != "SIGTRAP"):
        return None
    try:
        f = gdb.newest_frame()
    except gdb.error:
        return None
    sal = f.find_sal()
    if sal.symtab is None or not sal.symtab.filename.endswith(".iron"):
        return None
    if not iron_trap_message():
        return None
    return f


def _step_over_brk(f):
    """On AArch64 the trap (brk #0xf000) leaves the pc on itself, and a
    continue would run it again: move past it, still on the Iron line."""
    try:
        if not f.architecture().name().startswith("aarch64"):
            return
        pc = f.pc()
        word = int.from_bytes(bytes(gdb.selected_inferior().read_memory(pc, 4)), "little")
        if word == 0xd43e0000:
            gdb.execute("set var $pc = %d" % (pc + 4), to_string=True)
    except (gdb.error, gdb.MemoryError, ValueError):
        pass


def _on_stop(event):
    t = iron_trap_frame(event)
    if t is not None:
        _step_over_brk(t)
        sal = t.find_sal()
        gdb.write("Iron panic at %s:%d: %s\n"
                  % (sal.symtab.filename.split("/")[-1], sal.line, iron_trap_message()))
        return
    f = iron_panic_frame()
    if f is not None:
        f.select()
        sal = f.find_sal()
        gdb.write("Iron panic at %s:%d (frame #%d is selected)\n"
                  % (sal.symtab.filename.split("/")[-1], sal.line, f.level()))


gdb.events.stop.connect(_on_stop)


def _string(val):
    """The text of an Iron_String (small-string or heap form)."""
    heap = val["heap"]
    flags = int(heap["flags"])
    if flags & ~3:
        raise gdb.error("not a string yet")  # a binding not assigned so far
    if flags & 1:
        n = int(heap["byte_length"])
        if n > (1 << 28):
            raise gdb.error("not a string yet")
        return heap["data"].string(encoding="utf-8", errors="replace", length=n)
    sso = val["sso"]
    n = int(sso["len"])
    if n > 23:
        raise gdb.error("not a string yet")
    return sso["data"].string(encoding="utf-8", errors="replace", length=n)


def iron_quote(s):
    """A string as an Iron literal: the escapes of the manual (\\n, \\t,
    \\\\, \\"), any other control character (a zero byte too) as \\u{H}."""
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
    return '"%s"' % "".join(out)


def float_text(v, is_f32=False):
    """A float as Iron prints it (src/runtime/iron_fmt.c): the shortest
    digits that read back to the value, no trailing .0, inf / -inf / NaN."""
    import math
    import struct
    if math.isnan(v):
        return "NaN"
    if math.isinf(v):
        return "inf" if v > 0 else "-inf"
    if v == 0:
        return "0"

    def f32(x):
        return struct.unpack("f", struct.pack("f", x))[0]
    for p in range(1, 10 if is_f32 else 18):
        sci = "%.*e" % (p - 1, v)
        if (f32(float(sci)) == f32(v)) if is_f32 else float(sci) == v:
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


def _dap():
    """True inside gdb's DAP server (gdb -i=dap): a value there shows its
    summary and is expanded through its children; on the command line
    the summary alone is printed, as LLDB does."""
    import sys
    return "gdb.dap" in sys.modules


class StringPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            return iron_quote(_string(self.val))
        except gdb.error:
            return NOT_ASSIGNED


class FloatPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return float_text(float(self.val), self.val.type.strip_typedefs().sizeof == 4)


class SmallIntPrinter:
    """Int8 and UInt8: numbers, not characters."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        return str(int(self.val))

    def display_hint(self):
        return None


NOT_ASSIGNED = "<not assigned yet>"


def _list_count(val):
    """A list's element count, or None when the struct holds no list yet
    (a binding read before the statement that assigns it)."""
    try:
        count, cap, items = int(val["count"]), int(val["capacity"]), int(val["items"])
    except gdb.error:
        return None
    if count < 0 or count > (1 << 40) or cap < count or (count and not items):
        return None
    return count


def _table_count(val):
    try:
        count, cap, st = int(val["count"]), int(val["cap"]), int(val["st"])
    except gdb.error:
        return None
    if count < 0 or cap < 0 or cap > (1 << 30) or count > cap or (cap and not st):
        return None
    return count


def _text(v, depth=0):
    """A value's text as the printers write it, without its children
    (format_string adds ` = {...}` after a summary that has children)."""
    try:
        p = gdb.default_visualizer(v)
        if p is not None and hasattr(p, "to_string") and depth < 8:
            s = p.to_string()
            if isinstance(s, gdb.Value):
                return _text(s, depth + 1)
            if s is not None:
                return str(s)
        return v.format_string()
    except gdb.error as e:
        return "<%s>" % e


def _list_text(val):
    """A list as LLDB shows it: [1, 2, 3] (the first 20 elements)."""
    n = _list_count(val)
    if n is None:
        return NOT_ASSIGNED
    items = val["items"]
    shown = [_text(items[i]) for i in range(min(n, 20))]
    return "[%s%s]" % (", ".join(shown), ", ..." if n > 20 else "")


def _align(t):
    """The alignment of a C type, from its layout: the largest alignment
    among a struct's members, an array's element alignment, and a scalar's
    own size."""
    t = t.strip_typedefs()
    if t.code == gdb.TYPE_CODE_ARRAY:
        return _align(t.target())
    if t.code in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
        a = 1
        for f in t.fields():
            if not f.is_base_class and hasattr(f, "bitpos"):
                a = max(a, _align(f.type))
        return a
    return max(1, min(t.sizeof, 16))


def _soa_columns(elem):
    """Where a `layout: soa` list keeps each member of its element type:
    (offset in the element, size, bytes per element of capacity before the
    member's column). Columns follow decreasing alignment, declaration
    order among equals (emit_structs.c emit_object_soa_impl)."""
    members = []
    for i, f in enumerate(elem.fields()):
        members.append((i, f.bitpos // 8, f.type.sizeof, _align(f.type)))
    cols, before = [], 0
    for i, off, size, _a in sorted(members, key=lambda m: (-m[3], m[0])):
        cols.append((off, size, before))
        before += size
    return cols


class ListPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        n = _list_count(self.val)
        if n is None:
            return NOT_ASSIGNED
        if _dap():
            if self._is_soa():
                # Elements are gathered from the columns (see children).
                shown = []
                for _name, v in self.children():
                    if len(shown) == 20:
                        break
                    shown.append(_text(v))
                return "[%s%s]" % (", ".join(shown), ", ..." if n > 20 else "")
            return _list_text(self.val)
        return "[%d]" % n

    def _is_soa(self):
        # A list of an element type used with layout: soa has a `soa` flag;
        # when it is set, items points at one block of columns.
        t = self.val.type.strip_typedefs()
        return any(f.name == "soa" for f in t.fields()) and bool(self.val["soa"])

    def children(self):
        items = self.val["items"]
        n = min(_list_count(self.val) or 0, 10000)
        if not self._is_soa():
            for i in range(n):
                yield "[%d]" % i, items[i]
            return
        elem = items.type.target().strip_typedefs()
        columns = _soa_columns(elem)
        base, capacity = int(items), int(self.val["capacity"])
        inferior = gdb.selected_inferior()
        for i in range(n):
            buf = bytearray(elem.sizeof)
            for off, size, before in columns:
                if size:
                    chunk = inferior.read_memory(base + capacity * before + i * size, size)
                    buf[off:off + size] = bytes(chunk)
            yield "[%d]" % i, gdb.Value(bytes(buf), elem)

    def display_hint(self):
        return "array"


class MapPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        n = _table_count(self.val)
        return NOT_ASSIGNED if n is None else "Map[%d]" % n

    def children(self):
        # Each entry as [key] = value, as LLDB shows it.
        st, keys, vals = self.val["st"], self.val["keys"], self.val["vals"]
        if _table_count(self.val) is None:
            return
        for i in range(int(self.val["cap"])):
            if int(st[i]) == 1:
                yield "[%s]" % _text(keys[i]), vals[i]


class SetPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        n = _table_count(self.val)
        return NOT_ASSIGNED if n is None else "Set[%d]" % n

    def children(self):
        st, items = self.val["st"], self.val["items"]
        n = 0
        if _table_count(self.val) is None:
            return
        for i in range(int(self.val["cap"])):
            if int(st[i]) == 1:
                yield "[%d]" % n, items[i]
                n += 1

    def display_hint(self):
        return "array"


class OptionalPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        if not bool(self.val["has_value"]):
            return "null"
        return self.val["value"]


# ── Enums, interfaces, rc / weak rc, closures ───────────────────────────────


def _short(name):
    """An Iron type's C name as Iron writes it: Iron_Point is Point, and a
    generic instance Iron_Box__Int is Box[Int] (arguments that are plain
    type names; others keep the compiler's spelling)."""
    name = name[5:] if name.startswith("Iron_") else name
    parts = name.split("__")
    if len(parts) > 1 and all(p and p[0].isalpha() and p.isalnum() for p in parts):
        return "%s[%s]" % (parts[0], ", ".join(parts[1:]))
    return name


RUNTIME_TYPES = ("Iron_String", "Iron_RcHeader", "Iron_Closure", "Iron_Arena_RT",
                 "Iron_FatPtr", "Iron_Mutex", "Iron_Channel")


def _is_object(t, name):
    if t.code != gdb.TYPE_CODE_STRUCT or not name.startswith("Iron_") or name in RUNTIME_TYPES:
        return False
    if name.startswith(("Iron_List_", "Iron_Map_", "Iron_Set_", "Iron_Optional_",
                        "Iron_SplitList_", "Iron_Tuple_")):
        return False
    return not _is_tagged(t) and len(_fields(t)) > 0


# Objects that hold only a native handle, and the runtime call that
# describes what the handle holds.
HANDLE_TEXT = {"MysqlResult": "iron_mysql_debug_text(%d, 0)",
               "MysqlConn": "iron_mysql_debug_text(%d, 1)",
               "MysqlPool": "iron_mysql_debug_text(%d, 2)",
               "HttpClient": "iron_http_debug_text(%d)"}


def _json_text(v, budget):
    """A Json value as JSON text (at most about budget[0] characters)."""
    if budget[0] <= 0:
        return "..."
    node = v["node"]
    tag = str(node["tag"])
    i = tag.find("_TAG_")
    variant = tag[i + 5:] if i >= 0 else tag
    data = node["data"]
    names = [f.name for f in _fields(data.type.strip_typedefs())]
    inner = data[variant]["_0"] if variant in names and \
        [f.name for f in _fields(data[variant].type.strip_typedefs())][:1] == ["_0"] else None
    if variant == "Null" or inner is None:
        out = "null"
    elif variant in ("Bool", "Int", "Float", "Str"):
        out = _text(inner)
    elif variant == "Arr":
        items = inner.dereference()["items"]
        n = _list_count(items) or 0
        parts = []
        for k in range(min(n, 50)):
            if budget[0] <= 0:
                parts.append("...")
                break
            parts.append(_json_text(items["items"][k], budget))
        out = "[%s]" % ", ".join(parts)
    elif variant == "Obj":
        obj = inner.dereference()
        keys, vals = obj["keys"], obj["values"]
        n = _list_count(keys) or 0
        parts = []
        for k in range(min(n, 50)):
            if budget[0] <= 0:
                parts.append("...")
                break
            parts.append("%s: %s" % (_text(keys["items"][k]), _json_text(vals["items"][k], budget)))
        out = "{%s}" % ", ".join(parts)
    else:
        out = variant
    budget[0] -= len(out)
    return out


class ObjectPrinter:
    """An object: Point {x = 3, y = 4}, as LLDB and rc values show it. A
    Json value shows as its JSON text, a MySQL result, connection or pool
    and an HttpClient as what its handle holds."""

    def __init__(self, val, name):
        self.val, self.name = val, _short(name)

    def to_string(self):
        if self.name == "Json":
            return "Json " + _json_text(self.val, [400])
        if self.name.startswith("MutexGuard"):
            try:
                p = self.val["valptr"]
                return "MutexGuard(%s)" % (_text(p.dereference()) if int(p) else "null")
            except (gdb.error, gdb.MemoryError):
                pass
        if self.name in HANDLE_TEXT:
            try:
                text = gdb.parse_and_eval("(const char *)" + HANDLE_TEXT[self.name] % int(
                    self.val["handle"])).string()
                return "%s %s" % (self.name, text)
            except (gdb.error, UnicodeDecodeError):
                pass
        if self.name in ("JsonBuffer", "JsonWriter"):
            # What a JsonWriter wrote so far: its native buffer (iron_json.c).
            try:
                buf = self.val if self.name == "JsonBuffer" else self.val["buf"].dereference()
                w = gdb.Value(int(buf["handle"])).cast(
                    gdb.lookup_type("JsonWriter").pointer()).dereference()
                n = int(w["n"])
                if 0 <= n < (1 << 30):
                    raw = bytes(gdb.selected_inferior().read_memory(int(w["d"]), min(n, 300))) \
                        if n else b""
                    return "%s %s%s" % (self.name, iron_quote(raw.decode("utf-8", "replace")),
                                                " ..." if n > 300 else "")
            except (gdb.error, gdb.MemoryError):
                pass
        return self.fields_text()

    def fields_text(self):
        f = [x for x in _fields(self.val.type.strip_typedefs()) if x.name]
        parts = ["%s = %s" % (x.name, _text(self.val[x.name])) for x in f[:8]]
        return "%s {%s%s}" % (self.name, ", ".join(parts), ", ..." if len(f) > 8 else "")

    def children(self):
        if not _dap():
            return
        for x in _fields(self.val.type.strip_typedefs()):
            if x.name:
                yield x.name, self.val[x.name]


def _fields(t):
    try:
        return t.fields()
    except TypeError:
        return []


def _is_tagged(t):
    """An enum with payloads or an interface value:
    struct { <X>_Tag tag; <X>_data_t data; }."""
    f = _fields(t)
    return (t.code == gdb.TYPE_CODE_STRUCT and len(f) == 2 and f[0].name == "tag" and
            f[1].name == "data" and str(f[0].type).endswith("_Tag"))


class TaggedPrinter:
    def __init__(self, val):
        self.val = val
        tag = str(val["tag"])
        i = tag.find("_TAG_")
        self.variant = tag[i + 5:] if i >= 0 else tag
        data = val["data"]
        names = [f.name for f in _fields(data.type.strip_typedefs())]
        self.payload = data[self.variant] if self.variant in names else None

    def to_string(self):
        p = self.payload
        f = _fields(p.type.strip_typedefs()) if p is not None else []
        if not f:
            return self.variant
        # A variant's payload struct names its fields _0, _1, ...; an
        # interface value holds the implementing object itself.
        if f[0].name == "_0":
            return "%s(%s)" % (self.variant, ", ".join(_text(p[x.name]) for x in f))
        text = _text(p)
        t = p.type.strip_typedefs()
        if text.startswith(_short(t.tag or t.name or str(p.type)) + " {"):
            return text
        return "%s %s" % (self.variant, text)

    def children(self):
        if not _dap() or self.payload is None:
            return
        for x in _fields(self.payload.type.strip_typedefs()):
            if x.name:
                yield x.name, self.payload[x.name]


class EnumPrinter:
    """An enum without payloads: Green rather than Iron_Color_Green."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        text = str(self.val.cast(self.val.type.strip_typedefs()))
        prefix = str(self.val.type) + "_"
        return text[len(prefix):] if text.startswith(prefix) else text


def _rc_counts(val):
    """(strong, weak) counts of the rc block that pointer `val` points
    at, or None when it does not point at one (a plain pointer)."""
    try:
        rch = gdb.lookup_type("Iron_RcHeader")
        ahd = gdb.lookup_type("IronAllocHdr")
    except gdb.error:
        return None
    addr = int(val)
    # [Iron_RcHeader: strong, drop_fn, weak][IronAllocHdr: gen, size, ...][payload]
    base = addr - rch.sizeof - ahd.sizeof
    if base < 4096 or addr >= (1 << 63):
        return None
    u64 = gdb.lookup_type("unsigned long long").pointer()
    try:
        strong = int(gdb.Value(base).cast(u64).dereference())
        weak = int(gdb.Value(base + 16).cast(u64).dereference())
        size = int(gdb.Value(base + rch.sizeof + 8).cast(u64).dereference())
    except gdb.error:
        return None
    if (size != val.type.strip_typedefs().target().sizeof or strong > (1 << 40) or
            weak == 0 or weak > (1 << 40)):
        return None
    return strong, weak


def _is_rc_pointer(t):
    if t.code != gdb.TYPE_CODE_PTR:
        return False
    p = t.target().strip_typedefs()
    name = p.tag or p.name or str(t.target())
    return (p.code == gdb.TYPE_CODE_STRUCT and name.startswith("Iron_") and
            name not in ("Iron_String", "Iron_RcHeader", "Iron_Closure") and
            not name.startswith(("Iron_List_", "Iron_Map_", "Iron_Set_", "Iron_Optional_")))


class NullPrinter:
    def to_string(self):
        return "null"


_rc_depth = [0]


class RcPrinter:
    """An rc or weak rc value: the object and its counts."""

    def __init__(self, val, counts):
        self.val = val
        self.counts = counts

    def to_string(self):
        strong, weak = self.counts
        if strong == 0:
            return "dropped (a weak rc whose object was freed)"
        obj = self.val.dereference()
        t = obj.type.strip_typedefs()
        f = _fields(t)
        if _rc_depth[0] > 2:
            body = "{...}"
        else:
            _rc_depth[0] += 1
            try:
                # rc [T] holds the list in `items`.
                if len(f) == 1 and f[0].name == "items":
                    body = _list_text(obj["items"])
                else:
                    body = _text(obj)
                    name = _short(t.tag or t.name or str(obj.type))
                    if not body.startswith(name + " "):
                        body = "%s %s" % (name, body)
            finally:
                _rc_depth[0] -= 1
        # weak counts one extra reference shared by the strong ones.
        return "rc %s (strong=%d, weak=%d)" % (body, strong, weak - 1)

    def children(self):
        # The object's fields (an rc [T]: its elements), as LLDB shows them.
        if not _dap() or self.counts[0] == 0:
            return
        obj = self.val.dereference()
        f = [x for x in _fields(obj.type.strip_typedefs()) if x.name]
        if len(f) == 1 and f[0].name == "items":
            p = gdb.default_visualizer(obj["items"])
            if p is not None and hasattr(p, "children"):
                for c in p.children():
                    yield c
            return
        for x in f:
            yield x.name, obj[x.name]


class ClosurePrinter:
    """A closure: its function and where it is; children: its captures."""

    def __init__(self, val):
        self.val = val
        self.fn = int(val["fn"])
        self.name = None
        self.where = ""
        block = gdb.block_for_pc(self.fn) if self.fn else None
        func = block.function if block is not None else None
        if func is not None:
            self.name = func.name
            # The body's first address on an Iron line gives its location.
            pc = block.start
            while pc < min(block.end, block.start + 512):
                sal = gdb.find_pc_line(pc)
                if sal.symtab is not None and sal.symtab.filename.endswith(".iron"):
                    self.where = " at %s:%d" % (sal.symtab.filename.split("/")[-1], sal.line)
                    break
                pc = sal.last + 1 if sal.last is not None and sal.last >= pc else pc + 1

    def to_string(self):
        if not self.fn:
            return "null"
        return "func %s%s" % (self.name or hex(self.fn), self.where)

    def children(self):
        env = int(self.val["env"])
        if not env or not self.name:
            return
        try:
            t = gdb.lookup_type("%s_env_t" % self.name)
        except gdb.error:
            return
        e = gdb.Value(env).cast(t.pointer()).dereference()
        for f in _fields(t.strip_typedefs()):
            v = e[f.name]
            # A captured var: a pointer to its cell (<env>_cell_<name>),
            # shown as the value it holds.
            if f.type.code == gdb.TYPE_CODE_PTR and "_cell_" in str(f.type.target().name or "") \
                    and int(v):
                v = v.dereference()
            yield f.name, v


def _lookup_values(val):
    t = val.type.strip_typedefs()
    if str(val.type) == "Iron_Closure":
        return ClosurePrinter(val)
    if _is_tagged(t):
        return TaggedPrinter(val)
    if t.code == gdb.TYPE_CODE_ENUM and str(val.type).startswith("Iron_") and \
            not str(val.type).endswith("_Tag"):
        return EnumPrinter(val)
    if _is_rc_pointer(t):
        if int(val) == 0:
            return NullPrinter()
        counts = _rc_counts(val)
        if counts is not None:
            return RcPrinter(val, counts)
    return None


class SafePrinter:
    """A printer that never fails: a binding read before the statement
    that assigns it holds whatever its memory held, and one printer that
    raised made gdb's DAP server fail the whole Locals scope."""

    def __init__(self, inner):
        self.inner = inner
        if hasattr(inner, "display_hint"):
            self.display_hint = inner.display_hint
        if hasattr(inner, "children"):
            self.children = self._children

    def to_string(self):
        try:
            return self.inner.to_string()
        except (gdb.error, RuntimeError, ValueError, OverflowError, UnicodeError):
            return NOT_ASSIGNED

    def _children(self):
        out = []
        try:
            for name, v in self.inner.children():
                if isinstance(v, gdb.Value):
                    v.fetch_lazy()      # unreadable memory raises here, not in gdb
                out.append((name, v))
                if len(out) >= 10000:
                    break
        except (gdb.error, RuntimeError, ValueError, OverflowError, UnicodeError):
            pass
        return out


class MutexPrinter:
    """A Mutex[T] of a --debug build (Iron_Mutex__T *): the value it holds
    and which thread holds the lock."""

    def __init__(self, val, vtype):
        self.val, self.vtype = val, vtype

    def value(self):
        m = self.val.dereference()
        p = int(m["value"])
        return gdb.Value(p).cast(self.vtype.pointer()).dereference() if p else None

    def to_string(self):
        if not int(self.val):
            return "null"
        v = self.value()
        holder = int(self.val.dereference()["holder"])
        return "Mutex(%s)%s" % (_text(v) if v is not None else "?",
                                " locked by thread %d (0x%x)" % (holder, holder) if holder
                                else " unlocked")

    def children(self):
        if not _dap() or not int(self.val):
            return
        v = self.value()
        if v is not None:
            yield "value", v


class FatPtrPrinter:
    """A checked pointer or heap value of a --debug build (a typedef of
    Iron_FatPtr named after the pointee, Iron_FatPtr__Iron_Worker): the
    value it points at."""

    def __init__(self, val, pointee):
        self.val, self.pointee = val, pointee
        self.obj, self.state = None, ""
        addr, gen = int(val["addr"]), int(val["gen"])
        if not addr:
            self.state = "null"
            return
        try:
            hdr = gdb.lookup_type("IronAllocHdr")
            u64 = gdb.lookup_type("unsigned long long").pointer()
            base = addr - hdr.sizeof
            hgen = int(gdb.Value(base).cast(u64).dereference())
            size = int(gdb.Value(base + 8).cast(u64).dereference())
            if size == pointee.sizeof and hgen != gen:
                self.state = "freed"
                return
        except gdb.error:
            pass
        self.obj = gdb.Value(addr).cast(pointee.pointer()).dereference()

    def to_string(self):
        if self.state == "null":
            return "null"
        if self.state == "freed":
            return "freed (a stale pointer)"
        return _text(self.obj)

    def children(self):
        if not _dap() or self.obj is None:
            return
        p = gdb.default_visualizer(self.obj)
        if p is not None and hasattr(p, "children"):
            for c in p.children():
                yield c
            return
        t = self.obj.type.strip_typedefs()
        if t.code == gdb.TYPE_CODE_STRUCT:
            for x in _fields(t):
                if x.name:
                    yield x.name, self.obj[x.name]


def _lookup(val):
    try:
        p = _lookup_printer(val)
    except (gdb.error, RuntimeError, ValueError, OverflowError):
        return None
    return SafePrinter(p) if p is not None else None


def _lookup_printer(val):
    # Anonymous structs (`typedef struct { ... } Iron_Optional_Int64;`)
    # carry their name on the typedef, not on the stripped type.
    t = val.type.strip_typedefs()
    name = t.tag or t.name or val.type.name or str(val.type)
    if t.code == gdb.TYPE_CODE_FLT:
        return FloatPrinter(val)
    if t.code == gdb.TYPE_CODE_PTR and (val.type.strip_typedefs().target().name or
                                        str(val.type.target())).startswith("Iron_Mutex__"):
        tn = str(val.type.target())
        try:
            return MutexPrinter(val, gdb.lookup_type(tn[len("Iron_Mutex__"):]))
        except gdb.error:
            pass
    if (val.type.name or "").startswith("Iron_FatPtr__"):
        try:
            pointee = gdb.lookup_type(val.type.name[len("Iron_FatPtr__"):])
        except gdb.error:
            pointee = None
        if pointee is not None:
            return FatPtrPrinter(val, pointee)
    if val.type.name in ("int8_t", "uint8_t"):
        return SmallIntPrinter(val)
    if _is_object(t, name):
        return ObjectPrinter(val, name)
    if name == "Iron_String":
        return StringPrinter(val)
    if name.startswith("Iron_List_"):
        return ListPrinter(val)
    if name.startswith("Iron_Map_"):
        return MapPrinter(val)
    if name.startswith("Iron_Set_"):
        return SetPrinter(val)
    if name.startswith("Iron_Optional_"):
        return OptionalPrinter(val)
    return _lookup_values(val)


gdb.pretty_printers.append(_lookup)


def iron_variables(frame):
    """The frame's parameters and locals as Iron sees them: [(name, value)].

    In a --debug build a C local named after an Iron binding is that
    binding; a name starting with `_` is the compiler's (a temporary, a
    synthetic parameter), except `_ref_<name>`, a pointer to the Iron
    binding <name> (a var a closure captures, or a capture inside the
    closure), shown as the value it points at.
    """
    out, seen = [], set()
    try:
        block = frame.block()
    except RuntimeError:
        return out
    while block is not None:
        for sym in block:
            if not (sym.is_argument or sym.is_variable) or sym.name in seen:
                continue
            seen.add(sym.name)
            name = sym.name
            if name.startswith("_ref_") and len(name) > 5:
                try:
                    out.append((name[5:], frame.read_var(sym, block).dereference()))
                except gdb.error:
                    pass
            elif not name.startswith("_"):
                out.append((name, frame.read_var(sym, block)))
        if block.function is not None:
            break
        block = block.superblock
    return out


class LocalsCommand(gdb.Command):
    """locals: the Iron parameters and locals of the selected frame,
without the compiler's temporaries (`info locals` shows them all)."""

    def __init__(self):
        super().__init__("locals", gdb.COMMAND_STACK)

    def invoke(self, arg, from_tty):
        for name, value in iron_variables(gdb.selected_frame()):
            try:
                text = value.format_string()
            except gdb.error as e:
                text = "<%s>" % e
            gdb.write("%s = %s\n" % (name, text))


LocalsCommand()
