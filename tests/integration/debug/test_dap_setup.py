#!/usr/bin/env python3
"""What `iron dap` tells the user about its debugger, without starting one:
the "no debugger found" message, an adapter named but missing, and the
warning for an lldb-dap whose LLDB has no Python (#312).

Usage: test_dap_setup.py <src/debug/iron_dap.py>
"""
import importlib.util
import io
import os
import sys
import tempfile
from contextlib import redirect_stdout

spec = importlib.util.spec_from_file_location("iron_dap", sys.argv[1])
dap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dap)

failed = 0


def expect(name, cond, detail=""):
    global failed
    if cond:
        print("ok: %s" % name)
    else:
        failed += 1
        print("FAIL: %s %s" % (name, detail))


missing = os.path.join(tempfile.gettempdir(), "iron-no-such-lldb-dap")

# --adapter (or IRON_DAP_ADAPTER) naming nothing: no fallback, a note.
notes = []
expect("missing --adapter is not used", dap.find_adapter(missing, notes) is None)
expect("missing --adapter is named", any("--adapter is %s" % missing in n for n in notes), notes)

os.environ["IRON_DAP_ADAPTER"] = missing
notes = []
expect("missing IRON_DAP_ADAPTER is not used", dap.find_adapter(None, notes) is None)
expect("missing IRON_DAP_ADAPTER is named",
       any("IRON_DAP_ADAPTER is %s" % missing in n for n in notes), notes)
msg = dap.no_adapter_message(notes)
expect("message keeps the no debugger found prefix", msg.startswith("no debugger found"), msg)
expect("message says how to fix the variable", "or unset it to search PATH" in msg, msg)
expect("message points at --check", "iron debug --check" in msg, msg)
out = io.StringIO()
with redirect_stdout(out):
    rc = dap.check()
expect("--check exits 1", rc == 1, rc)
expect("--check reports it", "not found" in out.getvalue(), out.getvalue())
del os.environ["IRON_DAP_ADAPTER"]

# A program that exists is taken as is, by its name: gdb runs in DAP mode.
with tempfile.TemporaryDirectory() as d:
    gdb = os.path.join(d, "gdb")
    open(gdb, "w").close()
    found = dap.find_adapter(gdb)
    expect("an existing --adapter gdb runs in DAP mode",
           found == ("gdb", [gdb, "-q", "-i=dap"]), found)

# The install hint matches the OS.
hint = "\n".join(dap.install_hint())
if sys.platform == "darwin":
    expect("macOS hint", "xcode-select --install" in hint, hint)
elif os.name == "nt":
    expect("Windows hint", "winget install LLVM.LLVM" in hint, hint)
else:
    expect("Linux hint", "sudo apt install lldb" in hint and "gdb" in hint, hint)

# An lldb-dap without Python: one warning in the console, with what to do.
proxy = dap.Proxy("iron", None)
sent = []
proxy.send = sent.append
proxy.adapter_path = "/opt/lldb/bin/lldb-dap"
err = {"type": "event", "event": "output", "body": {
    "category": "console",
    "output": "error: module importing failed: This script interpreter does not "
              "support importing modules.\n"}}
proxy.note_no_scripting(err)
proxy.note_no_scripting(err)
warn = [m for m in sent if "has no Python scripting" in m["body"]["output"]]
expect("no-Python lldb-dap warned once", len(warn) == 1, sent)
expect("warning names the adapter", warn and "/opt/lldb/bin/lldb-dap" in warn[0]["body"]["output"])
sent.clear()
proxy2 = dap.Proxy("iron", None)
proxy2.send = sent.append
proxy2.note_no_scripting({"type": "event", "event": "output",
                          "body": {"category": "console", "output": "Running initCommands:\n"}})
expect("no warning for ordinary output", not sent, sent)

sys.exit(1 if failed else 0)
