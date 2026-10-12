#!/usr/bin/env python3
"""Regressions found by the debugger fuzzer (tests/fuzz/dbg_fuzz.py), checked
through `iron dap` on regress.iron.

Usage: dap_regress_client.py <regress.iron> <adapter command...>

Lines of regress.iron marked `-- @name` are the stops. Checks:
  values     every kind of binding shows as an Iron value (#582, #583,
             #584, #548: integer widths, floats, escaped strings, enums,
             interfaces, generics, rc and weak rc, an alias binding, a
             destructured tuple without its temporary, a closure's captured
             var, a heap value, a Mutex, Json values and a JsonWriter,
             globals by their Iron names); hover asks for the value (#586);
             the evaluator follows Iron's rules (#587, #664)
  stepping   Step Over leaves an if's branch for the next statement, not the
             `if` line; nested loops; Step Into reaches init, a method, a
             generic instance and a lambda, named Counter.add, gid[Int],
             func; the closing brace after a return (#631, #595)
  services   a checked pointer parameter shows the object it points at; a
             Mutex shows its value, `locked by thread N` while a guard is
             alive and `unlocked` after; an HttpClient, a MysqlConn, a
             MysqlResult and a MysqlPool show what their handles hold
             (#548), and reading them sends the client no event (#709)
  arrays     lists kept as C arrays show as lists whatever their elements
             (#588); Locals with bindings not assigned yet are answered
             (#585)
  spawn      a spawned task's thread is named after spawn("worker") in the
             Threads view, also when the client first asks at that stop
             (#710), its frame `spawn "worker"`, with a label frame where
             it was spawned (#548)
  protocol   junk requests are answered with errors and the session goes on
             (#663)
Exit 0 on success, 1 on a failed check, 77 when no DAP debugger is found.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dap_client import Client, launch  # noqa: E402

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL: " + what)


def main():
    src = os.path.abspath(sys.argv[1])
    lines = {}
    with open(src, encoding="utf-8") as f:
        for i, l in enumerate(f, 1):
            m = re.search(r"-- @(\w+)", l)
            if m:
                lines[m.group(1)] = i
    c = Client(sys.argv[2:], [])
    r = c.response(c.send("initialize", {"clientID": "t", "adapterID": "iron", "linesStartAt1": True,
                                         "columnsStartAt1": True, "pathFormat": "path",
                                         "supportsVariableType": True}), "initialize")
    if not r.get("success"):
        if "no debugger found" in (r.get("message") or ""):
            print("no DAP debugger: skipped")
            sys.exit(77)
        print("FAIL: initialize: %s" % r.get("message"))
        sys.exit(1)
    # A request before launch is answered (#663).
    r = c.response(c.send("threads", {}), "threads", 30)
    check(not r.get("success"), "threads before launch: %s" % r)
    launch(c, src, breakpoints=[{"line": lines["values"]}, {"line": lines["then"]},
                                {"line": lines["spawn_body"]}])
    stop = c.event("stopped", 120)
    tid = stop["body"]["threadId"]
    top = c.call("stackTrace", {"threadId": tid})["stackFrames"][0]
    check(top.get("line") == lines["values"], "first stop at %s" % top.get("line"))
    fid = top["id"]
    shown = {}
    for sc in c.call("scopes", {"frameId": fid})["scopes"]:
        if sc["name"].lower() in ("locals", "arguments"):
            for v in c.call("variables", {"variablesReference": sc["variablesReference"]})["variables"]:
                shown[v["name"]] = v

    def val(n):
        return str(shown.get(n, {}).get("value"))
    want = {"i8": "-5", "u8": "200", "i32": "-70000", "f": "0.1", "f32": "0.1",
            "zero": '"a\\u{0}b"', "quote": '"say \\"hi\\"\\nline2"', "uni": '"héllo"',
            "xs": "[1, 2, 3]", "color": "Green", "nm": 'Line {tag = "seg"}',
            "gb": "Box2[Int] {item = 99}", "r": "rc Point {x = 8, y = 9} (strong=2, weak=1)",
            "r2": "rc Point {x = 8, y = 9} (strong=2, weak=1)", "alias": "62", "q": "17",
            "rem": "5", "hp": "Point {x = 11, y = 12}", "js": 'Json {"id": 7, "tags": ["a"]}',
            "jw": 'JsonWriter "{\\"visits\\":3"'}
    for n, w in want.items():
        check(val(n) == w, "%s shows %r, want %r" % (n, val(n), w))
    check(val("mx").startswith("Mutex(5)"), "mx shows %r" % val("mx"))
    check(not [n for n in shown if n.startswith(("tuple_tmp", "_"))],
          "temporaries in Locals: %s" % sorted(shown))
    m = shown.get("m", {})
    kids = c.call("variables", {"variablesReference": m.get("variablesReference")})["variables"] \
        if m.get("variablesReference") else []
    check([(k["name"], k["value"]) for k in kids] == [('["ann"]', "31")], "m's entries: %s" % kids)
    add = shown.get("add", {})
    kids = c.call("variables", {"variablesReference": add.get("variablesReference")})["variables"] \
        if add.get("variablesReference") else []
    check({k["name"]: k["value"] for k in kids}.get("cap") == "5", "add's captures: %s" % kids)

    def ev(expr, ctx="watch"):
        r = c.response(c.send("evaluate", {"expression": expr, "frameId": fid, "context": ctx}),
                       "evaluate", 30)
        return (r.get("body") or {}).get("result") if r.get("success") else "error: %s" % r.get("message")
    for expr, w in [("i32", "-70000"), ("u8 + 100", "44"), ("5 / 0", "error: integer division by zero"),
                    ("f / 2.0", "0.05"), ('uni == "héllo"', "true"), ("s[10]", '""'),
                    ("m.has(31)", "false"), ("-7.5 % 2.0", "-1.5"), ("2.5 * 2.0", "5"),
                    ("counter", "4"), ("color == Color.Green", "true"),
                    ('m.get("zed")', "error: key not found in map"),
                    ("xs[3]", "error: index out of bounds (index 3, bound 3)"),
                    ("(item * item) / item", "62"), ("hp.x + 1", "12"), ("cap", "5"),
                    ('"n={xs.len()}"', '"n=3"'), ("f32 * 3.0", "0.3")]:
        got = ev(expr)
        check(got == w, "evaluate %s = %r, want %r" % (expr, got, w))
    check(ev("i32", "hover") == "-70000", "hover i32 = %r" % ev("i32", "hover"))
    r = c.response(c.send("stackTrace", {"threadId": [1]}), "stackTrace", 30)
    check(not r.get("success"), "stackTrace with a list threadId answered %s" % r)

    # Stepping
    c.call("continue", {"threadId": tid})
    stop = c.event("stopped", 60)
    tid = stop["body"]["threadId"]

    def at():
        f = c.call("stackTrace", {"threadId": tid})["stackFrames"][0]
        return f.get("name"), f.get("line")

    def step(cmd):
        c.call(cmd, {"threadId": tid})
        c.event("stopped", 60)
        return at()
    check(at() == ("stepping", lines["then"]), "the branch stop: %s" % (at(),))
    check(step("next") == ("stepping", lines["after_if"]), "next out of the branch: %s" % (at(),))
    seen = []
    for _ in range(5):
        seen.append(step("next")[1])
        if seen[-1] == lines["inner"]:
            break
    # the outer header, the inner header (each may take two stops), the body
    check(seen[-1] == lines["inner"] and set(seen[:-1]) <= {lines["inner"] - 2, lines["inner"] - 1},
          "into the nested loops: %s" % seen)
    seen = []
    for _ in range(4):
        seen.append(step("next")[1])
        if seen[-1] == lines["inner"]:
            break
    # the next pass: back through the headers, never a line after the loops
    check(seen[-1] == lines["inner"] and set(seen[:-1]) <= {lines["inner"] - 2, lines["inner"] - 1},
          "the second pass of the nested loops: %s" % seen)
    for _ in range(12):
        if at()[1] == lines["ctor"]:
            break
        step("next")
    check(at()[1] == lines["ctor"], "next through the loops reached %s" % (at(),))
    check(step("stepIn")[0] == "Counter.init", "step into the init: %s" % (at(),))
    step("stepOut")
    for name, mark in (("Counter.add", "method"), ("gid[Int]", "generic"), ("rec", "rec")):
        for _ in range(4):
            if at()[1] == lines[mark]:
                break
            step("next")
        got = step("stepIn")
        check(got[0] == name, "step into the %s call: %s" % (mark, got))
        if mark == "generic":
            # the instance's statement, not the declaration's line (#722)
            check(got[1] == lines["gid_body"], "the generic instance's line: %s" % (got,))
        step("stepOut")
    for _ in range(6):
        if at()[1] == lines["call_lambda"]:
            break
        step("next")
    got = step("stepIn")
    check(got == ("func", lines["lambda"]), "step into the lambda: %s" % (got,))

    # Debugging services (#548)
    c.call("setBreakpoints", {"source": {"path": src}, "breakpoints": [
        {"line": lines[n]} for n in ("pointer", "held", "released", "arrays", "spawn_body")]})

    def stop_at(mark):
        c.call("continue", {"threadId": tid})
        t = c.event("stopped", 60)["body"]["threadId"]
        f = c.call("stackTrace", {"threadId": t})["stackFrames"][0]
        check(f.get("line") == lines[mark], "the %s stop is at %s" % (mark, f.get("line")))
        out = {}
        for sc in c.call("scopes", {"frameId": f["id"]})["scopes"]:
            if sc["name"].lower() in ("locals", "arguments"):
                for v in c.call("variables", {"variablesReference": sc["variablesReference"]})["variables"]:
                    out[v["name"]] = v
        return t, out

    def kids(v):
        ref = (v or {}).get("variablesReference")
        return {k["name"]: k["value"] for k in
                c.call("variables", {"variablesReference": ref})["variables"]} if ref else {}
    tid, shown = stop_at("pointer")
    wk = shown.get("wk", {})
    check(wk.get("value") == "Counter {n = 4}", "the pointer parameter shows %r" % wk.get("value"))
    check(kids(wk) == {"n": "4"}, "the pointee's fields: %s" % kids(wk))
    tid, shown = stop_at("held")
    c.seen[:] = [m for m in c.seen if m.get("event") != "continued"]
    got = {n: str(shown.get(n, {}).get("value")) for n in ("lk", "g", "hc", "conn", "res", "dbs")}
    check(re.fullmatch(r"Mutex\(9\) locked by thread \d+ \(0x[0-9a-f]+\)", got["lk"]) is not None,
          "the held Mutex shows %r" % got["lk"])
    check(kids(shown.get("lk")).get("value") == "9", "the Mutex's value: %s" % kids(shown.get("lk")))
    check(got["g"] == "MutexGuard(9)", "the guard shows %r" % got["g"])
    client = kids(shown.get("hc")).get("client")
    check(client == "HttpClient http://127.0.0.1:9, 0 of 4 connections open, 0 busy",
          "the HttpClient shows %r" % client)
    check(got["conn"] == "MysqlConn closed, last error 7004: connection is closed",
          "the MysqlConn shows %r" % got["conn"])
    check(got["res"] == "MysqlResult error 7004: connection is closed",
          "the MysqlResult shows %r" % got["res"])
    check(got["dbs"].startswith("rc MysqlPool 0 connections (0 idle, 0 in use), min 0, max 2"),
          "the MysqlPool shows %r" % got["dbs"])
    # Reading the handles runs code in the program (the runtime describes
    # them): the client is not told the program ran and stopped again
    # (#709).
    c.call("stackTrace", {"threadId": tid})
    stray = [m.get("event") for m in c.seen if m.get("event") in ("stopped", "continued")]
    check(not stray, "events after reading the handles: %s" % stray)
    tid, shown = stop_at("released")
    check(str(shown.get("lk", {}).get("value")) == "Mutex(9) unlocked",
          "the released Mutex shows %r" % shown.get("lk", {}).get("value"))

    # C-array lists of any element type show as lists (#588); the Locals of
    # a frame with bindings not assigned yet are answered (#585).
    tid, shown = stop_at("arrays")
    for n, w in (("names", '["a", "b"]'), ("halves", "[0.5]"), ("bytes", "[1, 2]"), ("total", "5")):
        check(str(shown.get(n, {}).get("value")) == w,
              "%s shows %r, want %r" % (n, shown.get(n, {}).get("value"), w))
    check(not [n for n in shown if n.endswith("_len")], "length temporaries in Locals: %s" % sorted(shown))
    check({"later", "after", "seen"} <= set(shown), "bindings declared later: %s" % sorted(shown))
    for n in ("later", "after", "seen", "plus"):
        if shown.get(n, {}).get("variablesReference"):
            r = c.response(c.send("variables", {"variablesReference": shown[n]["variablesReference"]}),
                           "variables", 30)
            check(r is not None, "the children of %s (not assigned yet) are answered" % n)

    # The spawned task
    c.call("continue", {"threadId": tid})
    stop = c.event("stopped", 60)
    tid = stop["body"]["threadId"]
    # The session's first threads request to the debugger (#710).
    listed = c.call("threads", {})["threads"]
    named = [t.get("name") for t in listed if t.get("id") == tid]
    check(len(named) == 1 and "worker" in (named[0] or ""),
          "the task's thread %s in the threads: %s" % (tid, listed))
    frames = c.call("stackTrace", {"threadId": tid})["stackFrames"]
    names = [f.get("name") for f in frames[:3]]
    check(names[:2] == ['spawn "worker"', "spawned in main"],
          "the task's stack: %s" % [(f.get("name"), f.get("line")) for f in frames[:4]])
    check(frames[1].get("line") == lines["spawn_body"] - 1 if len(frames) > 1 else False,
          "the spawn site's line: %s" % (frames[1].get("line") if len(frames) > 1 else None))
    c.close()
    if failures:
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
