"""gdb pretty printers for Iron values in programs built with `iron build --debug`.

Load with `source <iron lib>/debug/iron_gdb.py` (the VS Code launch
configuration in the guide does it through setupCommands).

    String          "hello"
    [T]             [3] = {1, 2, 3}
    Map[K, V]       Map[2] = {["a"] = 1, ["b"] = 2}
    Set[T]          Set[2] = {1, 2}
    T?              the value, or null
"""
import re

import gdb
import gdb.printing


# Break on panic: every Iron panic (a failed assert, an index out of
# bounds, a missing map key, out of memory) prints its message and ends in
# the C library's abort(). With `break abort` (iron debug and iron dap set
# it), a stop there selects the Iron frame that panicked.

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


def _on_stop(event):
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
    if int(heap["flags"]) & 1:
        n = int(heap["byte_length"])
        return heap["data"].string(encoding="utf-8", errors="replace", length=n)
    sso = val["sso"]
    n = int(sso["len"])
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
    return None


gdb.pretty_printers.append(_lookup)
