"""LLDB formatters for Iron values in programs built with `iron build --debug`.

Load with `command script import <iron lib>/debug/iron_lldb.py` (the VS
Code launch configurations in the guide do it).

    String          "hello"
    [T]             size=3 { [0] = 1, [1] = 2, [2] = 3 }
    Map[K, V]       size=2 { [0] = "a": 1, ... }
    Set[T]          size=2 { [0] = 1, [1] = 2 }
    T?              the value, or null

The `locals` command lists the selected frame's Iron bindings without the
compiler's temporaries (see iron_variables).
"""
import lldb


def _string(valobj):
    heap = valobj.GetChildMemberWithName("heap")
    flags = heap.GetChildMemberWithName("flags").GetValueAsUnsigned(0)
    err = lldb.SBError()
    process = valobj.GetProcess()
    if flags & 1:
        n = heap.GetChildMemberWithName("byte_length").GetValueAsUnsigned(0)
        addr = heap.GetChildMemberWithName("data").GetValueAsUnsigned(0)
        data = process.ReadMemory(addr, n, err) if n else b""
    else:
        sso = valobj.GetChildMemberWithName("sso")
        n = sso.GetChildMemberWithName("len").GetValueAsUnsigned(0)
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


def optional_summary(valobj, _dict):
    v = valobj.GetNonSyntheticValue()
    if not v.GetChildMemberWithName("has_value").GetValueAsUnsigned(0):
        return "null"
    inner = v.GetChildMemberWithName("value")
    return inner.GetSummary() or inner.GetValue() or "?"


def iron_variables(frame):
    """The frame's parameters and locals as Iron sees them: (name, SBValue).

    In a --debug build a C local named after an Iron binding is that
    binding; a name starting with `_` is the compiler's (a temporary, a
    synthetic parameter), except `_ref_<name>`, a pointer to the Iron
    binding <name> (a var a closure captures, or a capture inside the
    closure), shown as the value it points at.
    """
    out = []
    for v in frame.GetVariables(True, True, False, True):
        name = v.GetName() or ""
        if name.startswith("_ref_") and len(name) > 5:
            t = v.GetType().GetPointeeType()
            addr = v.GetValueAsUnsigned(0)
            if addr and t.IsValid():
                out.append((name[5:], v.CreateValueFromAddress(name[5:], addr, t)))
        elif name and not name.startswith("_"):
            out.append((name, v))
    return out


def locals_command(debugger, command, result, _dict):
    """locals: the Iron parameters and locals of the selected frame,
    without the compiler's temporaries (`frame variable` shows them all)."""
    frame = debugger.GetSelectedTarget().GetProcess().GetSelectedThread().GetSelectedFrame()
    if not frame.IsValid():
        result.SetError("no frame selected")
        return
    for _name, v in iron_variables(frame):
        result.AppendMessage(str(v))


def __lldb_init_module(debugger, _dict):
    m = __name__
    run = debugger.HandleCommand
    run('command script add -o -f %s.locals_command locals' % m)
    run('type summary add -F %s.string_summary "Iron_String"' % m)
    run('type synthetic add -x "^Iron_List_" -l %s.ListProvider' % m)
    run('type summary add -e -x "^Iron_List_" -F %s.size_summary' % m)
    run('type synthetic add -x "^Iron_(Map|Set)_" -l %s.SlotProvider' % m)
    run('type summary add -e -x "^Iron_(Map|Set)_" -F %s.size_summary' % m)
    run('type summary add -x "^Iron_Optional_" -F %s.optional_summary' % m)
