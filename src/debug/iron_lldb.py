"""LLDB formatters for Iron values in programs built with `iron build --debug`.

Load with `command script import <iron lib>/debug/iron_lldb.py` (the VS
Code launch configurations in the guide do it).

    enum            Ok(5), Rect(2, 3), Empty; children: the payload
    interface       the implementing type and its value
    rc T, weak rc   rc Point {x = 3, y = 4} (strong=2, weak=0)
    rc [T]          rc size=2 (strong=1, weak=0); children: the elements
    closure         func __lambda_0 at main.iron:9; children: the captures
    String          "hello"
    [T]             size=3 { [0] = 1, [1] = 2, [2] = 3 }
    Map[K, V]       size=2 { [0] = "a": 1, ... }
    Set[T]          size=2 { [0] = 1, [1] = 2 }
    T?              the value, or null
"""
import lldb


def _string(valobj):
    heap = valobj.GetChildMemberWithName("heap")
    flags = heap.GetChildMemberWithName("flags").GetValueAsUnsigned(0)
    err = lldb.SBError()
    process = valobj.GetProcess()
    if flags & ~3:
        return None  # not a string yet (a binding not assigned so far)
    if flags & 1:
        n = heap.GetChildMemberWithName("byte_length").GetValueAsUnsigned(0)
        addr = heap.GetChildMemberWithName("data").GetValueAsUnsigned(0)
        if n > (1 << 28):
            return None
        data = process.ReadMemory(addr, n, err) if n else b""
    else:
        sso = valobj.GetChildMemberWithName("sso")
        n = sso.GetChildMemberWithName("len").GetValueAsUnsigned(0)
        if n > 23:
            return None
        addr = sso.GetChildMemberWithName("data").GetLoadAddress()
        data = process.ReadMemory(addr, n, err) if n else b""
    if not err.Success():
        return None
    return data.decode("utf-8", errors="replace")


def string_summary(valobj, _dict):
    s = _string(valobj.GetNonSyntheticValue())
    return "<invalid string>" if s is None else '"%s"' % s.replace('"', '\\"')


class ListProvider:
    def __init__(self, valobj, _dict):
        self.valobj = valobj

    def update(self):
        self.count = self.valobj.GetChildMemberWithName("count").GetValueAsUnsigned(0)
        self.items = self.valobj.GetChildMemberWithName("items")
        self.elem = self.items.GetType().GetPointeeType()
        return False

    def num_children(self):
        return min(self.count, 10000)

    def get_child_index(self, name):
        try:
            return int(name.strip("[]"))
        except ValueError:
            return -1

    def get_child_at_index(self, i):
        addr = self.items.GetValueAsUnsigned(0) + i * self.elem.GetByteSize()
        return self.valobj.CreateValueFromAddress("[%d]" % i, addr, self.elem)


class SlotProvider:
    """Map and Set: the full slots of an open-addressing table."""

    def __init__(self, valobj, _dict):
        self.valobj = valobj

    def update(self):
        v = self.valobj
        self.is_map = v.GetChildMemberWithName("keys").IsValid()
        self.keys = v.GetChildMemberWithName("keys" if self.is_map else "items")
        self.vals = v.GetChildMemberWithName("vals")
        cap = v.GetChildMemberWithName("cap").GetValueAsUnsigned(0)
        st = v.GetChildMemberWithName("st").GetValueAsUnsigned(0)
        err = lldb.SBError()
        states = v.GetProcess().ReadMemory(st, cap, err) if cap and st else b""
        self.full = [i for i, s in enumerate(states or b"") if s == 1]
        return False

    def num_children(self):
        return len(self.full)

    def get_child_index(self, name):
        try:
            return int(name.strip("[]"))
        except ValueError:
            return -1

    def _at(self, ptr, slot, name):
        t = ptr.GetType().GetPointeeType()
        return self.valobj.CreateValueFromAddress(
            name, ptr.GetValueAsUnsigned(0) + slot * t.GetByteSize(), t)

    def get_child_at_index(self, i):
        slot = self.full[i]
        if not self.is_map:
            return self._at(self.keys, slot, "[%d]" % i)
        key = self._at(self.keys, slot, "key")
        ksum = key.GetSummary() or key.GetValue() or "?"
        return self._at(self.vals, slot, "[%s]" % ksum)


def size_summary(valobj, _dict):
    return "size=%d" % valobj.GetNonSyntheticValue().GetChildMemberWithName(
        "count").GetValueAsUnsigned(0)


# ── Enums, interfaces, rc / weak rc, closures ───────────────────────────────


def _short(name):
    """An Iron type's C name as Iron writes it: Iron_Point is Point."""
    return name[5:] if name.startswith("Iron_") else name


def _text(v):
    """A value's text as the formatters show it."""
    s = v.GetSummary()
    if s:
        return s
    s = v.GetValue()
    if s is not None:
        return s
    n = v.GetNumChildren()
    if n > 0:
        parts = ["%s = %s" % (c.GetName(), _text(c))
                 for c in (v.GetChildAtIndex(i) for i in range(min(n, 8)))]
        return "{%s%s}" % (", ".join(parts), ", ..." if n > 8 else "")
    return "?"


def _variant(valobj):
    """(variant name, payload or None) of an enum with payloads or an
    interface value: struct { <X>_Tag tag; <X>_data_t data; }."""
    v = valobj.GetNonSyntheticValue()
    name = v.GetChildMemberWithName("tag").GetValue() or ""
    i = name.find("_TAG_")
    variant = name[i + 5:] if i >= 0 else name
    payload = v.GetChildMemberWithName("data").GetChildMemberWithName(variant)
    return variant, (payload if variant and payload.IsValid() else None)


def is_tagged(sbtype, _dict):
    t = sbtype.GetCanonicalType()
    if t.GetTypeClass() != lldb.eTypeClassStruct or t.GetNumberOfFields() != 2:
        return False
    tag, data = t.GetFieldAtIndex(0), t.GetFieldAtIndex(1)
    return (tag.GetName() == "tag" and data.GetName() == "data" and
            (tag.GetType().GetName() or "").endswith("_Tag"))


def tagged_summary(valobj, _dict):
    variant, payload = _variant(valobj)
    if payload is None or payload.GetNumChildren() == 0:
        return variant
    # A variant's payload struct names its fields _0, _1, ...; an
    # interface value holds the implementing object itself.
    if payload.GetChildAtIndex(0).GetName() == "_0":
        return "%s(%s)" % (variant, ", ".join(
            _text(payload.GetChildAtIndex(i)) for i in range(payload.GetNumChildren())))
    return "%s %s" % (variant, _text(payload))


class TaggedProvider:
    """Children of an enum or interface value: the active payload's."""

    def __init__(self, valobj, _dict):
        self.valobj = valobj
        self.payload = None

    def update(self):
        self.payload = _variant(self.valobj)[1]
        return False

    def num_children(self):
        return self.payload.GetNumChildren() if self.payload is not None else 0

    def get_child_index(self, name):
        return self.payload.GetIndexOfChildWithName(name) if self.payload is not None else -1

    def get_child_at_index(self, i):
        return self.payload.GetChildAtIndex(i)


def _rc_counts(v):
    """(strong, weak) counts of the rc block that pointer `v` points at,
    or None when it does not point at one (a plain pointer)."""
    addr = v.GetValueAsUnsigned(0)
    pointee = v.GetType().GetPointeeType()
    target = v.GetTarget()
    rch = target.FindFirstType("Iron_RcHeader")
    ahd = target.FindFirstType("IronAllocHdr")
    if not addr or not rch.IsValid() or not ahd.IsValid():
        return None
    # [Iron_RcHeader: strong, drop_fn, weak][IronAllocHdr: gen, size, ...][payload]
    base = addr - rch.GetByteSize() - ahd.GetByteSize()
    if base < 4096 or addr >= (1 << 63):
        return None
    err = lldb.SBError()
    proc = v.GetProcess()
    strong = proc.ReadUnsignedFromMemory(base, 8, err)
    weak = proc.ReadUnsignedFromMemory(base + 16, 8, err) if err.Success() else 0
    size = proc.ReadUnsignedFromMemory(base + rch.GetByteSize() + 8, 8, err) if err.Success() else 0
    if (not err.Success() or size != pointee.GetByteSize() or
            strong > (1 << 40) or weak == 0 or weak > (1 << 40)):
        return None
    return strong, weak


def is_rc_pointer(sbtype, _dict):
    if not sbtype.IsPointerType():
        return False
    p = sbtype.GetPointeeType().GetCanonicalType()
    name = p.GetName() or ""
    return (p.GetTypeClass() == lldb.eTypeClassStruct and name.startswith("Iron_") and
            name not in ("Iron_String", "Iron_RcHeader", "Iron_Closure") and
            not name.startswith(("Iron_List_", "Iron_Map_", "Iron_Set_", "Iron_Optional_")))


_rc_depth = [0]


def rc_summary(valobj, _dict):
    """An rc or weak rc value: the object and its counts. Plain pointers
    (a `self` receiver, an unchecked pointer) get no summary."""
    v = valobj.GetNonSyntheticValue()
    if not v.GetValueAsUnsigned(0):
        return "null"
    counts = _rc_counts(v)
    if counts is None:
        return None
    strong, weak = counts
    if strong == 0:
        return "dropped (a weak rc whose object was freed)"
    obj = v.Dereference()
    items = obj.GetChildMemberWithName("items")
    if _rc_depth[0] > 2:
        body = "{...}"
    else:
        _rc_depth[0] += 1
        try:
            # rc [T] holds the list in `items`.
            body = _text(items) if obj.GetNumChildren() == 1 and items.IsValid() else \
                "%s %s" % (_short(obj.GetTypeName() or ""), _text(obj))
        finally:
            _rc_depth[0] -= 1
    # weak counts one extra reference shared by the strong ones.
    return "rc %s (strong=%d, weak=%d)" % (body, strong, weak - 1)


def _closure_function(v):
    fn = v.GetChildMemberWithName("fn").GetValueAsUnsigned(0)
    return v.GetTarget().ResolveLoadAddress(fn).GetFunction() if fn else None


def closure_summary(valobj, _dict):
    v = valobj.GetNonSyntheticValue()
    f = _closure_function(v)
    if f is None:
        return "null"
    if not f.IsValid():
        return "func at 0x%x" % v.GetChildMemberWithName("fn").GetValueAsUnsigned(0)
    where = ""
    # The body's first instruction on an Iron line gives its location.
    insts = f.GetInstructions(v.GetTarget())
    for i in range(min(insts.GetSize(), 64)):
        e = insts.GetInstructionAtIndex(i).GetAddress().GetLineEntry()
        name = e.GetFileSpec().GetFilename() or ""
        if name.endswith(".iron"):
            where = " at %s:%d" % (name, e.GetLine())
            break
    return "func %s%s" % (f.GetName(), where)


class ClosureProvider:
    """Children of a closure: the bindings it captured (its env)."""

    def __init__(self, valobj, _dict):
        self.valobj = valobj
        self.env = None

    def update(self):
        self.env = None
        v = self.valobj.GetNonSyntheticValue()
        f = _closure_function(v)
        env = v.GetChildMemberWithName("env").GetValueAsUnsigned(0)
        if f is not None and f.IsValid() and env:
            t = v.GetTarget().FindFirstType("%s_env_t" % f.GetName())
            if t.IsValid():
                self.env = v.CreateValueFromAddress("env", env, t)
        return False

    def num_children(self):
        return self.env.GetNumChildren() if self.env is not None else 0

    def get_child_index(self, name):
        return self.env.GetIndexOfChildWithName(name) if self.env is not None else -1

    def get_child_at_index(self, i):
        return self.env.GetChildAtIndex(i)


def _register_values(debugger):
    m = __name__
    ci = debugger.GetCommandInterpreter()

    def run(cmd):
        res = lldb.SBCommandReturnObject()
        ci.HandleCommand(cmd, res)
        return res.Succeeded()

    run('type summary add -e -F %s.closure_summary "Iron_Closure"' % m)
    run('type synthetic add -l %s.ClosureProvider "Iron_Closure"' % m)
    # Enums, interface values and rc pointers are recognized by their
    # shape rather than their name (LLDB 16 and later).
    for kind, fn, rec in (("summary", "-F %s.tagged_summary", "is_tagged"),
                          ("synthetic", "-l %s.TaggedProvider", "is_tagged"),
                          ("summary", "-F %s.rc_summary", "is_rc_pointer")):
        run('type %s add %s --recognizer-function %s.%s' % (kind, fn % m, m, rec))


def optional_summary(valobj, _dict):
    v = valobj.GetNonSyntheticValue()
    if not v.GetChildMemberWithName("has_value").GetValueAsUnsigned(0):
        return "null"
    inner = v.GetChildMemberWithName("value")
    return inner.GetSummary() or inner.GetValue() or "?"


def __lldb_init_module(debugger, _dict):
    m = __name__
    run = debugger.HandleCommand
    run('type summary add -F %s.string_summary "Iron_String"' % m)
    run('type synthetic add -x "^Iron_List_" -l %s.ListProvider' % m)
    run('type summary add -e -x "^Iron_List_" -F %s.size_summary' % m)
    run('type synthetic add -x "^Iron_(Map|Set)_" -l %s.SlotProvider' % m)
    run('type summary add -e -x "^Iron_(Map|Set)_" -F %s.size_summary' % m)
    run('type summary add -x "^Iron_Optional_" -F %s.optional_summary' % m)
    _register_values(debugger)
