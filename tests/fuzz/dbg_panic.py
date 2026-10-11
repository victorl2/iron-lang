"""Random panics for the panic area of dbg_fuzz.py.

A program calls a chain of functions (methods, closures, a spawned task)
down to one statement that panics. Under `iron dap` the program must stop
on that statement's line, in that function, reported as an exception with
the panic's message, and continuing must end the program.
"""

HEADER = '''object P {
    var hp: Int
    init(hp: Int) {
        self.hp = hp
    }
}

func esc_closure() -> func() -> Int {
    val local = P(41)
    val p = &local
    return func() -> Int {
        return p.hp
    }
}
'''

ESC_LINE = 12      # `return p.hp` in esc_closure's lambda


class PanicProgram:
    def __init__(self, rng):
        self.r = rng
        self.lines = HEADER.rstrip("\n").split("\n") + [""]
        self.expect = None      # (function shown, line, message)

    def emit(self, text):
        self.lines.append(text)
        return len(self.lines)

    def filler(self, ind, var):
        r = self.r
        for _ in range(r.randint(0, 3)):
            c = r.random()
            if c < 0.5:
                self.emit(ind + "%s = %s + %d" % (var, var, r.randint(1, 9)))
            else:
                self.emit(ind + 'println("at {%s}")' % var)

    def panic_stmt(self, ind, kind):
        """Writes the panicking statement (after its setup); returns
        (line, message, function override or None)."""
        r = self.r
        e = self.emit
        if kind == "bounds":
            n = r.randint(1, 4)
            e(ind + "val xs = [%s]" % ", ".join(str(r.randint(0, 9)) for _ in range(n)))
            e(ind + "var k = %d" % (n - 1))
            k = n - 1 + r.randint(1, 4)
            e(ind + "k = k + %d" % (k - (n - 1)))
            return e(ind + "val bad = xs[k]"), "index out of bounds (index %d, bound %d)" % (k, n), None
        if kind == "null":
            e(ind + "val b: Box[P] = Box.null()")
            return e(ind + "val v = b.unwrap()"), "unwrap() on null Box", None
        if kind == "divzero":
            e(ind + "var d = %d" % r.randint(1, 5))
            e(ind + "d = d - d")
            return e(ind + "val q = 100 / d"), "integer division by zero", None
        if kind == "modzero":
            e(ind + "var d = 0")
            return e(ind + "val q = 7 % d"), "integer division by zero", None
        if kind == "key":
            e(ind + "var m = Map[String, Int]()")
            e(ind + 'm.put("a", 1)')
            return e(ind + 'val g = m.get("zz")'), "key not found in map", None
        if kind == "assert":
            v = r.randint(0, 99)
            e(ind + "val w = %d" % v)
            return e(ind + 'assert(w > 100, "w is {w}")'), "assertion failed: w is %d" % v, None
        if kind == "pop":
            e(ind + "var st = [7]")
            e(ind + "val a1 = st.pop()")
            return e(ind + "val a2 = st.pop()"), "pop from an empty list", None
        if kind == "shift":
            e(ind + "var s = 0 - %d" % r.randint(1, 3))
            l = e(ind + "val sh = 1 << s")
            self.shift = True
            return l, "shift by a negative count", None
        if kind == "stale":
            e(ind + "val h = heap P(5)")
            e(ind + "val hp: *P = &h")
            e(ind + "free h")
            return e(ind + "val z = hp.hp"), "stale pointer dereference", None
        if kind == "dangling":
            e(ind + "val f = esc_closure()")
            e(ind + "val got = f()")
            return ESC_LINE, "dangling stack pointer to frame", "func"
        raise ValueError(kind)

    KINDS = ["bounds", "null", "divzero", "modzero", "key", "assert", "pop", "shift", "stale",
             "dangling"]

    def generate(self):
        r = self.r
        kind = r.choice(self.KINDS)
        self.kind = kind
        depth = r.randint(0, 3)
        where = r.choice(["func", "func", "method", "closure", "spawn"])
        self.where = where
        # the innermost function first
        if where == "method":
            self.lines[1:1] = []   # (P is already declared)
            fname, shown = "Worker", "Worker.run"
            self.emit("object Worker {")
            self.emit("    var n: Int")
            self.emit("    init(n: Int) {")
            self.emit("        self.n = n")
            self.emit("    }")
            self.emit("    func run(n: Int) -> Int {")
            self.emit("        var x = n")
            self.filler("        ", "x")
            line, msg, fn = self.panic_stmt("        ", kind)
            self.emit("        return x")
            self.emit("    }")
            self.emit("}")
            self.emit("")
            inner_call = "Worker(1).run(%s)"
        else:
            shown = "inner"
            self.emit("func inner(n: Int) -> Int {")
            self.emit("    var x = n")
            self.filler("    ", "x")
            if where == "closure":
                self.emit("    val go = func(a: Int) -> Int {")
                self.emit("        var x = a")
                line, msg, fn = self.panic_stmt("        ", kind)
                self.emit("        return x")
                self.emit("    }")
                self.emit("    x = go(x)")
                shown = "func"
            else:
                line, msg, fn = self.panic_stmt("    ", kind)
            self.emit("    return x")
            self.emit("}")
            self.emit("")
            inner_call = "inner(%s)"
        callee = inner_call
        for k in range(depth):
            self.emit("func mid%d(n: Int) -> Int {" % k)
            self.emit("    var x = n")
            self.filler("    ", "x")
            self.emit("    x = " + (callee % "x"))
            self.emit("    return x")
            self.emit("}")
            self.emit("")
            callee = "mid%d(%%s)" % k
        self.emit("func main() {")
        self.emit("    var x = %d" % r.randint(0, 9))
        self.filler("    ", "x")
        if where == "spawn":
            self.emit('    val h = spawn("worker") {')
            self.emit("        return %s" % (callee % "3"))
            self.emit("    }")
            self.emit("    x = x + await h")
        else:
            self.emit("    x = " + (callee % "x"))
        self.emit('    println("end {x}")')
        self.emit("}")
        self.expect = (fn or shown, line, msg)
        return "\n".join(self.lines) + "\n"


def panic_program(rng):
    g = PanicProgram(rng)
    text = g.generate()
    return g, text
