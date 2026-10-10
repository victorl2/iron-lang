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
              are not offered.

Launch arguments:
  program      .iron file, package directory, or an already built binary
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


def build_plan(iron, program):
    """(argv, cwd, binary) that builds `program` with --debug, None when
    `program` is already a binary, or a str error."""
    exe = ".exe" if os.name == "nt" else ""
    program = os.path.abspath(program)
    if program.endswith(".iron"):
        out = os.path.join(tempfile.gettempdir(), "iron-debug",
                           os.path.basename(program)[:-5] + exe)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        return [iron, "build", program, "--debug", "-o", out], os.path.dirname(program), out
    if os.path.isdir(program) or os.path.basename(program) == "iron.toml":
        pkg = package_dir(program if os.path.isdir(program) else os.path.dirname(program))
        name = pkg and package_name(pkg)
        if not name:
            return "no iron.toml with a name found for %s" % program
        return [iron, "build", "--debug"], pkg, os.path.join(pkg, "target", name + exe)
    return None


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
        plan = build_plan(self.iron, program) if a.get("build", True) else None
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
        out = {"program": os.path.abspath(binary), "args": a.get("args", []),
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
            msg["body"] = body
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
            if any(n.replace("__GI_", "") in PANIC_FUNCTIONS for n in top):
                for i, f in enumerate(frames):
                    src = f.get("source") or {}
                    if is_iron_source(src.get("path") or src.get("name")):
                        self.panic_frames[tid] = i
                        msg = dict(msg, body=dict(body, reason="exception", description="Panic",
                                                  text=self.panic_text or "Iron panic"))
                        break
        self.send(msg)

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
        msg = dict(msg)
        msg["body"] = dict(msg.get("body") or {}, variables=out)
        self.send(msg)

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
