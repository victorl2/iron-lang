#!/usr/bin/env python3
"""The Iron debug adapter: `iron dap` (#312, #347).

A Debug Adapter Protocol server on stdin/stdout for any DAP client (VS
Code, Neovim's nvim-dap, Zed). It runs lldb-dap, or gdb 14 or later in
its DAP mode, and changes what passes between the client and it:

  launch      `program` may be a .iron file or a package directory: the
              adapter builds it with `iron build --debug` first and
              reports the build's output to the client. The value
              formatters next to this file (iron_lldb.py, iron_gdb.py)
              are loaded into the debugger.
  variables   in a frame's Locals scope, the compiler's temporaries (a C
              name starting with `_`) are hidden, and `_ref_<name>` (a
              var a closure captures, or a capture inside the closure)
              is shown as <name> with its value.
  stackTrace  Iron functions lose their C prefix (Iron_main is main);
              frames outside Iron source (the runtime, the C library)
              are marked subtle, so clients focus the Iron frame.
  initialize  the debugger's C++ / Ada / Objective-C exception filters
              are not offered; conditional breakpoints, hit counts and
              logpoints are.
  evaluate    hover, watch and the debug console take Iron expressions
              (`xs.len() > 2 and not done`, `name == "ann"`, `m.get(k)`),
              computed by the adapter from the values the debugger reads.
  setBreakpoints
              conditions, hit counts and logpoint messages are Iron and
              kept by the adapter, which checks them at each hit and
              continues when one does not hold.

Launch arguments:
  program      .iron file, package directory, or an already built binary
  test         the name of a `test "..."` block in program (a .iron file):
               the file is built with --test and only that test runs
  args, cwd, env, stopOnEntry
  build        false: debug `program` as is (default: build .iron files
               and packages)

Adapter selection: --adapter PATH or $IRON_DAP_ADAPTER, else lldb-dap on
PATH (also lldb-dap-NN and Xcode's), else gdb 14 or later.

Only the Python standard library is used.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
OWN_SEQ_BASE = 1000000000
SYNTH_REF_BASE = 1500000000   # variablesReferences the adapter answers itself
ARRAY_SHOWN = 100
POINTER = re.compile(r"^(\([^)]*\)\s*)?0x[0-9a-fA-F]+$")
# Every Iron panic prints its message and ends in the C library's abort().
PANIC_FUNCTIONS = ("abort", "__abort", "raise", "__pthread_kill", "pthread_kill",
                   "__pthread_kill_implementation", "gsignal")
PANIC_LINE = re.compile(r"^(panic|assertion failed|error: index|.*out of bounds)", re.I)


# ── DAP framing ───────────────────────────────────────────────────────────────

def read_message(stream):
    """One DAP message from a binary stream, or None at end of input."""
    length = None
    while True:
        line = stream.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            if length is not None:
                break
            continue
        m = re.match(rb"Content-Length:\s*(\d+)", line, re.I)
        if m:
            length = int(m.group(1))
    body = b""
    while len(body) < length:
        chunk = stream.read(length - len(body))
        if not chunk:
            return None
        body += chunk
    return json.loads(body.decode("utf-8"))


def frame(msg):
    body = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


# ── Naming rules shared with iron_lldb.py / iron_gdb.py ──────────────────────

def iron_name(c_name):
    """(Iron name, deref) for a C local, or None for a compiler temporary."""
    if c_name.startswith("_ref_") and len(c_name) > 5:
        return c_name[5:], True
    if not c_name or c_name.startswith("_"):
        return None
    return c_name, False


def iron_function_name(c_name):
    m = re.match(r"^Iron_(\w+)(.*)$", c_name)
    if m:
        return m.group(1) + m.group(2)
    m = re.match(r"^__lambda_\d+(.*)$", c_name)
    if m:
        return "func" + m.group(1)
    return c_name


def is_iron_source(path):
    return bool(path) and path.endswith(".iron")


# ── Finding the debugger and building ────────────────────────────────────────

def find_adapter(requested=None):
    """(kind, argv) for the DAP debugger to run, or None."""
    def as_adapter(path):
        if os.path.basename(path).startswith("gdb"):
            return "gdb", [path, "-q", "-i=dap"]
        return "lldb-dap", [path]

    requested = requested or os.environ.get("IRON_DAP_ADAPTER")
    if requested:
        found = shutil.which(requested) or requested
        return as_adapter(found)
    names = ["lldb-dap"] + ["lldb-dap-%d" % v for v in range(25, 17, -1)] + ["lldb-vscode"]
    for n in names:
        p = shutil.which(n)
        if p:
            return as_adapter(p)
    if sys.platform == "darwin":
        try:
            p = subprocess.run(["xcrun", "-f", "lldb-dap"], capture_output=True,
                               text=True).stdout.strip()
            if p and os.path.exists(p):
                return as_adapter(p)
        except OSError:
            pass
    gdb = shutil.which("gdb")
    if gdb:
        try:
            v = subprocess.run([gdb, "--version"], capture_output=True, text=True).stdout
            m = re.search(r"(\d+)\.\d+", v)
            if m and int(m.group(1)) >= 14:  # DAP mode since gdb 14
                return as_adapter(gdb)
        except OSError:
            pass
    return None


def package_dir(start):
    d = os.path.abspath(start)
    while True:
        if os.path.exists(os.path.join(d, "iron.toml")):
            return d
        up = os.path.dirname(d)
        if up == d:
            return None
        d = up


def package_name(d):
    with open(os.path.join(d, "iron.toml"), encoding="utf-8") as f:
        m = re.search(r'^\s*name\s*=\s*"([^"]+)"', f.read(), re.M)
    return m.group(1) if m else None


def build_plan(iron, program, test=False):
    """(argv, cwd, binary) that builds `program` with --debug (and its test
    blocks with `test`), None when `program` is already a binary, or a str
    error."""
    exe = ".exe" if os.name == "nt" else ""
    program = os.path.abspath(program)
    if program.endswith(".iron"):
        out = os.path.join(tempfile.gettempdir(), "iron-debug",
                           os.path.basename(program)[:-5] + ("_test" if test else "") + exe)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        argv = [iron, "build", program, "--debug"] + (["--test"] if test else []) + ["-o", out]
        return argv, os.path.dirname(program), out
    if test:
        return "to debug a test, set \"program\" to the .iron file that declares it"
    if os.path.isdir(program) or os.path.basename(program) == "iron.toml":
        pkg = package_dir(program if os.path.isdir(program) else os.path.dirname(program))
        name = pkg and package_name(pkg)
        if not name:
            return "no iron.toml with a name found for %s" % program
        return [iron, "build", "--debug"], pkg, os.path.join(pkg, "target", name + exe)
    return None


# ── Iron expressions ─────────────────────────────────────────────────────────
#
# The debuggers evaluate C. Hover, watch, the debug console, breakpoint
# conditions and logpoints get Iron text instead: `xs.len() > 2 and not
# done`, `name == "ann"`, `ages.get("ann")`, `xs[i] * 2`. The adapter
# parses the Iron expression, reads the values it names from the debugger
# (each a path such as `p.x` or `xs[1]`, which the debugger resolves through
# the value formatters) and computes the rest itself, with Iron's rules.

class IronEvalError(Exception):
    pass


TOKEN = re.compile(r'\s*(?:(\d+\.\d+)|(\d+)|("(?:[^"\\]|\\.)*")|([A-Za-z_][A-Za-z0-9_]*)|'
                   r'(==|!=|<=|>=|&&|\|\||[-+*/%<>()\[\].,!]))')
BINARY = {"or": 1, "and": 2, "==": 3, "!=": 3, "<": 4, ">": 4, "<=": 4, ">=": 4,
          "+": 5, "-": 5, "*": 6, "/": 6, "%": 6}
IRON_ONLY = re.compile(r'\b(and|or|not)\b|\.\s*(len|is_empty|has|get|get_or|contains)\s*\(|'
                       r'\blen\s*\(|"')


def tokenize(text):
    out, pos = [], 0
    text = text.strip()
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m or m.end() == pos:
            raise IronEvalError("unexpected %r" % text[pos:pos + 10])
        pos = m.end()
        f, i, s, name, op = m.groups()
        if f is not None:
            out.append(("num", float(f)))
        elif i is not None:
            out.append(("num", int(i)))
        elif s is not None:
            out.append(("str", bytes(s[1:-1], "utf-8").decode("unicode_escape")))
        elif name is not None:
            out.append(("name", name))
        else:
            out.append(("op", {"&&": "and", "||": "or", "!": "not"}.get(op, op)))
    return out


class Parser:
    """Iron expression syntax (manual 3.1) for the subset a debugger needs."""

    def __init__(self, text):
        self.toks = tokenize(text)
        self.i = 0

    def peek(self, kind=None, value=None):
        if self.i >= len(self.toks):
            return None
        t = self.toks[self.i]
        if kind and t[0] != kind and not (kind == "op" and t[0] == "name" and t[1] in ("and", "or", "not")):
            return None
        if value is not None and t[1] != value:
            return None
        return t

    def take(self, value=None):
        t = self.peek(value=value)
        if t is None:
            raise IronEvalError("expected %s" % (value or "more"))
        self.i += 1
        return t

    def parse(self):
        e = self.expr(1)
        if self.i != len(self.toks):
            raise IronEvalError("unexpected %r" % (self.toks[self.i][1],))
        return e

    def expr(self, min_prec):
        """Precedence climbing; binary operators are left associative."""
        left = self.unary()
        while True:
            t = self.peek()
            if not t or t[0] not in ("op", "name") or t[1] not in BINARY:
                return left
            prec = BINARY[t[1]]
            if prec < min_prec:
                return left
            self.i += 1
            left = ("bin", t[1], left, self.expr(prec + 1))

    def unary(self):
        t = self.peek()
        if t and t[1] == "not":
            self.i += 1
            return ("not", self.unary())
        if t and t == ("op", "-"):
            self.i += 1
            return ("neg", self.unary())
        return self.postfix(self.primary())

    def primary(self):
        t = self.take()
        if t[0] in ("num", "str"):
            return ("lit", t[1])
        if t[0] == "name":
            if t[1] in ("true", "false"):
                return ("lit", t[1] == "true")
            if t[1] == "null":
                return ("lit", None)
            if self.peek(value="("):        # len(x)
                self.i += 1
                args = self.args()
                return ("call", t[1], args)
            return ("var", t[1])
        if t[1] == "(":
            e = self.expr(0)
            self.take(")")
            return e
        raise IronEvalError("unexpected %r" % (t[1],))

    def args(self):
        out = []
        if self.peek(value=")"):
            self.i += 1
            return out
        while True:
            out.append(self.expr(0))
            if self.peek(value=")"):
                self.i += 1
                return out
            self.take(",")

    def postfix(self, e):
        while True:
            if self.peek(value="."):
                self.i += 1
                name = self.take()
                if name[0] != "name":
                    raise IronEvalError("expected a field or method name")
                if self.peek(value="("):
                    self.i += 1
                    e = ("method", e, name[1], self.args())
                else:
                    e = ("field", e, name[1])
            elif self.peek(value="["):
                self.i += 1
                idx = self.expr(0)
                self.take("]")
                e = ("index", e, idx)
            else:
                return e


class Ref:
    """A value the debugger holds: its path, its text and its children."""

    def __init__(self, path, text, ref):
        self.path, self.text, self.ref = path, text, ref


SIZE = re.compile(r"\bsize=(\d+)|^(?:Map|Set)?\[(\d+)\]$")   # lldb, gdb


class IronEval:
    def __init__(self, proxy, frame_id):
        self.proxy = proxy
        self.frame_id = frame_id

    def fetch(self, path):
        r = self.proxy.request("evaluate", {"expression": path, "frameId": self.frame_id,
                                            "context": "watch"})
        if not r.get("success"):
            if "." in path or "[" in path:
                raise IronEvalError("`%s` has no value here" % path)
            raise IronEvalError("no variable `%s` here" % path)
        b = r.get("body") or {}
        text = str(b.get("result", ""))
        if not text and b.get("variablesReference"):
            text = self.proxy.struct_text("", b["variablesReference"], b.get("type"))
        return self.value(path, text, b.get("variablesReference", 0))

    @staticmethod
    def value(path, text, ref):
        t = text.strip()
        if re.fullmatch(r"-?\d+", t):
            return int(t)
        if re.fullmatch(r"-?\d+\.\d*(e[-+]?\d+)?", t, re.I):
            return float(t)
        if t in ("true", "false"):
            return t == "true"
        if t == "null":
            return None
        if len(t) >= 2 and t[0] == '"' and t[-1] == '"':
            return t[1:-1].replace('\\"', '"')
        return Ref(path, t, ref)

    def children(self, v):
        if not isinstance(v, Ref) or not v.ref:
            return []
        r = self.proxy.request("variables", {"variablesReference": v.ref})
        return (r.get("body") or {}).get("variables", []) if r.get("success") else []

    def ev(self, e):
        k = e[0]
        if k == "lit":
            return e[1]
        if k == "var":
            return self.fetch(e[1])
        if k == "field":
            base = self.ev(e[1])
            if not isinstance(base, Ref):
                raise IronEvalError("`%s` has no fields" % fmt(base))
            return self.fetch("%s.%s" % (base.path, e[2]))
        if k == "index":
            base, idx = self.ev(e[1]), self.ev(e[2])
            if isinstance(base, str) and isinstance(idx, int):
                if not 0 <= idx < len(base):
                    raise IronEvalError("index %d out of range" % idx)
                return base[idx]
            if not isinstance(base, Ref):
                raise IronEvalError("`%s` cannot be indexed" % fmt(base))
            if isinstance(idx, int):
                m = SIZE.search(base.text)
                n = int(m.group(1) or m.group(2)) if m else None
                if n is None and base.text.startswith("["):
                    n = len(self.children(base))     # a list: [1, 2, 3]
                if n is not None and not 0 <= idx < n:
                    raise IronEvalError("index %d is out of range for a list of length %d"
                                        % (idx, n))
                try:
                    return self.fetch("%s[%d]" % (base.path, idx))
                except IronEvalError:
                    # gdb indexes the list struct, not its elements: take
                    # the element from the value printer's children.
                    for c in self.children(base):
                        if c.get("name") == "[%d]" % idx:
                            return self.value("%s[%d]" % (base.path, idx), str(c.get("value", "")),
                                              c.get("variablesReference", 0))
                    raise
            raise IronEvalError("a list index must be an Int")
        if k == "not":
            return not self.truth(self.ev(e[1]))
        if k == "neg":
            v = self.ev(e[1])
            if not isinstance(v, (int, float)) or isinstance(v, bool):
                raise IronEvalError("`-` needs a number")
            return -v
        if k == "bin":
            return self.binary(e[1], e[2], e[3])
        if k == "call":
            if e[1] == "len" and len(e[2]) == 1:
                return self.length(self.ev(e[2][0]))
            raise IronEvalError("`%s(...)` is not available while debugging" % e[1])
        if k == "method":
            return self.method(self.ev(e[1]), e[2], [self.ev(a) for a in e[3]])
        raise IronEvalError("cannot evaluate this expression")

    def truth(self, v):
        if isinstance(v, bool):
            return v
        raise IronEvalError("expected a Bool, got %s" % fmt(v))

    def binary(self, op, le, re_):
        if op in ("and", "or"):
            left = self.truth(self.ev(le))
            if op == "and" and not left:
                return False
            if op == "or" and left:
                return True
            return self.truth(self.ev(re_))
        a, b = self.ev(le), self.ev(re_)
        if op in ("==", "!="):
            if isinstance(a, Ref) or isinstance(b, Ref):
                eq = isinstance(a, Ref) and isinstance(b, Ref) and a.text == b.text
            else:
                eq = a == b and type(a) is type(b) or (a is None and b is None)
            return eq if op == "==" else not eq
        if op == "+" and isinstance(a, str) and isinstance(b, str):
            return a + b
        nums = all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in (a, b))
        strs = isinstance(a, str) and isinstance(b, str)
        if op in ("<", ">", "<=", ">="):
            if not (nums or strs):
                raise IronEvalError("`%s` compares numbers or strings" % op)
            return {"<": a < b, ">": a > b, "<=": a <= b, ">=": a >= b}[op]
        if not nums:
            raise IronEvalError("`%s` needs numbers" % op)
        if op in ("/", "%") and b == 0:
            raise IronEvalError("division by zero")
        if op == "/" and isinstance(a, int) and isinstance(b, int):
            q = abs(a) // abs(b)
            return q if (a >= 0) == (b >= 0) else -q
        if op == "%" and isinstance(a, int) and isinstance(b, int):
            q = abs(a) // abs(b)
            q = q if (a >= 0) == (b >= 0) else -q
            return a - q * b
        return {"+": a + b, "-": a - b, "*": a * b, "/": a / b if b else 0, "%": a % b}[op]

    def length(self, v):
        if isinstance(v, str):
            return len(v)
        if isinstance(v, Ref):
            m = SIZE.search(v.text)
            if m:
                return int(m.group(1) or m.group(2))
            if v.text == "[]":
                return 0
            if POINTER.match(v.text) and re.fullmatch(r"[A-Za-z_]\w*", v.path):
                n = self.fetch(v.path + "_len")      # a list kept as a C array
                if isinstance(n, int):
                    return n
            return len(self.children(v))
        raise IronEvalError("len takes a list, map, set or String")

    def method(self, v, name, args):
        if name in ("len", "count") and not args:
            return self.length(v)
        if name == "is_empty" and not args:
            return self.length(v) == 0
        if name == "to_string" and not args and not isinstance(v, Ref):
            return fmt(v).strip('"') if not isinstance(v, str) else v
        if isinstance(v, Ref) and name in ("has", "get", "get_or", "contains") and args:
            key = fmt(args[0])
            for c in self.children(v):
                cname = str(c.get("name", ""))
                cval = str(c.get("value", ""))
                if cname in ("[%s]" % key, key):               # map entry (lldb, gdb)
                    if name == "has":
                        return True
                    if name in ("get", "get_or"):
                        return self.value("", cval, c.get("variablesReference", 0))
                if name in ("has", "contains") and cval == key:  # set or list element
                    return True
            if name in ("has", "contains"):
                return False
            if name == "get_or" and len(args) == 2:
                return args[1]
            raise IronEvalError("no entry %s" % key)
        if isinstance(v, str) and name in ("contains", "starts_with", "ends_with") and \
                len(args) == 1 and isinstance(args[0], str):
            return {"contains": args[0] in v, "starts_with": v.startswith(args[0]),
                    "ends_with": v.endswith(args[0])}[name]
        raise IronEvalError("`.%s()` is not available while debugging" % name)


def fmt(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if v is None:
        return "null"
    if isinstance(v, str):
        return '"%s"' % v.replace('"', '\\"')
    if isinstance(v, Ref):
        return v.text
    if isinstance(v, float):
        return repr(v)
    return str(v)


def iron_evaluate(proxy, frame_id, text):
    """(result text, variablesReference) of an Iron expression."""
    v = IronEval(proxy, frame_id).ev(Parser(text).parse())
    return fmt(v), (v.ref if isinstance(v, Ref) else 0)


# ── The proxy ────────────────────────────────────────────────────────────────

class Proxy:
    def __init__(self, iron, adapter):
        self.iron = iron
        self.requested_adapter = adapter
        self.kind = None
        self.child = None
        self.out_lock = threading.Lock()
        self.child_lock = threading.Lock()
        self.pending = {}        # client requests by seq
        self.own = {}            # proxy requests: seq -> [event, response]
        self.own_seq = OWN_SEQ_BASE
        self.local_scopes = {}   # variablesReference -> frameId
        self.configured = False  # the client sent configurationDone
        self.held_launch = None  # gdb: launch waits for configurationDone
        self.panic_frames = {}   # threadId -> index of the Iron frame that panicked
        self.panic_text = ""     # the debuggee's last panic message
        self.bp_rules = {}       # (path, line) -> condition / hit count / log message
        self.synth = {}          # SYNTH_REF_BASE + n -> children (a C array shown as a list)
        self.synth_seq = SYNTH_REF_BASE

    # Output to the client.
    def send(self, msg):
        data = frame(msg)
        with self.out_lock:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()

    def output(self, text, category="console"):
        self.send({"seq": 0, "type": "event", "event": "output",
                   "body": {"category": category, "output": text}})

    def respond_error(self, req, message):
        self.send({"seq": 0, "type": "response", "request_seq": req["seq"],
                   "command": req.get("command"), "success": False, "message": message,
                   "body": {"error": {"id": 1, "format": message, "showUser": True}}})

    # Output to the debugger.
    def to_child(self, msg):
        with self.child_lock:
            if self.child and self.child.stdin:
                self.child.stdin.write(frame(msg))
                self.child.stdin.flush()

    def request(self, command, args, timeout=10):
        """A request of the proxy's own; its response never reaches the client."""
        with self.child_lock:
            self.own_seq += 1
            seq = self.own_seq
        slot = [threading.Event(), None]
        self.own[seq] = slot
        self.to_child({"seq": seq, "type": "request", "command": command, "arguments": args})
        slot[0].wait(timeout)
        self.own.pop(seq, None)
        return slot[1] or {"success": False}

    def start_child(self):
        found = find_adapter(self.requested_adapter)
        if not found:
            return ("no debugger found: install lldb-dap (LLVM) or gdb 14 or later, "
                    "or name one with IRON_DAP_ADAPTER")
        self.kind, argv = found
        if self.kind == "gdb":
            fmt = os.path.join(HERE, "iron_gdb.py")
            if os.path.exists(fmt):
                argv = argv + ["-iex", "source %s" % fmt]
        try:
            self.child = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE)
        except OSError as e:
            return "cannot start %s: %s" % (argv[0], e)
        threading.Thread(target=self.child_reader, daemon=True).start()
        threading.Thread(target=self.child_stderr, daemon=True).start()
        return None

    def child_stderr(self):
        for line in self.child.stderr:
            sys.stderr.write(line.decode("utf-8", "replace"))

    def child_reader(self):
        while True:
            msg = read_message(self.child.stdout)
            if msg is None:
                break
            try:
                self.from_child(msg)
            except Exception as e:  # never drop the session over a rewrite
                sys.stderr.write("iron dap: %r\n" % e)
                self.send(msg)
        self.send({"seq": 0, "type": "event", "event": "terminated", "body": {}})

    # Client -> debugger.
    def from_client(self, msg):
        if msg.get("type") != "request":
            self.to_child(msg)
            return
        cmd = msg.get("command")
        if cmd == "initialize" and self.child is None:
            err = self.start_child()
            if err:
                self.respond_error(msg, err)
                return
        if cmd == "evaluate":
            threading.Thread(target=self.handle_evaluate, args=(msg,), daemon=True).start()
            return
        if cmd == "variables":
            ref = (msg.get("arguments") or {}).get("variablesReference")
            if ref in self.synth:
                self.respond(msg, {"variables": self.synth[ref]})
                return
        if cmd == "setBreakpoints":
            msg = self.keep_breakpoint_rules(msg)
        if cmd == "stackTrace":
            a = msg.get("arguments") or {}
            skip = self.panic_frames.get(a.get("threadId"), 0)
            if skip and a.get("startFrame"):
                # A later page of a panic's stack: frames counted from the
                # Iron frame (see from_child).
                msg = dict(msg, arguments=dict(a, startFrame=a["startFrame"] + skip))
        if cmd == "launch":
            msg = self.rewrite_launch(msg)
            if msg is None:
                return
            if self.kind == "gdb" and not self.configured:
                # gdb starts the program on `launch`; clients send their
                # breakpoints after it, so the program would run past
                # them. Launch once the configuration is done.
                self.held_launch = msg
                return
        self.pending[msg["seq"]] = msg
        self.to_child(msg)
        if cmd == "configurationDone":
            self.configured = True
            if self.held_launch is not None:
                held, self.held_launch = self.held_launch, None
                self.pending[held["seq"]] = held
                self.to_child(held)

    def rewrite_launch(self, req):
        a = dict(req.get("arguments") or {})
        program = a.get("program")
        if not program:
            self.respond_error(req, "launch: set \"program\" to a .iron file, a package "
                                    "directory or a binary")
            return None
        binary = program
        test = a.get("test")
        plan = build_plan(self.iron, program, bool(test)) if a.get("build", True) else None
        if isinstance(plan, str):
            self.respond_error(req, plan)
            return None
        if plan:
            argv, cwd, binary = plan
            self.output("$ %s\n" % " ".join(argv))
            r = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
            if r.stdout:
                self.output(r.stdout, "stdout")
            if r.stderr:
                self.output(r.stderr, "stderr")
            if r.returncode != 0:
                self.respond_error(req, "the --debug build failed (see the debug console)")
                return None
            a.setdefault("cwd", cwd)
        args = list(a.get("args", []))
        if test:
            # A test binary lists its tests with --iron-list and runs test
            # n alone with --iron-test n.
            r = subprocess.run([binary, "--iron-list"], capture_output=True, text=True)
            names = r.stdout.splitlines() if r.returncode == 0 else []
            if test not in names:
                self.respond_error(req, "no test \"%s\" in %s (tests: %s)" %
                                   (test, os.path.basename(program),
                                    ", ".join(names) or "none"))
                return None
            args = ["--iron-test", str(names.index(test))] + args
        out = {"program": os.path.abspath(binary), "args": args,
               "cwd": a.get("cwd") or os.path.dirname(os.path.abspath(binary)),
               "stopOnEntry": bool(a.get("stopOnEntry", False))}
        env = a.get("env")
        if self.kind == "lldb-dap":
            if env:
                out["env"] = ["%s=%s" % kv for kv in env.items()]
            init = list(a.get("initCommands", []))
            fmt = os.path.join(HERE, "iron_lldb.py")
            if os.path.exists(fmt):
                init.insert(0, 'command script import "%s"' % fmt)
            out["initCommands"] = init
            for k in ("preRunCommands", "stopCommands", "exitCommands"):
                if k in a:
                    out[k] = a[k]
            if a.get("stopOnPanic", True) and os.path.exists(fmt):
                out["preRunCommands"] = list(out.get("preRunCommands", [])) + ["iron-panic-stop"]
        else:
            if env:
                out["env"] = env
            if a.get("stopOnPanic", True):
                # Pending: abort() is in the C library, loaded with the program.
                self.request("evaluate", {"expression": "set breakpoint pending on",
                                          "context": "repl"})
                self.request("evaluate", {"expression": "break abort", "context": "repl"})
        if "__restart" in a:
            out["__restart"] = a["__restart"]
        return dict(req, arguments=out)

    # Debugger -> client.
    def from_child(self, msg):
        if msg.get("type") == "response" and msg.get("request_seq", 0) > OWN_SEQ_BASE:
            slot = self.own.get(msg["request_seq"])
            if slot:
                slot[1] = msg
                slot[0].set()
            return
        if msg.get("type") != "response":
            ev = msg.get("event") if msg.get("type") == "event" else None
            if ev in ("stopped", "continued"):
                self.local_scopes.clear()
                self.synth.clear()
                self.panic_frames.clear()
            if ev == "output":
                self.note_output(msg)
            if ev == "stopped":
                # Telling a panic apart takes a request of our own: finish
                # on another thread.
                threading.Thread(target=self.finish_stopped, args=(msg,), daemon=True).start()
                return
            self.send(msg)
            return
        req = self.pending.pop(msg.get("request_seq"), None)
        if not req or not msg.get("success"):
            self.send(msg)
            return
        cmd = req.get("command")
        body = msg.get("body") or {}
        if cmd == "initialize":
            # The debugger's exception filters (C++ throw, Ada, Objective-C)
            # do not apply to Iron programs.
            body["exceptionBreakpointFilters"] = []
            body.update(supportsConditionalBreakpoints=True,
                        supportsHitConditionalBreakpoints=True,
                        supportsLogPoints=True, supportsEvaluateForHovers=True)
            msg["body"] = body
            self.send(msg)
        elif cmd == "setBreakpoints":
            # A breakpoint the debugger moved keeps its rule at the new line.
            args = req.get("arguments") or {}
            path = os.path.realpath((args.get("source") or {}).get("path") or "")
            for a, b in zip(args.get("breakpoints") or [], body.get("breakpoints", [])):
                rule = self.bp_rules.get((path, a.get("line")))
                if rule and b.get("line") and b["line"] != a.get("line"):
                    self.bp_rules[(path, b["line"])] = rule
            self.send(msg)
        elif cmd == "scopes":
            for s in body.get("scopes", []):
                if re.match(r"^(locals|arguments)$", s.get("name", ""), re.I) and \
                        s.get("variablesReference"):
                    self.local_scopes[s["variablesReference"]] = \
                        (req.get("arguments") or {}).get("frameId")
            self.send(msg)
        elif cmd == "variables":
            ref = (req.get("arguments") or {}).get("variablesReference")
            if ref in self.local_scopes:
                # Resolving `_ref_` values takes requests of our own, whose
                # responses arrive on this thread: finish on another.
                frame_id = self.local_scopes[ref]
                threading.Thread(target=self.finish_variables, args=(msg, frame_id),
                                 daemon=True).start()
            else:
                self.send(msg)
        elif cmd == "stackTrace":
            # Stopped in a panic: the stack starts at the Iron frame that
            # panicked, so every editor shows that line.
            args = req.get("arguments") or {}
            skip = self.panic_frames.get(args.get("threadId"), 0)
            if skip and not args.get("startFrame"):
                frames = body.get("stackFrames", [])[skip:]
                body["stackFrames"] = frames
                if isinstance(body.get("totalFrames"), int):
                    body["totalFrames"] = max(len(frames), body["totalFrames"] - skip)
            for f in body.get("stackFrames", []):
                name = f.get("name")
                if isinstance(name, str):
                    f["name"] = iron_function_name(name)
                src = f.get("source") or {}
                if not is_iron_source(src.get("path") or src.get("name")):
                    f["presentationHint"] = "subtle"
                    if src:
                        src["presentationHint"] = "deemphasize"
            self.send(msg)
        else:
            self.send(msg)

    def note_output(self, msg):
        """Remember the debuggee's last panic message."""
        body = msg.get("body") or {}
        if body.get("category") not in ("stdout", "stderr", None):
            return
        for line in str(body.get("output", "")).splitlines():
            if PANIC_LINE.search(line):
                self.panic_text = line.strip()

    def finish_stopped(self, msg):
        body = msg.get("body") or {}
        tid = body.get("threadId")
        if tid is not None and body.get("reason") in ("breakpoint", "signal", "exception",
                                                       "function breakpoint", None):
            r = self.request("stackTrace", {"threadId": tid, "startFrame": 0, "levels": 40})
            frames = (r.get("body") or {}).get("stackFrames", []) if r.get("success") else []
            top = [str(f.get("name", "")).split("(")[0].strip() for f in frames[:4]]
            if body.get("reason") == "breakpoint" and frames and \
                    not self.breakpoint_holds(frames[0], tid):
                return
            if any(n.replace("__GI_", "") in PANIC_FUNCTIONS for n in top):
                for i, f in enumerate(frames):
                    src = f.get("source") or {}
                    if is_iron_source(src.get("path") or src.get("name")):
                        self.panic_frames[tid] = i
                        msg = dict(msg, body=dict(body, reason="exception", description="Panic",
                                                  text=self.panic_text or "Iron panic"))
                        break
        self.send(msg)

    # ── Iron expressions in evaluate and breakpoints ───────────────────────

    def respond(self, req, body):
        self.send({"seq": 0, "type": "response", "request_seq": req["seq"],
                   "command": req.get("command"), "success": True, "body": body})

    def handle_evaluate(self, req):
        """Iron syntax is computed here; anything else goes to the
        debugger first (a raw debugger command starts with a backquote),
        and what it cannot evaluate is tried as Iron."""
        a = req.get("arguments") or {}
        text = str(a.get("expression", ""))
        frame_id = a.get("frameId")
        raw = text.startswith("`")
        iron_first = frame_id is not None and not raw and bool(IRON_ONLY.search(text))
        debugger_error = "cannot evaluate"
        if not iron_first:
            r = self.request("evaluate", a, timeout=30)
            if r.get("success") or frame_id is None or raw:
                b = r.get("body") or {}
                if r.get("success") and not b.get("result") and b.get("variablesReference"):
                    b = dict(b, result=self.struct_text("", b["variablesReference"], b.get("type")))
                    r = dict(r, body=b)
                if r.get("success") and frame_id is not None and \
                        re.fullmatch(r"[A-Za-z_]\w*", text) and POINTER.match(str(b.get("result", ""))):
                    # A list kept as a C array: show its elements.
                    n = self.request("evaluate", {"expression": text + "_len", "frameId": frame_id,
                                                  "context": "watch"})
                    count = str((n.get("body") or {}).get("result", "")) if n.get("success") else ""
                    if re.fullmatch(r"\d+", count):
                        shown, ref, _ = self.array_view(text, int(count), frame_id)
                        r = dict(r, body=dict(b, result=shown, variablesReference=ref))
                self.send(dict(r, seq=0, request_seq=req["seq"], command="evaluate"))
                return
            debugger_error = r.get("message") or debugger_error
        try:
            tree = Parser(text).parse()
        except IronEvalError as e:
            # Not Iron (a debugger command typed in the console, say): the
            # debugger's own message says more.
            self.respond_error(req, str(e) if iron_first else debugger_error)
            return
        try:
            v = IronEval(self, frame_id).ev(tree)
        except IronEvalError as e:
            self.respond_error(req, str(e))
            return
        self.respond(req, {"result": fmt(v), "variablesReference": v.ref if isinstance(v, Ref) else 0})

    def keep_breakpoint_rules(self, req):
        """Iron conditions, hit counts and log messages stay here; the
        debugger gets plain breakpoints."""
        a = dict(req.get("arguments") or {})
        path = os.path.realpath((a.get("source") or {}).get("path") or "")
        for key in [k for k in self.bp_rules if k[0] == path]:
            del self.bp_rules[key]
        plain = []
        for bp in a.get("breakpoints") or []:
            rule = {k: bp[k] for k in ("condition", "hitCondition", "logMessage") if bp.get(k)}
            if rule:
                rule["hits"] = 0
                self.bp_rules[(path, bp.get("line"))] = rule
            plain.append({k: v for k, v in bp.items()
                          if k not in ("condition", "hitCondition", "logMessage")})
        a["breakpoints"] = plain
        return dict(req, arguments=a)

    def breakpoint_holds(self, top, tid):
        """False when the breakpoint at `top` has a rule that says go on:
        the program is continued and the stop never reaches the client."""
        src = top.get("source") or {}
        rule = self.bp_rules.get((os.path.realpath(src.get("path") or ""), top.get("line")))
        if not rule:
            return True
        rule["hits"] += 1
        hit = str(rule.get("hitCondition", "")).strip()
        if hit:
            m = re.fullmatch(r"(==|>=|>|<=|<|%)?\s*(\d+)", hit)
            if not m:
                self.output("iron dap: hit count `%s`: write N, >= N or %% N\n" % hit)
                return True
            op, n, h = m.group(1) or "==", int(m.group(2)), rule["hits"]
            ok = {"==": h == n, ">=": h >= n, ">": h > n, "<=": h <= n, "<": h < n,
                  "%": n > 0 and h % n == 0}[op]
            if not ok:
                return self.go_on(tid)
        cond = rule.get("condition")
        if cond:
            try:
                v = IronEval(self, top.get("id")).ev(Parser(cond).parse())
            except IronEvalError as e:
                self.output("iron dap: breakpoint condition `%s`: %s\n" % (cond, e))
                return True
            if v is not True:
                if v is not False:
                    self.output("iron dap: breakpoint condition `%s` is not a Bool\n" % cond)
                    return True
                return self.go_on(tid)
        log = rule.get("logMessage")
        if log:
            self.output(self.interpolate(log, top.get("id")) + "\n")
            return self.go_on(tid)
        return True

    def interpolate(self, text, frame_id):
        """A logpoint message: `{expr}` is replaced by its value, as in an
        Iron string; `{{` and `}}` are braces."""
        out, i = [], 0
        while i < len(text):
            if text.startswith("{{", i) or text.startswith("}}", i):
                out.append(text[i])
                i += 2
            elif text[i] == "{":
                j = text.find("}", i)
                if j < 0:
                    out.append(text[i:])
                    break
                try:
                    v = IronEval(self, frame_id).ev(Parser(text[i + 1:j]).parse())
                    out.append(v if isinstance(v, str) else fmt(v))
                except IronEvalError as e:
                    out.append("<%s>" % e)
                i = j + 1
            else:
                out.append(text[i])
                i += 1
        return "".join(out)

    def go_on(self, tid):
        self.request("continue", {"threadId": tid}, timeout=30)
        return False

    def finish_variables(self, msg, frame_id):
        out = []
        for v in (msg.get("body") or {}).get("variables", []):
            n = iron_name(str(v.get("name", "")))
            if n is None:
                continue
            name, deref = n
            if not deref:
                out.append(v)
                continue
            expr = "*" + v["name"]
            r = self.request("evaluate", {"expression": expr, "frameId": frame_id,
                                          "context": "watch"})
            if not r.get("success"):
                continue
            b = r.get("body") or {}
            item = {"name": name, "value": b.get("result", ""),
                    "variablesReference": b.get("variablesReference", 0),
                    "evaluateName": expr}
            for k in ("type", "namedVariables", "indexedVariables"):
                if k in b:
                    item[k] = b[k]
            out.append(item)
        out = self.join_array_lengths(out, frame_id)
        msg = dict(msg)
        msg["body"] = dict(msg.get("body") or {}, variables=out)
        self.send(msg)

    def join_array_lengths(self, variables, frame_id):
        """A list the compiler keeps as a C array (a list literal never
        changed, a list parameter) is a pointer `xs` and a length `xs_len`:
        show `xs` as its elements and drop `xs_len`."""
        by_name = {v.get("name"): v for v in variables}
        out = []
        for v in variables:
            if not v.get("value") and v.get("variablesReference"):
                v = dict(v, value=self.struct_text("", v["variablesReference"], v.get("type")))
            name = str(v.get("name", ""))
            if name.endswith("_len") and POINTER.match(str(by_name.get(name[:-4], {}).get("value", ""))):
                continue
            n_var = by_name.get(name + "_len")
            if n_var and POINTER.match(str(v.get("value", ""))) and \
                    re.fullmatch(r"\d+", str(n_var.get("value", ""))):
                text, ref, count = self.array_view(name, int(n_var["value"]), frame_id)
                v = dict(v, value=text, variablesReference=ref, indexedVariables=count)
            out.append(v)
        return out

    def array_view(self, name, n, frame_id):
        """(text, variablesReference, children) of the C array `name` of n
        elements, shown as a list: [1, 2, 3]."""
        elems = []
        for i in range(min(max(n, 0), ARRAY_SHOWN)):
            r = self.request("evaluate", {"expression": "%s[%d]" % (name, i),
                                          "frameId": frame_id, "context": "watch"})
            b = (r.get("body") or {}) if r.get("success") else {}
            elems.append({"name": "[%d]" % i, "value": str(b.get("result", "?")),
                          "variablesReference": b.get("variablesReference", 0),
                          "evaluateName": "%s[%d]" % (name, i)})
        shown = ", ".join(e["value"] for e in elems[:20])
        text = "[%s%s]" % (shown, ", ..." if n > 20 else "")
        return text, (self.synthetic(elems) if elems else 0), len(elems)

    def struct_text(self, value, ref, type_name):
        """gdb shows a struct without a printer as an empty value: write it
        as LLDB's object summary does, Point {x = 3, y = 4}."""
        if value or not ref:
            return value
        r = self.request("variables", {"variablesReference": ref})
        kids = (r.get("body") or {}).get("variables", []) if r.get("success") else []
        if not kids:
            return value
        parts = ["%s = %s" % (k.get("name"), k.get("value") or "{...}") for k in kids[:8]]
        name = re.sub(r"^(struct\s+)?Iron_", "", str(type_name or "").strip())
        body = "{%s%s}" % (", ".join(parts), ", ..." if len(kids) > 8 else "")
        return "%s %s" % (name, body) if re.fullmatch(r"[A-Z]\w*", name) else body

    def synthetic(self, children):
        """A variablesReference the adapter answers itself (until the next stop)."""
        self.synth_seq += 1
        self.synth[self.synth_seq] = children
        return self.synth_seq

    def run(self):
        while True:
            msg = read_message(sys.stdin.buffer)
            if msg is None:
                break
            self.from_client(msg)
        if self.child:
            try:
                self.child.terminate()
            except OSError:
                pass


def main(argv):
    iron = "iron"
    adapter = None
    i = 1
    while i < len(argv):
        if argv[i] == "--iron" and i + 1 < len(argv):
            iron = argv[i + 1]
            i += 2
        elif argv[i] == "--adapter" and i + 1 < len(argv):
            adapter = argv[i + 1]
            i += 2
        elif argv[i] in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            sys.stderr.write("iron dap: unknown argument %s\n" % argv[i])
            return 2
    Proxy(iron, adapter).run()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
