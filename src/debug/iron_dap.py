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
PATH (also lldb-dap-NN, lldb-vscode(-NN), Homebrew's LLVM and Xcode's, and
on Windows LLVM's installer in Program Files), else gdb 14 or later. On
Windows, LLDB needs Python 3.10 or later, which it finds through the
Python running this adapter. `--check` reports what it finds, and how to
install a debugger when there is none, and exits 1 then.

Only the Python standard library is used.
"""
import json
import math
import os
import queue
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OWN_SEQ_BASE = 1000000000
SYNTH_REF_BASE = 1500000000   # variablesReferences the adapter answers itself
ARRAY_SHOWN = 100
POINTER = re.compile(r"^(\([^)]*\)\s*)?0x[0-9a-fA-F]+$")


def is_c_array_value(value, type_name=None):
    """A list kept as a C array is a pointer (`xs`, with `xs_len` beside
    it). lldb-dap writes a pointer with what it points at
    (`0x16fdfc9d4 3.04e-314`, `6171906384 "..."` for a uint8_t *)."""
    t = c_type_of(type_name)
    if t:
        return t.endswith("*")
    v = str(value or "")
    return bool(POINTER.match(v) or re.match(r"^0x[0-9a-fA-F]+ ", v))
# Every Iron panic prints its message and ends in the C library's abort().
PANIC_FUNCTIONS = ("abort", "__abort", "raise", "__pthread_kill", "pthread_kill",
                   "__pthread_kill_implementation", "gsignal")
# LLDB's answer to `command script import` when it was built without Python.
NO_SCRIPTING = re.compile(r"does not support importing modules|"
                          r"built without scripting language support")
# The runtime's own panics print "iron: <what happened>" (out of memory, an
# arena full, a missing map key...).
PANIC_LINE = re.compile(r"^(panic|assertion failed|error: index|iron: |.*out of bounds)", re.I)
ENCODED_IRON_NAME = re.compile(r"Iron__id_([0-9a-f]+)")


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
    msg = json.loads(body.decode("utf-8"))
    # (A body that is JSON null is a message, not the end of the input.)
    return msg if msg is not None else NOT_A_MESSAGE


NOT_A_MESSAGE = []      # read_message's value for a `null` body


def frame(msg):
    body = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


IRON_KEYWORDS = ["and", "or", "not", "true", "false", "null", "len"]

# Requests that let the program run.
RESUME_REQUESTS = ("launch", "attach", "restart", "continue", "next", "stepIn", "stepOut",
                   "stepBack", "reverseContinue", "restartFrame", "goto")
# Debugger commands typed in the debug console that do.
RESUME_COMMAND = re.compile(r"^\s*`?\s*(-exec\s+)?(c|continue|n|next|s|step|si|ni|stepi|nexti|"
                            r"fin|finish|r|run|start|until|advance|jump|kill|"
                            r"process\s+[\w-]+|thread\s+[\w-]+)(\s+[\w./-]|\s*$)")

# The requests of the Debug Adapter Protocol: anything else is answered
# with an error here (lldb-dap leaves an unknown request unanswered).
DAP_REQUESTS = {
    "initialize", "launch", "attach", "restart", "disconnect", "terminate", "breakpointLocations",
    "setBreakpoints", "setFunctionBreakpoints", "setExceptionBreakpoints", "dataBreakpointInfo",
    "setDataBreakpoints", "setInstructionBreakpoints", "configurationDone", "continue", "next",
    "stepIn", "stepOut", "stepBack", "reverseContinue", "restartFrame", "goto", "pause",
    "stackTrace", "scopes", "variables", "setVariable", "source", "threads", "terminateThreads",
    "modules", "loadedSources", "evaluate", "setExpression", "stepInTargets", "gotoTargets",
    "completions", "exceptionInfo", "readMemory", "writeMemory", "disassemble", "locations",
    "cancel", "runInTerminal", "startDebugging",
}
INT_ARGS = ("threadId", "frameId", "variablesReference", "startFrame", "levels", "line",
            "column", "start", "count", "targetId", "offset", "instructionCount",
            "instructionOffset", "sourceReference")


# Requests a debugger answers only when its initialize response says so;
# lldb-dap 18 stops answering (or crashes on) the others.
CAPABILITY_OF = {
    "loadedSources": "supportsLoadedSourcesRequest", "modules": "supportsModulesRequest",
    "readMemory": "supportsReadMemoryRequest", "writeMemory": "supportsWriteMemoryRequest",
    "disassemble": "supportsDisassembleRequest", "setVariable": "supportsSetVariable",
    "goto": "supportsGotoTargetsRequest", "gotoTargets": "supportsGotoTargetsRequest",
    "stepBack": "supportsStepBack", "reverseContinue": "supportsStepBack",
    "restartFrame": "supportsRestartFrame", "setExpression": "supportsSetExpression",
    "stepInTargets": "supportsStepInTargetsRequest",
    "terminateThreads": "supportsTerminateThreadsRequest",
    "dataBreakpointInfo": "supportsDataBreakpoints", "setDataBreakpoints": "supportsDataBreakpoints",
    "setInstructionBreakpoints": "supportsInstructionBreakpoints",
    "breakpointLocations": "supportsBreakpointLocationsRequest",
    "cancel": "supportsCancelRequest", "setFunctionBreakpoints": "supportsFunctionBreakpoints",
    "restart": "supportsRestartRequest", "terminate": "supportsTerminateRequest",
    "locations": "supportsLocationsRequest", "runInTerminal": "-", "startDebugging": "-",
}
# Arguments a request cannot do without (the protocol's required ones).
REQUIRED = {
    "stackTrace": ("threadId",), "continue": ("threadId",), "next": ("threadId",),
    "stepIn": ("threadId",), "stepOut": ("threadId",), "pause": ("threadId",),
    "stepBack": ("threadId",), "reverseContinue": ("threadId",), "goto": ("threadId", "targetId"),
    "exceptionInfo": ("threadId",), "scopes": ("frameId",), "variables": ("variablesReference",),
    "setBreakpoints": ("source",), "readMemory": ("memoryReference", "count"),
    "writeMemory": ("memoryReference", "data"), "disassemble": ("memoryReference", "instructionCount"),
    "setVariable": ("variablesReference", "name", "value"), "evaluate": ("expression",),
    "source": ("sourceReference",), "setExpression": ("expression", "value"),
    "restartFrame": ("frameId",), "gotoTargets": ("source", "line"),
    "stepInTargets": ("frameId",), "breakpointLocations": ("source", "line"),
    "dataBreakpointInfo": ("name",), "setDataBreakpoints": ("breakpoints",),
    "setInstructionBreakpoints": ("breakpoints",), "setFunctionBreakpoints": ("breakpoints",),
    "completions": ("text", "column"),
}


def bad_arguments(a):
    """Why a request's arguments cannot be what the protocol says (an id
    that is not an integer, a source that is not an object...), or None.
    Debuggers handle such requests badly (lldb-dap stops answering)."""
    for k in INT_ARGS:
        if k in a and a[k] is not None and (not isinstance(a[k], int) or isinstance(a[k], bool)
                                            or not -(1 << 62) < a[k] < (1 << 62)):
            return "%s must be an integer" % k
    for k in ("expression", "program", "context", "cwd", "name", "value", "text"):
        if k in a and a[k] is not None and not isinstance(a[k], str):
            return "%s must be a string" % k
    if "source" in a and a["source"] is not None:
        src = a["source"]
        if not isinstance(src, dict):
            return "source must be an object"
        if "path" in src and src["path"] is not None and not isinstance(src["path"], str):
            return "source.path must be a string"
    for k in ("breakpoints", "filters", "args"):
        if k in a and a[k] is not None and not isinstance(a[k], list):
            return "%s must be a list" % k
    for bp in a.get("breakpoints") or []:
        if not isinstance(bp, dict):
            return "each breakpoint must be an object"
        bad = bad_arguments(bp)
        if bad:
            return "a breakpoint's " + bad
    if "env" in a and a["env"] is not None and not isinstance(a["env"], dict):
        return "env must be an object"
    return None


# ── Naming rules shared with iron_lldb.py / iron_gdb.py ──────────────────────

def decode_iron_name(c_name):
    """Decode a target-reserved C identifier back to its Iron spelling."""
    def decode(match):
        digits = match.group(1)
        if len(digits) % 2:
            return match.group(0)
        try:
            return bytes.fromhex(digits).decode("ascii")
        except (ValueError, UnicodeDecodeError):
            return match.group(0)
    return ENCODED_IRON_NAME.sub(decode, c_name or "")


def iron_name(c_name):
    """(Iron name, deref) for a C local, or None for a compiler temporary."""
    if c_name.startswith("_ref_") and len(c_name) > 5:
        return decode_iron_name(c_name[5:]), True
    name = decode_iron_name(c_name)
    if not name or name.startswith("_"):
        return None
    return name, False


_METHODS = {}


def source_methods(path):
    """{lowercased type name: (type name, {method names})} declared in the
    Iron file `path` (objects, enums, patch blocks, receiver-form methods),
    to name a method's frame Counter.add rather than counter_add."""
    if path in _METHODS:
        return _METHODS[path]
    out = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except (OSError, TypeError, ValueError):
        _METHODS[path] = out
        return out
    current, depth = None, 0
    for line in text.splitlines():
        code = line.split("//")[0]
        if current is None:
            m = re.match(r"^\s*(?:pub\s+)?(?:patch\s+)?(?:object|enum|interface)\s+([A-Z]\w*)", code)
            if m and "{" in code:
                current, depth = m.group(1), 0
                out.setdefault(current.lower(), (current, set()))
            m2 = re.match(r"^\s*(?:pub\s+)?(?:\w+\s+)*func\s+([A-Z]\w*)\.(\w+)\s*\(", code)
            if m2:
                out.setdefault(m2.group(1).lower(), (m2.group(1), set()))[1].add(m2.group(2))
        if current is not None:
            if depth == 1:
                m = re.match(r"^\s*(?:\w+\s+)*func\s+(\w+)\s*\(", code)
                if m:
                    out[current.lower()][1].add(m.group(1))
                if re.match(r"^\s*(?:\w+\s+)*init\s*\(", code):
                    out[current.lower()][1].add("init")
            depth += code.count("{") - code.count("}")
            if depth <= 0 and "}" in code:
                current = None
    _METHODS[path] = out
    return out


def iron_function_name(c_name, test=None, source=None):
    """The Iron name of a frame's C function. A `test "..."` block compiles
    to Iron_iron__test_<n>: it is shown as test "<name>" when the session
    debugs that one test (`test`), else as test."""
    m = re.match(r"^(?:Iron_)?iron__test_\d+(.*)$", c_name)
    if m:
        return ('test "%s"' % test if test else "test") + m.group(1)
    m = re.match(r"^Iron_(\w+)(.*)$", c_name)
    if m:
        base, rest = m.group(1), m.group(2)
        # A method is Iron_<type, lowercased>_<method>: Counter.add
        mm = re.match(r"^([a-z0-9]+)_(\w+)$", base)
        types = source_methods(source) if source else {}
        if mm and mm.group(1) in types and mm.group(2) in types[mm.group(1)][1]:
            return "%s.%s%s" % (types[mm.group(1)][0], mm.group(2), rest)
        # A generic instance: gid__Int is gid[Int]
        parts = base.split("__")
        if len(parts) > 1 and all(p and p[0].isalpha() and p.isalnum() for p in parts):
            return "%s[%s]%s" % (parts[0], ", ".join(parts[1:]), rest)
        return base + rest
    m = re.match(r"^__lambda_\d+(.*)$", c_name)
    if m:
        return "func" + m.group(1)
    m = re.match(r"^__spawn_\d+(.*)$", c_name)
    if m:
        return "spawn" + m.group(1)
    m = re.match(r"^__pfor_\d+(.*)$", c_name)
    if m:
        return "parallel for" + m.group(1)
    return c_name


def is_iron_source(path):
    return bool(path) and path.endswith(".iron")


def is_runtime_source(path):
    """A C file of the Iron runtime or standard library (lib/runtime,
    lib/stdlib, or src/... in a source tree), as opposed to the program's
    generated C."""
    parent = os.path.basename(os.path.dirname(str(path or "").replace("\\", "/")))
    return parent in ("runtime", "stdlib")


# ── Finding the debugger and building ────────────────────────────────────────

def xcode_tools_installed():
    """macOS: the Command Line Tools (or Xcode) are installed. Without
    them, xcrun and the /usr/bin placeholders open the installer."""
    try:
        r = subprocess.run(["/usr/bin/xcode-select", "-p"], capture_output=True, text=True)
        return r.returncode == 0 and os.path.isdir(r.stdout.strip())
    except OSError:
        return False


def gdb_version(gdb):
    """gdb's major version, or None when it does not run."""
    try:
        v = subprocess.run([gdb, "--version"], capture_output=True, text=True).stdout
    except OSError:
        return None
    m = re.search(r"(\d+)\.\d+", v)
    return int(m.group(1)) if m else None


def windows_lldb_dap_usable(path, notes):
    """Windows: an lldb-dap.exe runs only with liblldb.dll beside it. The
    copy in Visual Studio's (and the Build Tools') LLVM has none, and
    fails to start; LLVM's own installer has both."""
    if os.path.isfile(os.path.join(os.path.dirname(path), "liblldb.dll")):
        return True
    notes.append("%s has no liblldb.dll beside it and cannot run (Visual Studio's LLVM "
                 "ships it that way); install LLVM itself" % path)
    return False


# Windows: LLDB from LLVM's installer loads Python (python3.dll) when it
# starts and stops at once without it; Python 3.9 and older do not work.
WINDOWS_LLDB_PYTHON = (3, 10)


def windows_python_dir():
    """The directory with this Python's python3.dll (also for a venv)."""
    base = getattr(sys, "base_prefix", sys.prefix)
    if os.path.isfile(os.path.join(base, "python3.dll")):
        return base
    return os.path.dirname(sys.executable)


def find_adapter(requested=None, notes=None):
    """(kind, argv) for the DAP debugger to run, or None. `notes`, a list,
    gets what was found but cannot be used (gdb too old, lldb without
    lldb-dap, an IRON_DAP_ADAPTER that names nothing)."""
    notes = [] if notes is None else notes

    def as_adapter(path):
        if os.path.basename(path).startswith("gdb"):
            return "gdb", [path, "-q", "-i=dap"]
        return "lldb-dap", [path]

    if requested:
        source = "--adapter"
    else:
        requested, source = os.environ.get("IRON_DAP_ADAPTER"), "IRON_DAP_ADAPTER"
    if requested:
        found = shutil.which(requested)
        if not found and not os.path.isfile(requested):
            notes.append("%s is %s, which is neither a program on PATH nor a file"
                         % (source, requested))
            return None
        return as_adapter(found or requested)
    names = (["lldb-dap"] + ["lldb-dap-%d" % v for v in range(25, 17, -1)] +
             ["lldb-vscode"] + ["lldb-vscode-%d" % v for v in range(17, 13, -1)])
    for n in names:
        p = shutil.which(n)
        if p and (os.name != "nt" or windows_lldb_dap_usable(p, notes)):
            return as_adapter(p)
    if os.name == "nt":
        # LLVM's installer puts it in Program Files and leaves PATH alone
        # unless asked to.
        for root in (os.environ.get("ProgramFiles"), os.environ.get("ProgramW6432"),
                     os.environ.get("LOCALAPPDATA") and
                     os.path.join(os.environ["LOCALAPPDATA"], "Programs")):
            p = root and os.path.join(root, "LLVM", "bin", "lldb-dap.exe")
            if p and os.path.isfile(p) and windows_lldb_dap_usable(p, notes):
                return as_adapter(p)
    if sys.platform == "darwin":
        # Homebrew's LLVM is keg-only: its lldb-dap is not on PATH.
        for p in ("/opt/homebrew/opt/llvm/bin/lldb-dap", "/usr/local/opt/llvm/bin/lldb-dap"):
            if os.path.isfile(p):
                return as_adapter(p)
        if xcode_tools_installed():
            try:
                p = subprocess.run(["/usr/bin/xcrun", "-f", "lldb-dap"], capture_output=True,
                                   text=True).stdout.strip()
                if p and os.path.exists(p):
                    return as_adapter(p)
            except OSError:
                pass
        else:
            notes.append("the Xcode Command Line Tools, which include lldb-dap, are not installed")
    gdb = shutil.which("gdb")
    if gdb:
        v = gdb_version(gdb)
        if v is not None and v >= 14:  # DAP mode since gdb 14
            return as_adapter(gdb)
        notes.append("%s is gdb %s; its DAP mode needs gdb 14 or later"
                     % (gdb, v if v is not None else "(version unknown)"))
    lldb = shutil.which("lldb")
    if lldb and not (sys.platform == "darwin" and lldb.startswith("/usr/bin/")):
        notes.append("%s is installed, but not lldb-dap, its DAP server (LLVM 18 and "
                     "later include it; LLVM 14 to 17 call it lldb-vscode)" % lldb)
    return None


def install_hint(requested=None):
    """How to get lldb-dap or gdb 14 or later on this OS, as lines."""
    if requested or os.environ.get("IRON_DAP_ADAPTER"):
        return ["set it to the path of lldb-dap or gdb (14 or later), "
                "or unset it to search PATH"]
    if sys.platform == "darwin":
        return ["lldb-dap comes with the Xcode Command Line Tools; install them with:",
                "    xcode-select --install",
                "or Homebrew's LLVM: brew install llvm"]
    if os.name == "nt":
        return ["LLVM for Windows includes lldb-dap.exe: winget install LLVM.LLVM",
                "(its LLDB needs Python 3.10 or later: winget install Python.Python.3.12;",
                "without them VS Code's Iron extension uses the C/C++ extension's debugger)"]
    return ["install LLDB (it includes lldb-dap) or gdb 14 or later:",
            "    sudo apt install lldb      (Debian, Ubuntu; or: sudo apt install gdb)",
            "    sudo dnf install lldb      (Fedora, RHEL, Rocky; or gdb)",
            "    sudo pacman -S lldb        (Arch; or gdb)"]


def no_adapter_message(notes, requested=None):
    """The error a client shows when no DAP debugger is found."""
    lines = ["no debugger found: iron dap runs lldb-dap, or gdb 14 or later in its DAP mode."]
    lines += ["  " + n for n in notes]
    lines += ["  " + h for h in install_hint(requested)]
    lines.append("  `iron debug --check` shows what debugging needs here.")
    return "\n".join(lines)


def check(requested=None):
    """`iron dap --check`: report the adapter's debugger; 0 when found."""
    notes = []
    found = find_adapter(requested, notes)
    for n in notes:
        print("  --   %s" % n)
    if not found:
        print("  --   lldb-dap, or gdb 14 or later: not found")
        for h in install_hint(requested):
            print("       " + h)
        return 1
    kind, argv = found
    err = windows_python_problem(kind)
    if err:
        print("  --   %s (%s): %s" % (kind, argv[0], err))
        return 1
    print("  ok   %s (%s)" % (kind, argv[0]))
    return 0


def windows_python_problem(kind):
    """Why LLDB on Windows cannot run with this Python, or None."""
    if os.name != "nt" or kind != "lldb-dap" or sys.version_info[:2] >= WINDOWS_LLDB_PYTHON:
        return None
    return ("LLDB on Windows needs Python %d.%d or later and this is Python %d.%d "
            "(winget install Python.Python.3.12)"
            % (WINDOWS_LLDB_PYTHON + tuple(sys.version_info[:2])))


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


def test_name(test):
    """The `test` launch argument as a test block's name. Zed's run button
    passes the block's string literal, quotes included (the tree-sitter
    capture behind $ZED_CUSTOM_test_name), so `"adds"` names the test
    adds; a bare name is taken as is."""
    if isinstance(test, str) and len(test) >= 2 and test[0] == '"' and test[-1] == '"':
        return re.sub(r'\\(["\\])', r"\1", test[1:-1])
    return test


def build_plan(iron, program, test=False, out_dir=None):
    """(argv, cwd, binary) that builds `program` with --debug (and its test
    blocks with `test`), None when `program` is already a binary, or a str
    error. A .iron file's binary goes to out_dir: each session has its own
    (#412), since two sessions building the same file into one shared path
    replace or delete the binary under each other's debugger."""
    exe = ".exe" if os.name == "nt" else ""
    program = os.path.abspath(program)
    if program.endswith(".iron"):
        out_dir = out_dir or os.path.join(tempfile.gettempdir(), "iron-debug")
        out = os.path.join(out_dir, os.path.basename(program)[:-5] + ("_test" if test else "") + exe)
        os.makedirs(out_dir, exist_ok=True)
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


# ── Values as Iron shows them ────────────────────────────────────────────────

def float_text(v, is_f32=False):
    """A float as Iron prints it (src/runtime/iron_fmt.c): the shortest
    digits that read back to the value, no trailing .0, inf / -inf / NaN."""
    if math.isnan(v):
        return "NaN"
    if math.isinf(v):
        return "inf" if v > 0 else "-inf"
    if v == 0:
        return "0"
    for p in range(1, 10 if is_f32 else 18):
        sci = "%.*e" % (p - 1, v)
        if (to_f32(float(sci)) == to_f32(v)) if is_f32 else float(sci) == v:
            break
    neg = sci.startswith("-")
    mant, e = sci.lstrip("-").split("e")
    e = int(e)
    digits = mant.replace(".", "").rstrip("0") or "0"
    n = len(digits)
    out = "-" if neg else ""
    if -6 <= e < 21:
        if e >= 0:
            out += "".join(digits[i] if i < n else "0" for i in range(e + 1))
            if n > e + 1:
                out += "." + digits[e + 1:]
        else:
            out += "0." + "0" * (-e - 1) + digits
    else:
        out += digits[0] + ("." + digits[1:] if n > 1 else "")
        out += "e%s%d" % ("-" if e < 0 else "+", abs(e))
    return out


def to_f32(x):
    try:
        return struct.unpack("f", struct.pack("f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def iron_quote(s):
    """A string as an Iron literal: the escapes of the manual (\\n, \\t,
    \\\\, \\"), any other control character (a zero byte too) as \\u{H}.
    The value formatters (iron_lldb.py, iron_gdb.py) write strings so."""
    out = []
    for ch in s:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif o < 0x20 or o == 0x7f:
            out.append("\\u{%x}" % o)
        else:
            out.append(ch)
    return '"%s"' % "".join(out)


ESCAPE = re.compile(r'\\(u\{([0-9a-fA-F]{1,6})\}|.)', re.S)


def iron_unescape(body):
    """The text of an Iron string literal's body (manual 1.4): \\n, \\t,
    \\\\, \\", \\{, \\}, \\u{H}; any other backslash is kept."""
    def rep(m):
        if m.group(2):
            try:
                return chr(int(m.group(2), 16))
            except (ValueError, OverflowError):
                return m.group(0)
        c = m.group(1)
        return {"n": "\n", "t": "\t", "\\": "\\", '"': '"', "{": "{", "}": "}"}.get(c, m.group(0))
    return ESCAPE.sub(rep, body)


# An Iron integer type: (name, bits, signed); a literal has none and takes
# its partner's (manual 1.4).
INT_C_TYPES = {"int8_t": ("Int8", 8, True), "int16_t": ("Int16", 16, True),
               "int32_t": ("Int32", 32, True), "int64_t": ("Int", 64, True),
               "uint8_t": ("UInt8", 8, False), "uint16_t": ("UInt16", 16, False),
               "uint32_t": ("UInt32", 32, False), "uint64_t": ("UInt", 64, False),
               "signed char": ("Int8", 8, True), "unsigned char": ("UInt8", 8, False),
               "short": ("Int16", 16, True), "unsigned short": ("UInt16", 16, False),
               "int": ("Int32", 32, True), "unsigned int": ("UInt32", 32, False),
               "long": ("Int", 64, True), "long long": ("Int", 64, True),
               "unsigned long": ("UInt", 64, False),
               "unsigned long long": ("UInt", 64, False)}
INT64 = ("Int", 64, True)


class IInt(int):
    """An integer with its Iron type (None: a literal)."""

    def __new__(cls, v, ty=None):
        o = int.__new__(cls, v)
        o.ty = ty
        return o


class IFloat(float):
    """A float, Float32 when f32 (None: a literal)."""

    def __new__(cls, v, f32=None):
        o = float.__new__(cls, to_f32(v) if f32 else v)
        o.f32 = f32
        return o


class Variant:
    """`Color.Green`, `Shape.Rect(2, 3)` written in an expression: equal to
    an enum value the debugger shows as `Green`, `Rect(2, 3)`."""

    def __init__(self, enum, name, args=None):
        self.enum, self.name, self.args = enum, name, args

    def text(self):
        if self.args is None:
            return self.name
        return "%s(%s)" % (self.name, ", ".join(fmt(a) for a in self.args))


def wrap_int(v, ty):
    _, bits, signed = ty or INT64
    v &= (1 << bits) - 1
    if signed and v >> (bits - 1):
        v -= 1 << bits
    return IInt(v, ty)


def c_type_of(text):
    t = re.sub(r"\b(const|volatile)\b", "", str(text or "")).strip()
    return re.sub(r"\s+", " ", t)


def clean_value(text, type_name=None):
    """A value as the debugger wrote it, as Iron shows it. lldb-dap writes
    a scalar or a pointer followed by its summary (`0x1006d61a8 rc Point
    {...}`, `0.10000000000000001 0.1`, `Iron_Color_Green Green`): the
    summary is kept. Without the value formatters (an LLDB without
    Python), a float is written as Iron prints it and Int8 / UInt8 as a
    number rather than a character."""
    t = str(text if text is not None else "")
    ty = c_type_of(type_name)
    m = RAW_THEN_SUMMARY.match(t)
    if m and ((ty.endswith("*") and not ty.startswith(("char", "const char", "unsigned char",
                                                       "uint8_t", "signed char"))) or
              ty in ("double", "float") or m.group(1).startswith("Iron_")):
        t = m.group(2)
    if ty in ("double", "float"):
        try:
            v = float(t)
        except ValueError:
            return t
        return float_text(v, ty == "float")
    if ty in ("int8_t", "uint8_t", "signed char", "unsigned char", "char"):
        m = re.match(r"^(-?\d+) '.*'$", t)              # gdb: -5 '\373'
        if m:
            return m.group(1)
        m = re.fullmatch(r"'(\\x[0-9a-fA-F]{1,2}|\\[0-7]{1,3}|\\.|.)'", t)   # lldb: '\xfb'
        if m:
            c = m.group(1)
            if c.startswith("\\x"):
                v = int(c[2:], 16)
            elif re.fullmatch(r"\\[0-7]{1,3}", c):
                v = int(c[1:], 8)
            elif c.startswith("\\"):
                v = ord({"n": "\n", "t": "\t", "r": "\r", "0": "\0", "a": "\a", "b": "\b",
                         "f": "\f", "v": "\v", "e": "\x1b"}.get(c[1], c[1]))
            else:
                v = ord(c)
            if ty in ("int8_t", "signed char") and v > 127:
                v -= 256
            return str(v)
    return t


RAW_THEN_SUMMARY = re.compile(r"^(0x[0-9a-fA-F]+|[-+]?\d[\d.]*(?:[eE][-+]?\d+)?|"
                              r"[-+]?(?:inf|nan)|Iron_\w+) (.+)$", re.I | re.S)


# ── Parsing ──────────────────────────────────────────────────────────────────

TOKEN = re.compile(r'\s*(?:(\d+\.\d+)|(0[xX][0-9a-fA-F]+|0[bB][01]+|\d+)|("(?:[^"\\]|\\.)*")|'
                   r'([A-Za-z_][A-Za-z0-9_]*)|'
                   r'(==|!=|<=|>=|&&|\|\||<<|>>|[-+*/%<>()\[\].,!&|^~]))', re.S)
BINARY = {"or": 1, "and": 2, "==": 3, "!=": 3, "<": 4, ">": 4, "<=": 4, ">=": 4,
          "|": 5, "^": 6, "&": 7, "<<": 8, ">>": 8,
          "+": 9, "-": 9, "*": 10, "/": 10, "%": 10}
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
            out.append(("num", IFloat(float(f))))
        elif i is not None:
            out.append(("num", IInt(int(i, 0))))
        elif s is not None:
            out.append(("str", s[1:-1]))      # unescaped (and interpolated) by the parser
        elif name is not None:
            out.append(("name", name))
        else:
            out.append(("op", {"&&": "and", "||": "or", "!": "not"}.get(op, op)))
    return out


def split_interpolation(body):
    """An Iron string literal's body as [("text", str) | ("expr", src)]:
    `{expr}` is interpolated unless its brace is escaped (manual 3.3)."""
    parts, i, buf = [], 0, []
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            m = re.match(r"\\u\{[0-9a-fA-F]{1,6}\}", body[i:])
            n = len(m.group(0)) if m else 2
            buf.append(body[i:i + n])
            i += n
            continue
        if c == "{":
            depth, j = 1, i + 1
            in_str = False
            while j < len(body) and depth:
                if body[j] == "\\":
                    j += 2
                    continue
                if body[j] == '"':
                    in_str = not in_str
                elif not in_str and body[j] == "{":
                    depth += 1
                elif not in_str and body[j] == "}":
                    depth -= 1
                j += 1
            if depth:
                raise IronEvalError("unclosed `{` in a string")
            if buf:
                parts.append(("text", iron_unescape("".join(buf))))
                buf = []
            parts.append(("expr", body[i + 1:j - 1]))
            i = j
            continue
        buf.append(c)
        i += 1
    if buf or not parts:
        parts.append(("text", iron_unescape("".join(buf))))
    return parts


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
        if value is not None and (t[0] not in ("op", "name") or t[1] != value):
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
        if t and t[0] in ("op", "name") and t[1] == "not":
            self.i += 1
            return ("not", self.unary())
        if t and t == ("op", "-"):
            self.i += 1
            return ("neg", self.unary())
        if t and t == ("op", "~"):
            self.i += 1
            return ("bnot", self.unary())
        return self.postfix(self.primary())

    def primary(self):
        t = self.take()
        if t[0] == "num":
            return ("lit", t[1])
        if t[0] == "str":
            parts = split_interpolation(t[1])
            if len(parts) == 1 and parts[0][0] == "text":
                return ("lit", parts[0][1])
            return ("interp", [(k, v if k == "text" else Parser(v).parse()) for k, v in parts])
        if t[0] == "name":
            if t[1] in ("true", "false"):
                return ("lit", t[1] == "true")
            if t[1] == "null":
                return ("lit", None)
            if self.peek(value="("):        # len(x), Int(x)
                self.i += 1
                args = self.args()
                return ("call", t[1], args)
            if t[1] == "String" and self.peek(value=".") and \
                    self.i + 2 < len(self.toks) and self.toks[self.i + 1] == ("name", "from_byte") \
                    and self.toks[self.i + 2] == ("op", "("):
                self.i += 3
                return ("call", "String.from_byte", self.args())
            if t[1][:1].isupper() and self.peek(value=".") and \
                    self.i + 1 < len(self.toks) and self.toks[self.i + 1][0] == "name" and \
                    self.toks[self.i + 1][1][:1].isupper():
                # Color.Green, Shape.Rect(2, 3): an enum variant (manual 3.6)
                self.i += 1
                v = self.take()[1]
                args = None
                if self.peek(value="("):
                    self.i += 1
                    args = self.args()
                return ("variant", t[1], v, args)
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


def is_field_path(tree):
    """A name or a field (`total`, `p.x`): what the debugger reads
    directly, with its type and children. (An element, `xs[i]`, is the
    evaluator's: it checks the index against the list's length.)"""
    k = tree[0]
    if k == "var":
        return True
    if k == "field":
        return is_field_path(tree[1])
    return False


class Ref:
    """A value the debugger holds: its path, its text and its children."""

    def __init__(self, path, text, ref, type_name=None):
        self.path, self.text, self.ref, self.type = path, text, ref, type_name


SIZE = re.compile(r"\bsize=(\d+)|^(?:Map|Set)\[(\d+)\]$")   # lldb, gdb
CONVERSIONS = {"Int": INT64, "Int64": ("Int64", 64, True), "Int32": ("Int32", 32, True),
               "Int16": ("Int16", 16, True), "Int8": ("Int8", 8, True),
               "UInt": ("UInt", 64, False), "UInt64": ("UInt64", 64, False),
               "UInt32": ("UInt32", 32, False), "UInt16": ("UInt16", 16, False),
               "UInt8": ("UInt8", 8, False)}


def int_name(ty):
    return (ty or INT64)[0]


def same_int(a, b):
    """Int and Int64 are one type to the debugger (both int64_t)."""
    na, nb = int_name(a), int_name(b)
    norm = {"Int64": "Int", "UInt64": "UInt"}
    return norm.get(na, na) == norm.get(nb, nb)


class IronEval:
    def __init__(self, proxy, frame_id):
        self.proxy = proxy
        self.frame_id = frame_id

    def ask(self, path):
        return self.proxy.request("evaluate", {"expression": path, "frameId": self.frame_id,
                                               "context": "watch"})

    def fetch(self, path):
        r = self.ask(path)
        if not r.get("success") and re.fullmatch(r"[A-Za-z_]\w*", path):
            # A var captured by a closure lives behind `_ref_<name>` (the
            # Locals view shows it as <name>); a global is `Iron_g_<name>`.
            for alt in ("(*_ref_%s)" % path, "Iron_g_%s" % path):
                r2 = self.ask(alt)
                if r2.get("success"):
                    r, path = r2, alt
                    break
        if not r.get("success"):
            if "." in path or "[" in path:
                raise IronEvalError("`%s` has no value here" % path)
            raise IronEvalError("no variable `%s` here" % path)
        b = r.get("body") or {}
        text = str(b.get("result", ""))
        if not text and b.get("variablesReference"):
            text = self.proxy.struct_text("", b["variablesReference"], b.get("type"))
        return self.value(path, text, b.get("variablesReference", 0), b.get("type"))

    @staticmethod
    def value(path, text, ref, type_name=None):
        ty = c_type_of(type_name)
        t = clean_value(text, type_name).strip()
        m = re.match(r"^Iron_Optional_(\w+)$", ty)
        if m and t != "null":
            ty = m.group(1).replace("Iron_String", "")
        if ty in ("double", "float"):
            try:
                return IFloat(float(t), ty == "float")
            except ValueError:
                pass
        if ty.endswith("*"):
            return Ref(path, t, ref, type_name)     # a pointer, however it is written
        if re.fullmatch(r"-?\d+", t):
            return IInt(int(t), INT_C_TYPES.get(ty, INT64))
        if re.fullmatch(r"-?\d+\.\d*(e[-+]?\d+)?|-?inf|NaN", t, re.I):
            return IFloat(float(t.replace("NaN", "nan")))
        if t in ("true", "false"):
            return t == "true"
        if t == "null":
            return None
        if len(t) >= 2 and t[0] == '"' and t[-1] == '"' and \
                re.fullmatch(r'"(?:[^"\\]|\\.)*"', t, re.S):
            return iron_unescape(t[1:-1])
        return Ref(path, t, ref, type_name)

    def children(self, v):
        if not isinstance(v, Ref) or not v.ref:
            return []
        synth = getattr(self.proxy, "synth", {})
        if v.ref in synth:
            return synth[v.ref]
        r = self.proxy.request("variables", {"variablesReference": v.ref})
        if not r.get("success"):
            return []
        return [dict(c, value=clean_value(c.get("value"), c.get("type")))
                for c in (r.get("body") or {}).get("variables", [])]

    def list_len(self, base):
        m = SIZE.search(base.text)
        if m:
            return int(m.group(1) or m.group(2))
        if base.text == "[]":
            return 0
        if is_c_array_value(base.text, base.type) and re.fullmatch(r"[A-Za-z_]\w*", base.path):
            n = self.fetch(base.path + "_len")      # a list kept as a C array
            if isinstance(n, int):
                return n
        if base.text.startswith("["):
            # A list shows its first elements: its length is the struct's count.
            if re.fullmatch(r"[A-Za-z_][\w.]*", base.path or ""):
                try:
                    n = self.fetch(base.path + ".count")
                    if isinstance(n, int) and not isinstance(n, bool) and n >= 0:
                        return n
                except IronEvalError:
                    pass
            return len(self.children(base))
        return None

    def element(self, base, idx):
        n = self.list_len(base)
        if n is not None and not 0 <= idx < n:
            raise IronEvalError("index out of bounds (index %d, bound %d)" % (idx, n))
        for c in self.children(base):
            if c.get("name") == "[%d]" % idx:
                return self.value("%s[%d]" % (base.path, idx), str(c.get("value", "")),
                                  c.get("variablesReference", 0), c.get("type"))
        try:
            return self.fetch("%s[%d]" % (base.path, idx))
        except IronEvalError:
            raise IronEvalError("index out of bounds (index %d, bound %s)" %
                                (idx, n if n is not None else "?"))

    def ev(self, e):
        k = e[0]
        if k == "lit":
            return e[1]
        if k == "var":
            return self.fetch(e[1])
        if k == "interp":
            return "".join(p if kind == "text" else text_of(self.ev(p)) for kind, p in e[1])
        if k == "variant":
            return Variant(e[1], e[2], None if e[3] is None else [self.ev(a) for a in e[3]])
        if k == "field":
            base = self.ev(e[1])
            if not isinstance(base, Ref):
                raise IronEvalError("`%s` has no fields" % fmt(base))
            for c in self.children(base):     # an object's field, also behind rc
                if c.get("name") == e[2]:
                    return self.value("%s.%s" % (base.path, e[2]), str(c.get("value", "")),
                                      c.get("variablesReference", 0), c.get("type"))
            return self.fetch("%s.%s" % (base.path, e[2]))
        if k == "index":
            base, idx = self.ev(e[1]), self.ev(e[2])
            if not isinstance(idx, int) or isinstance(idx, bool):
                raise IronEvalError("an index must be an Int")
            if isinstance(base, str):
                # s[i] is s.char_at(i): "" out of range (manual 9.2)
                return base[idx] if 0 <= idx < len(base) else ""
            if not isinstance(base, Ref):
                raise IronEvalError("`%s` cannot be indexed" % fmt(base))
            return self.element(base, idx)
        if k == "not":
            return not self.truth(self.ev(e[1]))
        if k == "neg":
            v = self.ev(e[1])
            if isinstance(v, IFloat):
                return IFloat(-v, v.f32)
            if isinstance(v, int) and not isinstance(v, bool):
                return wrap_int(-v, v.ty) if getattr(v, "ty", None) else IInt(-v)
            raise IronEvalError("`-` needs a number")
        if k == "bnot":
            v = self.ev(e[1])
            if not isinstance(v, int) or isinstance(v, bool):
                raise IronEvalError("`~` needs an integer")
            return wrap_int(~v, v.ty) if getattr(v, "ty", None) else IInt(~v)
        if k == "bin":
            return self.binary(e[1], e[2], e[3])
        if k == "call":
            args = [self.ev(a) for a in e[2]]
            if e[1] == "len" and len(args) == 1:
                return self.length(args[0])
            if e[1] == "String.from_byte" and len(args) == 1 and isinstance(args[0], int):
                b = int(args[0]) & 0xff
                return chr(b) if b < 0x80 else "\ufffd"
            if len(args) == 1 and e[1] in CONVERSIONS:
                v = args[0]
                if isinstance(v, float):
                    if math.isnan(v):
                        return IInt(0, CONVERSIONS[e[1]])
                    _, bits, signed = CONVERSIONS[e[1]]
                    lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else \
                        (0, (1 << bits) - 1)
                    v = max(lo, min(hi, int(v))) if not math.isinf(v) else (hi if v > 0 else lo)
                    return IInt(v, CONVERSIONS[e[1]])
                if isinstance(v, int) and not isinstance(v, bool):
                    return wrap_int(v, CONVERSIONS[e[1]])
            if len(args) == 1 and e[1] in ("Float", "Float64", "Float32"):
                v = args[0]
                if isinstance(v, (int, float)) and not isinstance(v, bool):
                    return IFloat(float(v), e[1] == "Float32")
            raise IronEvalError("`%s(...)` is not available while debugging" % e[1])
        if k == "method":
            return self.method(self.ev(e[1]), e[2], [self.ev(a) for a in e[3]])
        raise IronEvalError("cannot evaluate this expression")

    def truth(self, v):
        if isinstance(v, bool):
            return v
        raise IronEvalError("expected a Bool, got %s" % fmt(v))

    def numbers(self, op, a, b):
        """The common type of two numbers (manual 2.1): one integer type,
        or one float type; a literal takes its partner's type."""
        ints = all(isinstance(x, int) and not isinstance(x, bool) for x in (a, b))
        floats = all(isinstance(x, float) for x in (a, b))
        if ints:
            ta, tb = getattr(a, "ty", None), getattr(b, "ty", None)
            if ta and tb and not same_int(ta, tb):
                raise IronEvalError("`%s` mixes %s and %s; convert one (%s(x))" %
                                    (op, int_name(ta), int_name(tb), int_name(tb)))
            return "int", ta or tb or INT64
        if floats:
            fa, fb = getattr(a, "f32", None), getattr(b, "f32", None)
            if fa is not None and fb is not None and fa != fb:
                raise IronEvalError("`%s` mixes Float and Float32; convert one" % op)
            return "float", bool(fa or fb)
        if all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in (a, b)):
            raise IronEvalError("`%s` mixes an integer and a float; convert one (Float(x))" % op)
        return None, None

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
            eq = self.equal(a, b)
            return eq if op == "==" else not eq
        if op == "+" and isinstance(a, str) and isinstance(b, str):
            return a + b
        strs = isinstance(a, str) and isinstance(b, str)
        if op in ("<", ">", "<=", ">=") and strs:
            return {"<": a < b, ">": a > b, "<=": a <= b, ">=": a >= b}[op]
        kind, ty = self.numbers(op, a, b)
        if kind is None:
            if op in ("<", ">", "<=", ">="):
                raise IronEvalError("`%s` compares numbers or strings" % op)
            raise IronEvalError("`%s` needs numbers" % op)
        if op in ("<", ">", "<=", ">="):
            return {"<": a < b, ">": a > b, "<=": a <= b, ">=": a >= b}[op]
        if kind == "float":
            if op in ("&", "|", "^", "<<", ">>"):
                raise IronEvalError("`%s` takes integers" % op)
            try:
                if op == "/":
                    if b == 0:
                        v = math.nan if a == 0 or math.isnan(a) else \
                            math.copysign(math.inf, a) * math.copysign(1.0, b)
                    else:
                        v = a / b
                elif op == "%":
                    v = math.fmod(a, b) if b != 0 and not math.isinf(a) else math.nan
                else:
                    v = {"+": a + b, "-": a - b, "*": a * b}[op]
            except OverflowError:
                v = math.inf
            return IFloat(v, ty)
        x, y = int(a), int(b)
        bits = ty[1]
        if op in ("/", "%"):
            if y == 0:
                raise IronEvalError("integer division by zero")
            q = abs(x) // abs(y)
            q = q if (x >= 0) == (y >= 0) else -q
            return wrap_int(q if op == "/" else x - q * y, ty)
        if op in ("<<", ">>"):
            if y < 0:
                raise IronEvalError("shift by a negative count")
            if y >= bits:
                return wrap_int(0 if op == "<<" or x >= 0 else -1, ty)
            return wrap_int(x << y if op == "<<" else x >> y, ty)
        return wrap_int({"+": x + y, "-": x - y, "*": x * y, "&": x & y, "|": x | y,
                         "^": x ^ y}[op], ty)

    def equal(self, a, b):
        if isinstance(a, Variant) or isinstance(b, Variant):
            v, o = (a, b) if isinstance(a, Variant) else (b, a)
            if isinstance(o, Variant):
                return v.text() == o.text()
            return isinstance(o, Ref) and o.text == v.text()
        if isinstance(a, Ref) or isinstance(b, Ref):
            return isinstance(a, Ref) and isinstance(b, Ref) and a.text == b.text
        if a is None or b is None:
            return a is None and b is None
        if isinstance(a, bool) or isinstance(b, bool):
            return isinstance(a, bool) and isinstance(b, bool) and a == b
        if isinstance(a, str) or isinstance(b, str):
            return isinstance(a, str) and isinstance(b, str) and a == b
        kind, _ = self.numbers("==", a, b)
        return kind is not None and a == b

    def length(self, v):
        if isinstance(v, str):
            return IInt(len(v))
        if isinstance(v, Ref):
            n = self.list_len(v)
            return IInt(n if n is not None else len(self.children(v)))
        raise IronEvalError("len takes a list, map, set or String")

    def method(self, v, name, args):
        if name in ("len", "count") and not args and not isinstance(v, str):
            return self.length(v)
        if isinstance(v, str):
            return self.string_method(v, name, args)
        if name == "len" and not args:
            return self.length(v)
        if name == "is_empty" and not args:
            return self.length(v) == 0
        if name == "to_string" and not args and not isinstance(v, Ref):
            return text_of(v)
        if isinstance(v, Ref) and name in ("has", "get", "get_or", "contains") and args:
            kids = self.children(v)
            is_list = v.text.startswith("[")
            # A set's items are named [0], [1]... like a map's integer keys:
            # its type tells them apart.
            is_set = "Iron_Set_" in str(v.type or "") or v.text.startswith("Set[")
            if is_set:
                is_list = True
            if is_list and name == "get" and len(args) == 1 and isinstance(args[0], int):
                return self.element(v, args[0])
            key = fmt(args[0])
            for c in kids:
                cname = str(c.get("name", ""))
                if not is_list and cname == "[%s]" % key and name in ("has", "get", "get_or"):
                    if name == "has":
                        return True
                    return self.value("", str(c.get("value", "")), c.get("variablesReference", 0),
                                      c.get("type"))
            if name in ("has", "contains"):
                # a set's items, a list's elements
                if (is_list or name == "has") and any(
                        re.fullmatch(r"\[\d+\]", str(c.get("name", ""))) and
                        str(c.get("value", "")) == key for c in kids):
                    return True
                return False
            if name == "get_or" and len(args) == 2:
                return args[1]
            raise IronEvalError("key not found in map")
        raise IronEvalError("`.%s()` is not available while debugging" % name)

    def string_method(self, s, name, args):
        a = args
        if name in ("len", "count") and not a:
            return IInt(len(s))
        if name == "byte_len" and not a:
            return IInt(len(s.encode("utf-8", "surrogatepass")))
        if name == "is_empty" and not a:
            return s == ""
        if name in ("upper", "lower", "trim") and not a:
            return {"upper": s.upper(), "lower": s.lower(), "trim": s.strip()}[name]
        if name == "to_string" and not a:
            return s
        if len(a) == 1 and isinstance(a[0], str):
            x = a[0]
            if name == "contains":
                return x in s
            if name == "starts_with":
                return s.startswith(x)
            if name == "ends_with":
                return s.endswith(x)
            if name == "index_of":
                return IInt(s.find(x))
            if name == "rindex_of":
                return IInt(s.rfind(x))
            if name == "count":
                return IInt(s.count(x) if x else 0)
        if name == "char_at" and len(a) == 1 and isinstance(a[0], int):
            return s[a[0]] if 0 <= a[0] < len(s) else ""
        if name == "substring" and len(a) == 2 and all(isinstance(x, int) for x in a):
            lo, hi = max(0, min(a[0], len(s))), max(0, min(a[1], len(s)))
            return s[lo:hi] if lo < hi else ""
        if name == "replace" and len(a) == 2 and all(isinstance(x, str) for x in a):
            return s.replace(a[0], a[1]) if a[0] else s
        if name == "repeat" and len(a) == 1 and isinstance(a[0], int):
            return s * max(0, a[0])
        raise IronEvalError("`.%s()` is not available while debugging" % name)


def text_of(v):
    """The text Iron interpolates for a value (manual 3.3)."""
    if isinstance(v, str):
        return v
    return fmt(v)


def fmt(v):
    """A value as the debugger shows it."""
    if isinstance(v, bool):
        return "true" if v else "false"
    if v is None:
        return "null"
    if isinstance(v, str):
        return iron_quote(v)
    if isinstance(v, Ref):
        return v.text
    if isinstance(v, Variant):
        return v.text()
    if isinstance(v, float):
        return float_text(v, bool(getattr(v, "f32", False)))
    return str(int(v))


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
        self.global_scopes = set()  # variablesReference of a Globals scope
        self.configured = False  # the client sent configurationDone
        self.held_launch = None  # gdb: launch waits for configurationDone
        self.panic_frames = {}   # threadId -> index of the Iron frame that panicked
        self.panic_text = ""     # the debuggee's last panic message
        self.panic_text_changed = threading.Condition()
        self.trapped = set()     # threads stopped on a debug trap (their abort follows)
        self.trap_texts = {}       # threadId -> message of the debug trap it stopped on
        self.bp_rules = {}       # (path, line) -> condition / hit count / log message
        self.synth = {}          # SYNTH_REF_BASE + n -> children (a C array shown as a list)
        self.step_waiter = None  # queue of stops while the adapter steps on its own
        self.synth_seq = SYNTH_REF_BASE
        self.test = None         # the test block this session debugs, if any
        self.adapter_path = None
        self.warned_no_scripting = False
        self.build_dir = None    # this session's --debug builds (see build_plan)
        self.launched = False    # a launch or attach went to the debugger
        self.label_frames = set()  # frame ids the adapter made up (spawn sites)
        self.capabilities = {}     # the debugger's initialize response
        self.threads_asked = False  # the first stop asked lldb-dap for its threads
        self.resumed = True      # a request that runs the program went out since the last stop
        self.pending_since = {}  # client request seq -> when it went to the debugger
        threading.Thread(target=self.watchdog, daemon=True).start()

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
        if msg.get("command") in RESUME_REQUESTS or (
                msg.get("command") == "evaluate" and
                RESUME_COMMAND.match(str((msg.get("arguments") or {}).get("expression") or ""))):
            self.resumed = True
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
        if not slot[0].wait(timeout):
            sys.stderr.write("iron dap: %s did not answer %s within %ss\n" %
                             (self.kind or "the debugger", command, timeout))
        self.own.pop(seq, None)
        return slot[1] or {"success": False}

    def start_child(self):
        notes = []
        found = find_adapter(self.requested_adapter, notes)
        if not found:
            return no_adapter_message(notes, self.requested_adapter)
        self.kind, argv = found
        self.adapter_path = argv[0]
        err = windows_python_problem(self.kind)
        if err:
            return "%s: %s.\n  `iron debug --check` shows what debugging needs here." % (argv[0], err)
        env = None
        if os.name == "nt" and self.kind == "lldb-dap":
            # LLDB finds python3.dll on PATH: give it this Python's, which
            # may only be reachable through the py launcher.
            env = dict(os.environ)
            env["PATH"] = windows_python_dir() + os.pathsep + env.get("PATH", "")
        if self.kind == "gdb":
            fmt = os.path.join(HERE, "iron_gdb.py")
            if os.path.exists(fmt):
                argv = argv + ["-iex", "source %s" % fmt]
        try:
            self.child = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, env=env)
        except OSError as e:
            return "cannot start %s: %s\n  `iron debug --check` shows what debugging needs here." % (argv[0], e)
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
        """One message from the client. A message that is not a request
        the adapter can answer is ignored (and said so on stderr); a
        request it cannot handle gets an error response: nothing a client
        sends ends the session."""
        if not isinstance(msg, dict):
            sys.stderr.write("iron dap: ignored a message that is not a JSON object\n")
            return
        if msg.get("type") != "request":
            if msg.get("type") == "response":     # to a reverse request (runInTerminal)
                self.to_child(msg)
            else:
                sys.stderr.write("iron dap: ignored a message of type %r\n" % (msg.get("type"),))
            return
        seq = msg.get("seq")
        if not isinstance(seq, int) or isinstance(seq, bool):
            sys.stderr.write("iron dap: ignored a request without a numeric seq\n")
            return
        cmd = msg.get("command")
        if not isinstance(cmd, str) or not cmd:
            self.respond_error(msg, "a request needs a command")
            return
        if "arguments" in msg and msg["arguments"] is not None and \
                not isinstance(msg["arguments"], dict):
            self.respond_error(msg, "%s: the arguments must be an object" % cmd)
            return
        if self.child is None and cmd != "initialize":
            if cmd in ("disconnect", "terminate"):
                self.respond(msg, {})
            else:
                self.respond_error(msg, "%s: the session is not initialized (send initialize "
                                        "first)" % cmd)
            return
        if self.child is not None and self.child.poll() is not None and \
                cmd not in ("disconnect", "terminate"):
            self.respond_error(msg, "%s: the debugger has exited" % cmd)
            return
        if cmd not in DAP_REQUESTS:
            self.respond_error(msg, "%s: not a request iron dap supports" % cmd)
            return
        bad = bad_arguments(msg.get("arguments") or {})
        if bad:
            self.respond_error(msg, "%s: %s" % (cmd, bad))
            return
        missing = [k for k in REQUIRED.get(cmd, ()) if (msg.get("arguments") or {}).get(k) is None]
        if missing:
            self.respond_error(msg, "%s: the arguments need %s" % (cmd, ", ".join(missing)))
            return
        cap = CAPABILITY_OF.get(cmd)
        if cap and self.child is not None and not self.capabilities.get(cap):
            self.respond_error(msg, "%s: %s does not support it" % (cmd, self.kind or "the debugger"))
            return
        if cmd == "setExceptionBreakpoints":
            a = dict(msg.get("arguments") or {})
            if not isinstance(a.get("filters"), list):
                a["filters"] = []
            msg = dict(msg, arguments=a)
        fid = (msg.get("arguments") or {}).get("frameId")
        if fid in self.label_frames:
            if cmd == "scopes":
                self.respond(msg, {"scopes": []})
            else:
                self.respond_error(msg, "%s: this frame only shows where a task was spawned" % cmd)
            return
        if cmd == "configurationDone" and self.configured:
            # lldb-dap resumes the program on a second one.
            self.respond(msg, {})
            return
        if self.child is not None and cmd == "initialize":
            self.respond_error(msg, "initialize: the session is already initialized")
            return
        if self.child is not None and not self.launched and \
                cmd not in ("initialize", "launch", "attach", "disconnect", "terminate"):
            # lldb-dap leaves such requests unanswered until there is a target.
            self.respond_error(msg, "%s: no program is being debugged yet (send launch first)"
                               % cmd)
            return
        if cmd == "initialize":
            a = dict(msg.get("arguments") or {})
            for k, v in (("adapterID", "iron"), ("pathFormat", "path"), ("linesStartAt1", True),
                         ("columnsStartAt1", True)):
                a.setdefault(k, v)
            msg = dict(msg, arguments=a)
        try:
            self.dispatch(msg, cmd)
        except Exception as e:    # never drop the session over one request
            sys.stderr.write("iron dap: %s failed: %r\n" % (cmd, e))
            self.respond_error(msg, "%s failed in iron dap: %s" % (cmd, e))

    def watchdog(self):
        """A request the debugger never answers gets an error after a
        while, so a client never waits forever."""
        while True:
            time.sleep(5)
            now = time.monotonic()
            for seq, t in list(self.pending_since.items()):
                req = self.pending.get(seq)
                if req is None:
                    self.pending_since.pop(seq, None)
                    continue
                limit = 900 if req.get("command") in ("launch", "attach") else 120
                if now - t > limit:
                    self.pending.pop(seq, None)
                    self.pending_since.pop(seq, None)
                    self.respond_error(req, "%s: %s did not answer within %ds"
                                       % (req.get("command"), self.kind or "the debugger", limit))

    def guarded(self, fn, msg, *args):
        """Run a request's handler on its own thread: a failure is an
        error response, not a lost answer."""
        try:
            fn(msg, *args)
        except Exception as e:
            sys.stderr.write("iron dap: %s failed: %r\n" % (msg.get("command"), e))
            self.respond_error(msg, "%s failed in iron dap: %s" % (msg.get("command"), e))

    def dispatch(self, msg, cmd):
        if cmd == "initialize" and self.child is None:
            err = self.start_child()
            if err:
                self.respond_error(msg, err)
                return
        if cmd == "evaluate":
            threading.Thread(target=self.guarded, args=(self.handle_evaluate, msg),
                             daemon=True).start()
            return
        if cmd == "completions":
            threading.Thread(target=self.guarded, args=(self.handle_completions, msg),
                             daemon=True).start()
            return
        if cmd == "exceptionInfo":
            tid = (msg.get("arguments") or {}).get("threadId")
            if tid in self.panic_frames or tid in self.trap_texts:
                # A panic: the debugger would describe its abort()
                # breakpoint or the debug trap (EXC_BREAKPOINT, SIGTRAP);
                # say what the program printed instead.
                text = self.trap_texts.get(tid) or self.panic_text or "Iron panic"
                self.respond(msg, {"exceptionId": "panic", "breakMode": "always",
                                   "description": text})
                return
        if cmd in ("stepIn", "next", "stepOut") and self.configured:
            threading.Thread(target=self.guarded, args=(self.step, msg, cmd),
                             daemon=True).start()
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
            self.launched = True
            if self.kind == "gdb" and not self.configured:
                # gdb starts the program on `launch`; clients send their
                # breakpoints after it, so the program would run past
                # them. Launch once the configuration is done.
                self.held_launch = msg
                return
        if cmd in ("launch", "attach"):
            self.launched = True
        self.pending[msg["seq"]] = msg
        self.pending_since[msg["seq"]] = time.monotonic()
        self.to_child(msg)
        if cmd == "configurationDone":
            self.configured = True
            if self.held_launch is not None:
                held, self.held_launch = self.held_launch, None
                self.pending[held["seq"]] = held
                self.pending_since[held["seq"]] = time.monotonic()
                self.to_child(held)

    def rewrite_launch(self, req):
        a = dict(req.get("arguments") or {})
        program = a.get("program")
        if not program:
            self.respond_error(req, "launch: set \"program\" to a .iron file, a package "
                                    "directory or a binary")
            return None
        binary = program
        test = self.test = test_name(a.get("test"))
        if self.build_dir is None and a.get("build", True):
            self.build_dir = tempfile.mkdtemp(prefix="iron-debug-")
        plan = build_plan(self.iron, program, bool(test), self.build_dir) \
            if a.get("build", True) else None
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
            if a.get("stopOnPanic", True):
                # A plain LLDB breakpoint, not the formatters' iron-panic-stop:
                # it works in an LLDB without Python too, and the adapter
                # picks the Iron frame itself (finish_stopped).
                out["preRunCommands"] = (list(out.get("preRunCommands", [])) +
                                         ["breakpoint set --name abort"])
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
            if self.kind == "gdb":
                # gdb reports a function it calls in the program (a value
                # formatter asking the runtime what a handle holds, a call
                # typed in the console) as `continued` then `stopped`
                # ("function call"). The program is where it was: a client
                # told otherwise reads the Locals again, which calls again.
                if ev == "stopped" and (msg.get("body") or {}).get("reason") == "function call":
                    return
                if ev == "continued" and not self.resumed:
                    return
                if ev == "stopped":
                    self.resumed = False
            if ev in ("stopped", "continued"):
                self.local_scopes.clear()
                self.global_scopes.clear()
                self.synth.clear()
                self.panic_frames.clear()
            if ev == "output":
                self.note_output(msg)
                self.note_no_scripting(msg)
            if ev in ("stopped", "continued") and self.step_waiter is not None:
                # A step of the adapter's own (see step_in): not the client's.
                if ev == "stopped":
                    self.step_waiter.put(msg)
                return
            if ev == "stopped":
                # Telling a panic apart takes a request of our own: finish
                # on another thread.
                threading.Thread(target=self.finish_stopped, args=(msg,), daemon=True).start()
                return
            self.send(msg)
            return
        self.pending_since.pop(msg.get("request_seq"), None)
        req = self.pending.pop(msg.get("request_seq"), None)
        if not req or not msg.get("success"):
            self.send(msg)
            return
        cmd = req.get("command")
        body = msg.get("body") or {}
        if cmd == "initialize":
            self.capabilities = dict(body)
            # The debugger's exception filters (C++ throw, Ada, Objective-C)
            # do not apply to Iron programs.
            body["exceptionBreakpointFilters"] = []
            body.update(supportsConditionalBreakpoints=True,
                        supportsHitConditionalBreakpoints=True,
                        supportsLogPoints=True, supportsEvaluateForHovers=True,
                        supportsCompletionsRequest=True)
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
                if re.match(r"^globals$", s.get("name", ""), re.I) and s.get("variablesReference"):
                    self.global_scopes.add(s["variablesReference"])
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
                globals_ = ref in self.global_scopes
                out = []
                for v in body.get("variables", []):
                    if globals_:
                        # The program's globals (Iron_g_<name>) under their
                        # Iron names; the compiler's statics are hidden.
                        n = str(v.get("name", ""))
                        if not n.startswith("Iron_g_"):
                            continue
                        v = dict(v, name=n[7:])
                    if "value" in v:
                        v = dict(v, value=clean_value(v.get("value"), v.get("type")))
                    out.append(v)
                msg["body"] = dict(body, variables=out)
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
            spawned = [f for f in body.get("stackFrames", [])
                       if re.match(r"^__spawn_\d+", str(f.get("name", "")))]
            for f in body.get("stackFrames", []):
                name = f.get("name")
                src = f.get("source") or {}
                if isinstance(name, str):
                    path = src.get("path") if is_iron_source(src.get("path")) else None
                    f["raw_name"] = name
                    f["name"] = iron_function_name(name, self.test, path)
                if not is_iron_source(src.get("path") or src.get("name")):
                    f["presentationHint"] = "subtle"
                    if src:
                        src["presentationHint"] = "deemphasize"
            if spawned:
                # Reading where the tasks were spawned takes requests of
                # our own, whose answers arrive on this thread.
                threading.Thread(target=self.guarded_stack, args=(msg,), daemon=True).start()
                return
            for f in body.get("stackFrames", []):
                f.pop("raw_name", None)
            self.send(msg)
        else:
            self.send(msg)

    def guarded_stack(self, msg):
        try:
            self.finish_stack(msg)
        except Exception as e:
            sys.stderr.write("iron dap: stackTrace: %r\n" % e)
            self.send(msg)

    def finish_stack(self, msg):
        """A spawned task's stack: its body is `spawn "name"`, and under it
        a label frame says where the task was spawned (a --debug build
        records it in iron_dbg_spawn_<body>), the async part of its stack."""
        out = []
        for f in msg["body"].get("stackFrames", []):
            raw = f.pop("raw_name", "")
            out.append(f)
            m = re.match(r"^(__spawn_\d+)", str(raw))
            src = f.get("source") or {}
            if not m or not is_iron_source(src.get("path") or src.get("name")):
                continue
            r = self.request("evaluate", {"expression": "(const char *)iron_dbg_spawn_%s" % m.group(1),
                                          "frameId": f.get("id"), "context": "watch"})
            text = str((r.get("body") or {}).get("result", "")) if r.get("success") else ""
            q = re.search(r'"((?:[^"\\]|\\.)*)"', text)
            if not q:
                continue
            site = q.group(1).encode("latin-1", "backslashreplace").decode("unicode_escape")
            parts = site.split("|", 3)
            if len(parts) != 4 or not parts[1].isdigit():
                continue
            path, line, parent, name = parts
            if name:
                f["name"] = 'spawn "%s"' % name
            self.synth_seq += 1
            out.append({"id": self.synth_seq, "name": "spawned in %s" %
                        iron_function_name("Iron_" + parent if parent and not parent.startswith("_")
                                           else parent, self.test, path),
                        "source": {"name": os.path.basename(path), "path": path},
                        "line": int(line), "column": 1, "presentationHint": "label"})
            self.label_frames.add(self.synth_seq)
        msg["body"]["stackFrames"] = out
        self.send(msg)

    def note_output(self, msg):
        """Remember the debuggee's last panic message."""
        body = msg.get("body") or {}
        if body.get("category") not in ("stdout", "stderr", None):
            return
        for line in str(body.get("output", "")).splitlines():
            if PANIC_LINE.search(line):
                with self.panic_text_changed:
                    self.panic_text = line.strip()
                    self.panic_text_changed.notify_all()

    def note_no_scripting(self, msg):
        """An lldb-dap whose LLDB has no Python cannot load iron_lldb.py:
        say so once, with what to do, instead of leaving only LLDB's
        error in the console."""
        text = str((msg.get("body") or {}).get("output", ""))
        if self.warned_no_scripting or not NO_SCRIPTING.search(text):
            return
        self.warned_no_scripting = True
        self.output("iron dap: %s has no Python scripting, so the Iron value formatters "
                    "cannot load: strings, lists, maps and optionals show as their C "
                    "structs. Breakpoints, stepping and Iron expressions still work. For "
                    "Iron values, use an lldb-dap with Python (Xcode's, or your "
                    "distribution's lldb package) or gdb 14 or later (IRON_DAP_ADAPTER).\n"
                    % (self.adapter_path or "this lldb-dap"))

    def finish_stopped(self, msg):
        body = msg.get("body") or {}
        tid = body.get("threadId")
        self.trap_texts.pop(tid, None)   # set again below if this stop is a trap
        if self.kind == "lldb-dap" and not self.threads_asked:
            # lldb-dap answers its first threads request with the list it
            # kept when the configuration was done: a client that asks only
            # now (nvim-dap) would not find the thread of a stop in a
            # spawned task. Ask once ourselves.
            self.threads_asked = True
            self.request("threads", {})
        if tid is not None and body.get("reason") in ("breakpoint", "signal", "exception",
                                                       "function breakpoint", "stopped", None):
            r = self.request("stackTrace", {"threadId": tid, "startFrame": 0, "levels": 40})
            frames = (r.get("body") or {}).get("stackFrames", []) if r.get("success") else []
            top = [str(f.get("name", "")).split("(")[0].strip() for f in frames[:4]]
            if body.get("reason") == "breakpoint" and frames and \
                    not self.breakpoint_holds(frames[0], tid):
                return
            if any(n.replace("__GI_", "") in PANIC_FUNCTIONS for n in top):
                if tid in self.trapped:
                    # The debug trap already stopped on this panic, on its
                    # Iron line: the abort that follows ends the program.
                    self.request("continue", {"threadId": tid})
                    return
                for i, f in enumerate(frames):
                    src = f.get("source") or {}
                    if is_iron_source(src.get("path") or src.get("name")):
                        self.panic_frames[tid] = i
                        # GDB may forward the abort stop before the stderr
                        # output event even though the runtime flushed the
                        # message before aborting. Give its reader thread a
                        # bounded chance to deliver that already-written line.
                        with self.panic_text_changed:
                            if not self.panic_text:
                                self.panic_text_changed.wait(timeout=1.0)
                        msg = dict(msg, body=dict(body, reason="exception", description="Panic",
                                                  text=self.panic_text or "Iron panic"))
                        break
            elif frames and not body.get("hitBreakpointIds") and \
                    is_iron_source((frames[0].get("source") or {}).get("path")):
                text = self.trap_message(frames[0])
                if text:
                    self.trapped.add(tid)
                    self.trap_texts[tid] = text
                    msg = dict(msg, body=dict(body, reason="exception", description="Panic",
                                              text=text))
        self.send(msg)

    def trap_message(self, frame):
        """The panic message of a stop on a --debug build's debug trap
        (#388), or None. A check that fails under a debugger keeps its
        message in iron_debug_panic_message and runs a breakpoint
        instruction in the Iron function, so the stop is already on the
        Iron line; the panic itself (and its abort) runs on continue."""
        args = {"expression": "(const char *)iron_debug_panic_message",
                "frameId": frame.get("id"), "context": "watch"}
        r = {}
        for _ in range(2):
            r = self.request("evaluate", args)
            if r.get("success"):
                break
        if not r.get("success"):
            return None
        m = re.search(r'"((?:[^"\\]|\\.)*)"', str((r.get("body") or {}).get("result", "")))
        return m.group(1) if m and m.group(1) else None

    # ── Iron expressions in evaluate and breakpoints ───────────────────────

    def respond(self, req, body):
        self.send({"seq": 0, "type": "response", "request_seq": req["seq"],
                   "command": req.get("command"), "success": True, "body": body})

    def handle_evaluate(self, req):
        """An Iron expression is computed here, with Iron's rules; a name
        or a field (`total`, `p.x`) is read from the debugger, which gives
        its type and children; anything that is not Iron (a debugger
        command typed in the console, a C expression) goes to the debugger
        as written, and so does text starting with a backquote."""
        a = req.get("arguments") or {}
        text = str(a.get("expression", ""))
        frame_id = a.get("frameId")
        raw = text.startswith("`")
        tree = None
        if frame_id is not None and not raw:
            try:
                tree = Parser(text).parse()
            except (IronEvalError, RecursionError):
                tree = None
        if tree is not None and not is_field_path(tree):
            self.evaluate_iron(req, tree, frame_id)
            return
        if a.get("context") == "hover":
            # lldb-dap 18 answers a hover with `frame variable`'s whole line,
            # `(int32_t) t4 = -45`: ask for the value as a watch does.
            a = dict(a, context="watch")
        r = self.request("evaluate", a, timeout=30)
        if r.get("success") or tree is None:
            b = r.get("body") or {}
            if r.get("success"):
                if not b.get("result") and b.get("variablesReference"):
                    b = dict(b, result=self.struct_text("", b["variablesReference"], b.get("type")))
                b = dict(b, result=clean_value(b.get("result"), b.get("type")))
                r = dict(r, body=b)
            if r.get("success") and frame_id is not None and \
                    re.fullmatch(r"[A-Za-z_]\w*", text) and \
                    is_c_array_value(b.get("result"), b.get("type")):
                # A list kept as a C array: show its elements.
                n = self.request("evaluate", {"expression": text + "_len", "frameId": frame_id,
                                              "context": "watch"})
                count = str((n.get("body") or {}).get("result", "")) if n.get("success") else ""
                if re.fullmatch(r"\d+", count):
                    shown, ref, _ = self.array_view(text, int(count), frame_id)
                    r = dict(r, body=dict(b, result=shown, variablesReference=ref))
            self.send(dict(r, seq=0, request_seq=req["seq"], command="evaluate"))
            return
        # A name the debugger does not know: a captured var (`_ref_`), a
        # global (`Iron_g_`), a field behind a formatter's children.
        self.evaluate_iron(req, tree, frame_id)

    def evaluate_iron(self, req, tree, frame_id):
        try:
            v = IronEval(self, frame_id).ev(tree)
        except IronEvalError as e:
            self.respond_error(req, str(e))
            return
        except RecursionError:
            self.respond_error(req, "the expression is nested too deeply")
            return
        body = {"result": fmt(v), "variablesReference": v.ref if isinstance(v, Ref) else 0}
        if isinstance(v, Ref) and v.type:
            body["type"] = v.type
        self.respond(req, body)

    def handle_completions(self, req):
        """Debug console completions: the Iron names in scope (the frame's
        locals under their Iron names) and Iron's keywords, for the word
        before the cursor. Answered here: the debugger's completions are C
        names and debugger commands (and lldb-dap 18 crashes on a request
        without a frame)."""
        a = req.get("arguments") or {}
        text = a.get("text") if isinstance(a.get("text"), str) else ""
        col = a.get("column") if isinstance(a.get("column"), int) else len(text) + 1
        col = max(0, min(col - 1, len(text)))     # columnsStartAt1
        m = re.search(r"[A-Za-z_]\w*$", text[:col])
        prefix = m.group(0) if m else ""
        names = []
        fid = a.get("frameId")
        if isinstance(fid, int) and not isinstance(fid, bool) and fid not in self.label_frames:
            r = self.request("scopes", {"frameId": fid})
            for sc in ((r.get("body") or {}).get("scopes", []) if r.get("success") else []):
                if not re.match(r"^(locals|arguments)$", str(sc.get("name", "")), re.I):
                    continue
                vr = self.request("variables", {"variablesReference": sc.get("variablesReference")})
                for v in ((vr.get("body") or {}).get("variables", []) if vr.get("success") else []):
                    n = iron_name(str(v.get("name", "")))
                    if n:
                        names.append(n[0])
        words = sorted(set(names)) + [k for k in IRON_KEYWORDS if k not in names]
        targets = [{"label": w, "text": w, "start": col - len(prefix), "length": len(prefix),
                    "type": "variable" if w in names else "keyword"}
                   for w in words if w.startswith(prefix) and w != prefix]
        self.respond(req, {"targets": targets[:200]})

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

    def top_frame(self, tid):
        r = self.request("stackTrace", {"threadId": tid, "startFrame": 0, "levels": 50})
        frames = (r.get("body") or {}).get("stackFrames", []) if r.get("success") else []
        return (frames[0] if frames else {}), len(frames)

    def step_in(self, req):
        self.step(req, "stepIn")

    def frames_of(self, tid):
        r = self.request("stackTrace", {"threadId": tid, "startFrame": 0, "levels": 50})
        return (r.get("body") or {}).get("stackFrames", []) if r.get("success") else []

    def step(self, req, kind):
        """Step In, Over and Out stay in Iron code. Step In: a step that
        lands outside a .iron file (the runtime's C, a generated helper)
        steps back out, and a step that comes back to the line it started
        on steps in again, so `println(...)` goes to the next line and
        `square(4)` into square. Any step that leaves the program's Iron
        code altogether (out of main, into the C main that calls it) lets
        the program run on to its end instead of stopping in the C."""
        tid = (req.get("arguments") or {}).get("threadId")
        start, start_depth = self.top_frame(tid)
        start_at = ((start.get("source") or {}).get("path"), start.get("line"))
        self.respond(req, {})
        self.step_waiter = queue.Queue()
        last = None
        try:
            command = kind
            for _ in range(60):
                self.request(command, {"threadId": tid}, timeout=30)
                try:
                    last = self.step_waiter.get(timeout=30)
                except queue.Empty:
                    last = None
                    break
                if last["body"].get("reason") not in ("step", None):
                    break          # a breakpoint, a panic: show it
                frames = self.frames_of(tid)
                top, depth = (frames[0] if frames else {}), len(frames)
                src = top.get("source") or {}
                if not is_iron_source(src.get("path") or src.get("name")):
                    if not any(is_iron_source((f.get("source") or {}).get("path") or
                                              (f.get("source") or {}).get("name"))
                               for f in frames):
                        # Out of the program's Iron code: run on.
                        self.step_waiter = None
                        self.request("continue", {"threadId": tid}, timeout=30)
                        last = None
                        break
                    # An Iron function's prologue is the generated C's until
                    # its first statement: go on to that line. Anything else
                    # (the runtime, whose builtins are Iron_ too, a generated
                    # helper) is stepped out of.
                    iron_fn = str(top.get("name", "")).startswith(
                        ("Iron_", "__lambda_", "__spawn_", "__pfor_")) and \
                        not is_runtime_source(src.get("path") or src.get("name"))
                    command = "next" if kind == "stepIn" and iron_fn and depth > start_depth \
                        else "stepOut"
                    continue
                if kind == "stepIn" and (src.get("path"), top.get("line")) == start_at and \
                        depth == start_depth:
                    command = "stepIn"
                    continue
                break
        finally:
            self.step_waiter = None
        if last is not None:
            self.local_scopes.clear()
            self.synth.clear()
            threading.Thread(target=self.finish_stopped, args=(last,), daemon=True).start()

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
                out.append(dict(v, name=name))
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
        out = [dict(v, value=clean_value(v.get("value"), v.get("type")))
               if "value" in v and not v.get("_joined") else v for v in out]
        for v in out:
            v.pop("_joined", None)
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
            v = self.view_value(v, frame_id)
            if v.get("variablesReference"):
                v = dict(v, value=self.struct_text(str(v.get("value", "")),
                                                   v["variablesReference"], v.get("type")))
            name = str(v.get("name", ""))
            base = by_name.get(name[:-4], {}) if name.endswith("_len") else {}
            if base and is_c_array_value(base.get("value"), base.get("type")):
                continue
            n_var = by_name.get(name + "_len")
            if n_var and is_c_array_value(v.get("value"), v.get("type")) and \
                    re.fullmatch(r"\d+", str(n_var.get("value", ""))):
                text, ref, count = self.array_view(name, int(n_var["value"]), frame_id)
                v = dict(v, value=text, variablesReference=ref, indexedVariables=count,
                         _joined=True)
            out.append(v)
        return out

    def view_value(self, v, frame_id):
        """Present the generated Iron_View_T {ptr, len} value as the Iron
        View it represents, with indexed children instead of C fields."""
        type_name = str(v.get("type", ""))
        if not re.search(r"\bIron_View_[A-Za-z0-9_]+\b", type_name):
            return v
        ref = v.get("variablesReference", 0)
        name = str(v.get("evaluateName") or v.get("name", ""))
        if not ref or not name:
            return v
        r = self.request("variables", {"variablesReference": ref})
        kids = (r.get("body") or {}).get("variables", []) if r.get("success") else []
        lengths = [str(k.get("value", "")) for k in kids if k.get("name") == "len"]
        if not lengths or not re.fullmatch(r"\d+", lengths[0]):
            return v
        n = int(lengths[0])
        text, child_ref, count = self.array_view("(%s).ptr" % name, n, frame_id)
        return dict(v, value="View[size=%d] %s" % (n, text),
                    variablesReference=child_ref, indexedVariables=count)

    def array_view(self, name, n, frame_id):
        """(text, variablesReference, children) of the C array `name` of n
        elements, shown as a list: [1, 2, 3]."""
        elems = []
        for i in range(min(max(n, 0), ARRAY_SHOWN)):
            r = self.request("evaluate", {"expression": "%s[%d]" % (name, i),
                                          "frameId": frame_id, "context": "watch"})
            b = (r.get("body") or {}) if r.get("success") else {}
            elems.append({"name": "[%d]" % i,
                          "value": clean_value(b.get("result", "?"), b.get("type")),
                          "variablesReference": b.get("variablesReference", 0),
                          "evaluateName": "%s[%d]" % (name, i)})
        shown = ", ".join(e["value"] for e in elems[:20])
        text = "[%s%s]" % (shown, ", ..." if n > 20 else "")
        return text, (self.synthetic(elems) if elems else 0), len(elems)

    def struct_text(self, value, ref, type_name):
        """Write a GDB struct as LLDB's Point {x = 3, y = 4} summary.

        GDB DAP serializes a pretty-printer whose to_string returns Python
        None as the literal text "None". Native GDB needs that None to keep
        its nested `{x = 3, y = 4}` rendering, so treat it as the missing
        summary that it is only at this DAP boundary.
        """
        if self.kind == "gdb" and value == "None" and ref:
            value = ""
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
            try:
                msg = read_message(sys.stdin.buffer)
            except (ValueError, UnicodeError, RecursionError) as e:
                # A body that is not JSON (ValueError covers JSONDecodeError).
                sys.stderr.write("iron dap: ignored a message that is not JSON: %s\n" % e)
                continue
            if msg is None:
                break
            self.from_client(msg)
        if self.child:
            # The client is gone: end the debugger (and with it the
            # program), by force when it does not stop on its own.
            try:
                self.child.terminate()
                self.child.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    self.child.kill()
                    self.child.wait(timeout=3)
                except (OSError, subprocess.TimeoutExpired):
                    pass
        if self.build_dir:
            shutil.rmtree(self.build_dir, ignore_errors=True)


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
        elif argv[i] == "--check":
            return check(adapter)
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
