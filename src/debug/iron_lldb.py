"""LLDB formatters for Iron values in programs built with `iron build --debug`.

Load with `command script import <iron lib>/debug/iron_lldb.py` (the VS
Code launch configurations in the guide do it).

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


# ── Break on panic ──────────────────────────────────────────────────────────
#
# Every Iron panic (a failed assert, an index out of bounds, a missing map
# key, out of memory) prints its message and ends in the C library's
# abort(). `iron-panic-stop` puts a breakpoint there; when it is hit, the
# stop hook selects the Iron frame that panicked.

PANIC_FUNCTIONS = ("abort", "__abort", "raise", "__pthread_kill", "pthread_kill",
                   "__pthread_kill_implementation", "gsignal")


def iron_panic_frame(thread):
    """The index of the Iron frame a panic stopped in, or -1 when the
    thread is not stopped in a panic."""
    names = [(f.GetFunctionName() or "") for f in thread.frames[:4]]
    if not any(n.replace("__GI_", "") in PANIC_FUNCTIONS for n in names):
        return -1
    for i, f in enumerate(thread.frames):
        if (f.GetLineEntry().GetFileSpec().GetFilename() or "").endswith(".iron"):
            return i
    return -1


class PanicStopHook:
    def __init__(self, target, extra_args, _dict):
        self.target = target

    def handle_stop(self, exe_ctx, stream):
        thread = exe_ctx.GetThread()
        i = iron_panic_frame(thread)
        if i >= 0:
            thread.SetSelectedFrame(i)
            e = thread.GetFrameAtIndex(i).GetLineEntry()
            stream.Print("Iron panic at %s:%d (frame #%d is selected)\n"
                         % (e.GetFileSpec().GetFilename(), e.GetLine(), i))
        return True


def panic_stop_command(debugger, command, result, _dict):
    """iron-panic-stop: stop when the program panics, on the Iron line."""
    target = debugger.GetSelectedTarget()
    if not target.IsValid():
        result.SetError("iron-panic-stop: no target")
        return
    bp = target.BreakpointCreateByName("abort")
    bp.AddName("iron-panic")
    debugger.HandleCommand("target stop-hook add -P %s.PanicStopHook" % __name__)


def size_summary(valobj, _dict):
    return "size=%d" % valobj.GetNonSyntheticValue().GetChildMemberWithName(
        "count").GetValueAsUnsigned(0)


def optional_summary(valobj, _dict):
    v = valobj.GetNonSyntheticValue()
    if not v.GetChildMemberWithName("has_value").GetValueAsUnsigned(0):
        return "null"
    inner = v.GetChildMemberWithName("value")
    return inner.GetSummary() or inner.GetValue() or "?"


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand("command script add -o -f %s.panic_stop_command iron-panic-stop"
                           % __name__)
    m = __name__
    run = debugger.HandleCommand
    run('type summary add -F %s.string_summary "Iron_String"' % m)
    run('type synthetic add -x "^Iron_List_" -l %s.ListProvider' % m)
    run('type summary add -e -x "^Iron_List_" -F %s.size_summary' % m)
    run('type synthetic add -x "^Iron_(Map|Set)_" -l %s.SlotProvider' % m)
    run('type summary add -e -x "^Iron_(Map|Set)_" -F %s.size_summary' % m)
    run('type summary add -x "^Iron_Optional_" -F %s.optional_summary' % m)
