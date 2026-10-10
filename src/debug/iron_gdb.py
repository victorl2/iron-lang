"""gdb pretty printers for Iron values in programs built with `iron build --debug`.

Load with `source <iron lib>/debug/iron_gdb.py` (the VS Code launch
configuration in the guide does it through setupCommands).

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
