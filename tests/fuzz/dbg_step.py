"""Random call graphs for the stepping area of dbg_fuzz.py.

A program is written twice from one tree: the program the debugger runs,
and a trace twin with the same statements and a `println` before each
one naming its line in the first (`@L<line>`), plus `@F<function>` when a
function starts and `@R` when it returns (a `defer` registered first, so
it runs after the function's own deferred statements). Running the twin
gives the sequence of Iron lines and call depths the program goes
through; Step In / Over / Out must follow it.

Events the debugger may or may not stop on are written `@O<line>`: a
loop's header between iterations, the closing brace of a function.
"""
import re

INDENT = "    "


class S:
    """A statement: kind, text (for simple ones), children (blocks)."""

    def __init__(self, kind, text=None, body=None, orelse=None, extra=None):
        self.kind, self.text, self.body, self.orelse, self.extra = kind, text, body, orelse, extra


class Func:
    def __init__(self, name, shown, header, body, ret=None, kind="func"):
        self.name, self.shown, self.header, self.body, self.ret = name, shown, header, body, ret
        self.kind = kind


class StepProgram:
    def __init__(self, rng):
        self.r = rng
        self.funcs = []          # Func, in call order constraints: fK calls only fJ, J > K
        self.nfuncs = 0
        self.depth_budget = 0
        self.loop_lines = set()

    # ── generation ──
    def expr(self, fidx, var):
        """An Int expression that may call functions with a larger index,
        the method, the generic function, a closure or the recursion."""
        r = self.r
        c = r.random()
        later = [f for f in self.funcs_plan if f > fidx]
        if c < 0.3 and later:
            return "fn%d(%s)" % (r.choice(later), var)
        if c < 0.4:
            return "counter.add(%s %% 7)" % var if self.has_counter.get(fidx) else "%s + 1" % var
        if c < 0.5:
            return "gid(%s)" % var
        if c < 0.58:
            return "rec(%d)" % r.randint(0, 3)
        if c < 0.66 and self.has_lambda.get(fidx):
            return "twice(%s)" % var
        if c < 0.7 and later:
            return "fn%d(%s) + fn%d(1)" % (r.choice(later), var, r.choice(later))
        return "%s %s %d" % (var, r.choice(["+", "-", "*"]), r.randint(1, 9))

    def block(self, fidx, depth, n):
        r = self.r
        out = []
        for _ in range(n):
            c = r.random()
            if c < 0.45 or depth > 1:
                out.append(S("simple", "x = %s" % self.expr(fidx, "x")))
            elif c < 0.55:
                out.append(S("simple", 'println("p{x}")'))
            elif c < 0.7:
                out.append(S("if", "x %% %d == 0" % r.randint(2, 3),
                             self.block(fidx, depth + 1, r.randint(1, 2)),
                             self.block(fidx, depth + 1, r.randint(0, 2))))
            elif c < 0.85:
                out.append(S("for", "i%d" % depth, self.block(fidx, depth + 1, r.randint(1, 2)),
                             extra=r.randint(1, 3)))
            else:
                out.append(S("defer", None, [S("simple", "x = side(x)")]))
        return out

    def generate(self):
        r = self.r
        n = r.randint(1, 4)
        self.funcs_plan = list(range(n))
        self.has_counter = {k: r.random() < 0.5 for k in range(-1, n)}
        self.has_lambda = {k: r.random() < 0.5 for k in range(-1, n)}
        bodies = {}
        for k in range(n):
            pre = []
            if self.has_counter[k]:
                pre.append(S("simple", "var counter = Counter(%d)" % r.randint(0, 5)))
            if self.has_lambda[k]:
                pre.append(S("lambda"))
            bodies[k] = [S("simple", "var x = n")] + pre + self.block(k, 0, r.randint(2, 5))
        main_pre = [S("simple", "var x = %d" % r.randint(0, 9))]
        if self.has_counter[-1]:
            main_pre.append(S("simple", "var counter = Counter(1)"))
        if self.has_lambda[-1]:
            main_pre.append(S("lambda"))
        self.main = main_pre + self.block(-1, 0, r.randint(3, 6))
        self.bodies = bodies
        self.side_body = [S("simple", "val y = v + 1")]

    # ── rendering ──
    def render(self, trace):
        """The program, or with trace=True its trace twin. Returns text;
        sets self.lines (line of each statement) when not tracing."""
        out = []
        line_of = {}           # id(stmt) -> line in the program
        if trace:
            line_of = self.line_of

        def emit(text):
            out.append(text)
            return len(out)

        def T(ind, text):
            if trace:
                emit(ind + text)

        def stmts(ss, ind):
            for s in ss:
                stmt(s, ind)

        def stmt(s, ind):
            if s.kind == "simple":
                T(ind, 'println("@L%d")' % line_of.get(id(s), 0))
                l = emit(ind + s.text)
                if not trace:
                    line_of[id(s)] = l
            elif s.kind == "lambda":
                T(ind, 'println("@L%d")' % line_of.get(id(s), 0))
                l = emit(ind + "val twice = func(a: Int) -> Int {")
                if not trace:
                    line_of[id(s)] = l
                T(ind + INDENT, 'println("@Ffunc")')
                T(ind + INDENT, 'defer println("@R")')
                T(ind + INDENT, 'defer println("@O%d")' % (line_of.get(id(s), 0) + 3))
                T(ind + INDENT, 'println("@L%d")' % (line_of.get(id(s), 0) + 1))
                emit(ind + INDENT + "val b = a * 2")
                T(ind + INDENT, 'println("@L%d")' % (line_of.get(id(s), 0) + 2))
                emit(ind + INDENT + "return b + 1")
                emit(ind + "}")
            elif s.kind == "if":
                T(ind, 'println("@L%d")' % line_of.get(id(s), 0))
                l = emit(ind + "if %s {" % s.text)
                if not trace:
                    line_of[id(s)] = l
                stmts(s.body, ind + INDENT)
                if s.orelse:
                    emit(ind + "} else {")
                    stmts(s.orelse, ind + INDENT)
                emit(ind + "}")
            elif s.kind == "for":
                T(ind, 'println("@L%d")' % line_of.get(id(s), 0))
                l = emit(ind + "for %s in range(%d) {" % (s.text, s.extra))
                if not trace:
                    line_of[id(s)] = l
                    self.loop_lines.add(l)
                # the header's test after each pass, after the pass's own
                # deferred statements
                T(ind + INDENT, 'defer println("@O%d")' % line_of.get(id(s), 0))
                stmts(s.body, ind + INDENT)
                emit(ind + "}")
            elif s.kind == "defer":
                l = emit(ind + "defer {")
                if not trace:
                    line_of[id(s)] = l
                stmts(s.body, ind + INDENT)
                emit(ind + "}")

        def func(header, shown, body, ret, closing=True):
            emit(header)
            T(INDENT, 'println("@F%s")' % shown)
            T(INDENT, 'defer println("@R")')
            # the return, on the closing brace, after the function's own
            # deferred statements
            T(INDENT, 'defer println("@O%d")' % line_of.get(("end", header), 0))
            stmts(body, INDENT)
            if ret:
                T(INDENT, 'println("@L%d")' % line_of.get(("ret", header), 0))
                l = emit(INDENT + ret)
                if not trace:
                    line_of[("ret", header)] = l
            l = emit("}")
            if not trace:
                line_of[("end", header)] = l
            emit("")

        # helpers
        emit("object Counter {")
        emit("    var n: Int")
        emit("    init(n: Int) {")
        T("        ", 'println("@FCounter.init")')
        T("        ", 'defer println("@R")')
        T("        ", 'defer println("@O%d")' % line_of.get("init_end", 0))
        T("        ", 'println("@L%d")' % line_of.get("init1", 0))
        l = emit("        self.n = n")
        if not trace:
            line_of["init1"] = l
        l = emit("    }")
        if not trace:
            line_of["init_end"] = l
        # (the method is written by hand: it needs the object's indentation)
        emit("    func add(k: Int) -> Int {")
        T("        ", 'println("@FCounter.add")')
        T("        ", 'defer println("@R")')
        T("        ", 'defer println("@O%d")' % line_of.get("add_end", 0))
        T("        ", 'println("@L%d")' % line_of.get("add1", 0))
        l = emit("        self.n = self.n + k")
        if not trace:
            line_of["add1"] = l
        T("        ", 'println("@L%d")' % line_of.get("add2", 0))
        l = emit("        return self.n")
        if not trace:
            line_of["add2"] = l
        l = emit("    }")
        if not trace:
            line_of["add_end"] = l
        emit("}")
        emit("")
        func("func gid[T](v: T) -> T {", "gid[Int]", [], "return v")
        emit("func rec(n: Int) -> Int {")
        T(INDENT, 'println("@Frec")')
        T(INDENT, 'defer println("@R")')
        T(INDENT, 'defer println("@O%d")' % line_of.get("rec_end", 0))
        T(INDENT, 'println("@L%d")' % line_of.get("rec1", 0))
        l = emit(INDENT + "if n <= 0 {")
        if not trace:
            line_of["rec1"] = l
        T(INDENT * 2, 'println("@L%d")' % line_of.get("rec2", 0))
        l = emit(INDENT * 2 + "return 0")
        if not trace:
            line_of["rec2"] = l
        emit(INDENT + "}")
        T(INDENT, 'println("@L%d")' % line_of.get("rec3", 0))
        l = emit(INDENT + "return rec(n - 1) + 1")
        if not trace:
            line_of["rec3"] = l
        l = emit("}")
        if not trace:
            line_of["rec_end"] = l
        emit("")
        func("func side(v: Int) -> Int {", "side", self.side_body, "return y")
        for k in sorted(self.bodies, reverse=True):
            func("func fn%d(n: Int) -> Int {" % k, "fn%d" % k, self.bodies[k], "return x")
        func("func main() {", "main", self.main, None)
        if not trace:
            self.line_of = line_of
        return "\n".join(out) + "\n"


def parse_trace(stdout):
    """[(line, function, depth, optional)] from the trace twin's output."""
    events, stack, ids, n = [], [], [], 0
    for l in stdout.splitlines():
        if l.startswith("@F"):
            n += 1
            stack.append(l[2:])
            ids.append(n)
        elif l.startswith("@R"):
            if stack:
                stack.pop()
                ids.pop()
        elif l.startswith("@L") or l.startswith("@O"):
            events.append((int(l[2:]), stack[-1] if stack else "?", len(stack),
                           l.startswith("@O"), tuple(stack), tuple(ids)))
    return events


def step_program(rng):
    g = StepProgram(rng)
    g.generate()
    text = g.render(False)
    twin = g.render(True)
    return g, text, twin


def first_line(g):
    """The line of main's first statement."""
    return g.line_of[id(g.main[0])]
