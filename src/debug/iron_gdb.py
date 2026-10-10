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


class StringPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            return '"%s"' % _string(self.val).replace('"', '\\"')
        except gdb.error:
            return "<invalid string>"

    def display_hint(self):
        return None


class ListPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return "[%d]" % int(self.val["count"])

    def children(self):
        items = self.val["items"]
        for i in range(min(int(self.val["count"]), 10000)):
            yield "[%d]" % i, items[i]

    def display_hint(self):
        return "array"


class MapPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return "Map[%d]" % int(self.val["count"])

    def children(self):
        st, keys, vals = self.val["st"], self.val["keys"], self.val["vals"]
        n = 0
        for i in range(int(self.val["cap"])):
            if int(st[i]) == 1:
                yield "key%d" % n, keys[i]
                yield "val%d" % n, vals[i]
                n += 1

    def display_hint(self):
        return "map"


class SetPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return "Set[%d]" % int(self.val["count"])

    def children(self):
        st, items = self.val["st"], self.val["items"]
        n = 0
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
    """An Iron type's C name as Iron writes it: Iron_Point is Point."""
    return name[5:] if name.startswith("Iron_") else name


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
            return "%s(%s)" % (self.variant, ", ".join(p[x.name].format_string() for x in f))
        return "%s %s" % (self.variant, p.format_string())


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
                    body = obj["items"].format_string()
                else:
                    body = "%s %s" % (_short(t.tag or t.name or str(obj.type)), obj.format_string())
            finally:
                _rc_depth[0] -= 1
        # weak counts one extra reference shared by the strong ones.
        return "rc %s (strong=%d, weak=%d)" % (body, strong, weak - 1)


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
            yield f.name, e[f.name]


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


def _lookup(val):
    # Anonymous structs (`typedef struct { ... } Iron_Optional_Int64;`)
    # carry their name on the typedef, not on the stripped type.
    t = val.type.strip_typedefs()
    name = t.tag or t.name or val.type.name or str(val.type)
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
