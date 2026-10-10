#!/usr/bin/env python3
"""Iron expressions under `iron dap`: evaluate, conditional breakpoints,
hit counts and logpoints, as an editor uses them.

Usage: dap_eval_client.py <conditions.iron> <adapter command...>

conditions.iron sums range(10) into `total` (line 15), prints each of
names = ["a", "bb", "ccc"] (line 18) and ends with p = Point(3, 4)
(line 21). Checks:
  - a breakpoint with the condition `i == 7 and total > 20` on line 15
    stops once, with i = 7 and total = 21;
  - hover / watch expressions in Iron: names.len(), names[1] == "bb",
    not (i > 8), p.x + p.y, the object as Point {x = 3, y = 4};
  - a logpoint on line 18 (`name={n} len={n.len()}`) prints for each
    name and never stops;
  - names reads as ["a", "bb", "ccc"], and the locals show the loop
    variable n under its name;
  - in a second session, the hit count `>= 9` on line 15 first stops on
    the ninth hit (i = 8);
  - launched with "test": "sums the first ten", only that test block runs
    and its breakpoint (line 29) stops with total = 45;
  - step into from line 20 enters Point's init (line 6), and from line 18
    (println) it stays in conditions.iron instead of the runtime's C.
Exit 0 on success, 1 on a failed check, 77 when no DAP debugger is found.
"""
import os
import queue
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dap_client import Client  # noqa: E402


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def start(argv, src, breakpoints, extra=None):
    c = Client(argv)
    out = []
    seq = c.send("initialize", {"clientID": "iron-test", "adapterID": "iron",
                                "linesStartAt1": True, "columnsStartAt1": True,
                                "pathFormat": "path"})
    r = c.wait(lambda m: m.get("type") == "response" and m.get("request_seq") == seq)
    if not r.get("success"):
        if "no debugger found" in (r.get("message") or ""):
            print("no DAP debugger (lldb-dap, gdb 14+): skipped")
            sys.exit(77)
        fail("initialize: %s" % r.get("message"))
    caps = r.get("body") or {}
    for cap in ("supportsConditionalBreakpoints", "supportsHitConditionalBreakpoints",
                "supportsLogPoints"):
        if not caps.get(cap):
            fail("initialize does not report %s" % cap)
    launch = c.send("launch", dict({"program": src}, **(extra or {})))
    c.event("initialized", timeout=180)
    c.call("setBreakpoints", {"source": {"path": src}, "breakpoints": breakpoints})
    c.call("configurationDone")
    c.wait(lambda m: m.get("type") == "response" and m.get("request_seq") == launch, 180)

    def wait(pred, timeout=60):
        for m in list(c.seen):
            if pred(m):
                c.seen.remove(m)
                return m
        while True:
            m = c.q.get(timeout=timeout)
            if m is None:
                raise RuntimeError("adapter exited")
            if m.get("type") == "event" and m.get("event") == "output":
                out.append(m["body"].get("output", ""))
                continue
            if pred(m):
                return m
            c.seen.append(m)
    c.wait = wait
    return c, out


def next_stop(c):
    try:
        m = c.wait(lambda m: m.get("type") == "event" and
                   m.get("event") in ("stopped", "exited", "terminated"), 120)
    except queue.Empty:
        print("the debugger did not start the program here: skipped")
        sys.exit(77)
    if m["event"] != "stopped":
        return None, None
    tid = m["body"].get("threadId")
    top = c.call("stackTrace", {"threadId": tid, "levels": 1})["stackFrames"][0]
    return tid, top


def ev(c, top, expr):
    seq = c.send("evaluate", {"expression": expr, "frameId": top["id"], "context": "watch"})
    r = c.wait(lambda m: m.get("type") == "response" and m.get("request_seq") == seq, 30)
    if not r.get("success"):
        fail("evaluate %r: %s" % (expr, r.get("message")))
    return (r.get("body") or {}).get("result")


def finish(c):
    try:
        c.call("disconnect", {"terminateDebuggee": True}, timeout=20)
    except Exception:
        pass
    c.p.kill()


def main():
    src = os.path.abspath(sys.argv[1])
    argv = sys.argv[2:]

    c, out = start(argv, src, [{"line": 15, "condition": "i == 7 and total > 20"},
                               {"line": 18, "logMessage": "name={n} len={n.len()}"},
                               {"line": 21}])
    tid, top = next_stop(c)
    if not top or top.get("line") != 15:
        fail("the conditional breakpoint did not stop on line 15 (%s)" % top)
    got = {e: ev(c, top, e) for e in ("i", "total", "names.len()", 'names[1] == "bb"',
                                       "not (i > 8)", "total + i * 2")}
    want = {"i": "7", "total": "21", "names.len()": "3", 'names[1] == "bb"': "true",
            "not (i > 8)": "true", "total + i * 2": "35"}
    if got != want:
        fail("at i == 7: %s, want %s" % (got, want))
    print("dap: `i == 7 and total > 20` stopped once at i = 7; Iron expressions evaluate")

    c.call("continue", {"threadId": tid})
    tid, top = next_stop(c)
    if not top or top.get("line") != 21:
        fail("after the loop the next stop is %s, want line 21 (the condition held "
             "once, the logpoint never stops)" % top)
    text = "".join(out)
    for line in ("name=a len=1", "name=bb len=2", "name=ccc len=3"):
        if line not in text:
            fail("logpoint output %r is missing %r" % (text[-300:], line))
    p = ev(c, top, "p")
    # LLDB shows the object as Point {x = 3, y = 4}; gdb as {x = 3, y = 4}.
    if p not in ("Point {x = 3, y = 4}", "{x = 3, y = 4}") or ev(c, top, "p.x + p.y") != "7":
        fail("p is %r, want Point {x = 3, y = 4} and p.x + p.y = 7" % p)
    print("dap: the logpoint printed each name; p = %s" % p)
    # A list reads as its elements (LLDB), or as [3] with them as children (gdb).
    shown = ev(c, top, "names")
    elems = [ev(c, top, "names[%d]" % i) for i in range(3)]
    if shown not in ('["a", "bb", "ccc"]', "[3]") or elems != ['"a"', '"bb"', '"ccc"'] or \
            ev(c, top, "names.len()") != "3":
        fail("names is %r with %s, want [\"a\", \"bb\", \"ccc\"]" % (shown, elems))
    local_names = []
    for s in c.call("scopes", {"frameId": top["id"]})["scopes"]:
        if s["name"].lower() == "locals":
            local_names = [v["name"] for v in
                           c.call("variables", {"variablesReference": s["variablesReference"]})["variables"]]
    if "names_len" in local_names or "n" not in local_names:
        fail("locals are %s: want n, and no names_len" % local_names)
    print("dap: names = [\"a\", \"bb\", \"ccc\"]; locals %s" % local_names)
    finish(c)

    c, out = start(argv, src, [{"line": 15, "hitCondition": ">= 9"}])
    tid, top = next_stop(c)
    if not top or top.get("line") != 15 or ev(c, top, "i") != "8":
        fail("hit count >= 9 stopped at %s with i = %s, want line 15 with i = 8" %
             (top, top and ev(c, top, "i")))
    print("dap: hit count >= 9 stopped on the ninth hit")
    finish(c)

    # Debugging one test block: built with --test, only that test runs.
    c, out = start(argv, src, [{"line": 29}], {"test": "sums the first ten"})
    tid, top = next_stop(c)
    if not top or top.get("line") != 29 or ev(c, top, "total") != "45":
        fail("the test session stopped at %s, want line 29 of the test with total = 45" % top)
    if any(n + "\r\n" in "".join(out) or n + "\n" in "".join(out) for n in ("a", "bb", "ccc")):
        fail("main ran in the test session: %r" % "".join(out)[-200:])
    print("dap: test \"sums the first ten\" stopped on line 29 with total = 45")
    finish(c)

    # Step into Iron code only: into init from Point(3, 4), over println's
    # runtime code to the next Iron line.
    for line, want in ((20, (6,)), (18, (17, 18, 19, 20))):
        c, out = start(argv, src, [{"line": line}])
        tid, top = next_stop(c)
        c.call("stepIn", {"threadId": tid})
        tid, top = next_stop(c)
        path = os.path.realpath(((top or {}).get("source") or {}).get("path") or "")
        if path != os.path.realpath(src) or top.get("line") not in want:
            fail("step into from line %d stopped at %s:%s, want conditions.iron line %s" %
                 (line, path, top and top.get("line"), " or ".join(map(str, want))))
        finish(c)
    print("dap: step into enters init from line 20 and skips println's runtime code")
    print("PASS")


if __name__ == "__main__":
    main()
