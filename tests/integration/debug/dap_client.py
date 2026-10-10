#!/usr/bin/env python3
"""Drive `iron dap` as an editor would (#312, #347) and check what it shows.

Usage: dap_client.py <stepping.iron> <adapter command...>   (e.g. iron dap)

Launches stepping.iron (the adapter builds it with --debug), stops on
line 3 inside area(w, h), and checks:
  - the stack: area called from main (C prefixes gone);
  - the Locals scope: w = 0, h = 2, product = 0, and no compiler
    temporary (a name starting with `_`);
  - a value formatter is loaded: `names` in main reads as its elements.
With --panic [--expect=func:line:text[:trap]] it checks the stop on a
panic instead (see check_panic).
Exit 0 on success, 1 on a failed check, 77 when no DAP debugger is found.
"""
import json
import os
import queue
import subprocess
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))


def frame(msg):
    body = json.dumps(msg).encode()
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


def reader(stream, q):
    while True:
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            c = stream.read(1)
            if not c:
                q.put(None)
                return
            header += c
        n = int(header.split(b"Content-Length:")[1].split(b"\r\n")[0])
        m = json.loads(stream.read(n))
        if os.environ.get("DAP_TRACE"):
            sys.stderr.write("<- %s\n" % json.dumps(m)[:400])
        q.put(m)


class Client:
    def __init__(self, argv):
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.q = queue.Queue()
        self.seq = 0
        self.seen = []
        threading.Thread(target=reader, args=(self.p.stdout, self.q), daemon=True).start()

    def send(self, command, args=None):
        self.seq += 1
        self.p.stdin.write(frame({"seq": self.seq, "type": "request", "command": command,
                                  "arguments": args or {}}))
        self.p.stdin.flush()
        return self.seq

    def wait(self, pred, timeout=60):
        # Messages seen earlier while waiting for something else.
        for m in self.seen:
            if pred(m):
                self.seen.remove(m)
                return m
        while True:
            m = self.q.get(timeout=timeout)
            if m is None:
                raise RuntimeError("adapter exited")
            if m.get("type") == "event" and m.get("event") == "output":
                sys.stderr.write(m["body"].get("output", ""))
                continue
            if pred(m):
                return m
            self.seen.append(m)

    def call(self, command, args=None, timeout=120):
        seq = self.send(command, args)
        r = self.wait(lambda m: m.get("type") == "response" and m.get("request_seq") == seq,
                      timeout)
        if not r.get("success"):
            raise RuntimeError("%s failed: %s" % (command, r.get("message")))
        return r.get("body") or {}

    def event(self, name, timeout=60):
        return self.wait(lambda m: m.get("type") == "event" and m.get("event") == name, timeout)


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def check_panic(c, stop, tid, expect):
    """--panic (panic.iron): the failed assert in check() stops the
    program as an exception, and the stack starts at the Iron line.
    --expect=func:line:text[:trap] checks another panic; with `trap` (a
    check's debug trap, #388) continuing ends the program without a
    second stop (the abort after the trap is not shown again)."""
    func, line, text, mode = (expect.split(":", 3) + [""])[:4] if expect else \
        ("check", "6", "total too large", "")
    body = stop["body"]
    if body.get("reason") != "exception" or text not in body.get("text", ""):
        fail("the panic stopped as %s, want an exception with '%s'" % (body, text))
    frames = c.call("stackTrace", {"threadId": tid})["stackFrames"]
    top = frames[0] if frames else {}
    if not str(top.get("name", "")).startswith(func) or top.get("line") != int(line):
        fail("the panic's stack starts at %s, want %s at line %s" %
             ([(f.get("name"), f.get("line")) for f in frames[:4]], func, line))
    print("dap: the panic stops at line %s in %s (%s)" % (line, func, body.get("text")))
    # VS Code shows exceptionInfo in its exception widget: the panic's
    # message, not the debugger's abort() breakpoint.
    info = c.call("exceptionInfo", {"threadId": tid})
    if text not in str(info.get("description", "")):
        fail("exceptionInfo is %s, want '%s'" % (info, text))
    print("dap: exceptionInfo says %s" % info.get("description"))
    if mode == "trap":
        c.call("continue", {"threadId": tid})
        end = c.wait(lambda m: m.get("type") == "event" and
                     m.get("event") in ("stopped", "exited", "terminated"), 60)
        if end.get("event") == "stopped":
            fail("continuing from the panic stopped again: %s" % end.get("body"))
        print("dap: continuing from the panic ends the program (%s)" % end.get("event"))
    try:
        c.call("disconnect", {"terminateDebuggee": True}, timeout=20)
    except Exception:
        pass
    c.p.kill()
    print("PASS")


def main():
    args = sys.argv[1:]
    panic = args[:1] == ["--panic"]
    expect = None
    if panic:
        args = args[1:]
        if args and args[0].startswith("--expect="):
            expect = args[0][len("--expect="):]
            args = args[1:]
    src = os.path.abspath(args[0])
    c = Client(args[1:])
    seq = c.send("initialize", {"clientID": "iron-test", "adapterID": "iron",
                                "linesStartAt1": True, "columnsStartAt1": True,
                                "pathFormat": "path"})
    r = c.wait(lambda m: m.get("type") == "response" and m.get("request_seq") == seq)
    if not r.get("success"):
        if "no debugger found" in (r.get("message") or ""):
            print("no DAP debugger (lldb-dap, gdb 14+): skipped")
            sys.exit(77)
        fail("initialize: %s" % r.get("message"))
    launch = c.send("launch", {"program": src})
    c.event("initialized", timeout=180)
    lines = [] if panic else [{"line": 3}, {"line": 12}]
    c.call("setBreakpoints", {"source": {"path": src}, "breakpoints": lines})
    c.call("configurationDone")
    c.wait(lambda m: m.get("type") == "response" and m.get("request_seq") == launch, 180)
    try:
        stop = c.wait(lambda m: m.get("type") == "event" and
                      m.get("event") in ("stopped", "exited", "terminated"), 120)
    except queue.Empty:
        # No event at all: the debugger cannot run a program on this host
        # (no debugserver, no ptrace permission).
        print("the debugger did not start the program here: skipped")
        sys.exit(77)
    if stop.get("event") != "stopped":
        fail("the program ended without stopping %s" %
             ("on the panic" if panic else "on the breakpoint at line 3"))
    tid = stop["body"].get("threadId")
    if panic:
        check_panic(c, stop, tid, expect)
        return

    frames = c.call("stackTrace", {"threadId": tid})["stackFrames"]
    names = [f["name"] for f in frames]
    if not names or not names[0].startswith("area") or not any(n.startswith("main") for n in names[1:3]):
        fail("stack is %s, want area called from main" % names)

    refs = {}

    def locals_of(frame_id):
        scopes = c.call("scopes", {"frameId": frame_id})["scopes"]
        out = {}
        for s in scopes:
            if s["name"].lower() in ("locals", "arguments"):
                for v in c.call("variables", {"variablesReference": s["variablesReference"]})["variables"]:
                    out[v["name"]] = v["value"]
                    refs[v["name"]] = v.get("variablesReference", 0)
        return out

    loc = locals_of(frames[0]["id"])
    for name, want in (("w", "0"), ("h", "2"), ("product", "0")):
        if loc.get(name) != want:
            fail("area's locals are %s, want %s = %s" % (loc, name, want))
    hidden = [n for n in loc if n.startswith("_")]
    if hidden:
        fail("compiler temporaries in the locals: %s" % hidden)
    print("dap: stack and locals of area(w, h) under Iron names")

    # Run on to line 12 in main: the list formatter is loaded.
    for _ in range(4):
        c.call("continue", {"threadId": tid})
        stop = c.event("stopped", timeout=60)
        frames = c.call("stackTrace", {"threadId": stop["body"].get("threadId")})["stackFrames"]
        if frames[0]["name"].startswith("main"):
            break
    loc = locals_of(frames[0]["id"])
    # The list formatter: `names` has its elements as children.
    elems = []
    if refs.get("names"):
        elems = [v["value"] for v in
                 c.call("variables", {"variablesReference": refs["names"]})["variables"]]
    if elems[:2] != ['"a"', '"b"']:
        fail("main's names is %s with elements %s, want \"a\", \"b\"" % (loc.get("names"), elems))
    print("dap: value formatters loaded (names = %s, elements %s)" % (loc["names"], elems))
    try:
        c.call("disconnect", {"terminateDebuggee": True}, timeout=20)
    except Exception:
        pass
    c.p.kill()
    print("PASS")


if __name__ == "__main__":
    main()
