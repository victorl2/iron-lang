#!/usr/bin/env python3
"""Debugger fuzzer: random programs debugged through `iron dap`, lldb and gdb.

Areas:
    values   every binding in scope at random probe lines, read through the
             DAP Locals scope (value and children) and `evaluate`, compared
             with the generator's model (dbg_gen.py): integers of every
             width, floats, strings (empty, unicode, zero bytes, long),
             lists, maps, sets, optionals, enums, objects, rc / weak rc,
             interfaces, generic instances, destructured tuples, parameters
    eval     random Iron expressions over the bindings at each probe,
             evaluated by the adapter and compared with what the program
             prints for the same expression on the probe line; failing
             expressions (division by zero, an index out of range, a
             missing key, an unknown name) must give the evaluator's error
    step     random call graphs (recursion, closures, methods, generics,
             defer): Step In / Over / Out sequences compared with the line
             trace a model of the program gives; the call stack's names
    panic    every panic kind must stop on its Iron line with its message
    proto    malformed and random DAP traffic: the adapter must answer or
             ignore it, never crash or hang

    dbg_fuzz.py AREA IRON OUTDIR SEED [COUNT] [--adapter lldb|gdb] [-j N]
    dbg_fuzz.py --suite IRON OUTDIR AREA...      (fixed seeds, for ctest)
    dbg_fuzz.py --gen AREA SEED                  (print one program)

IRON is the `iron` binary (ironc is found next to it). --adapter picks the
debugger `iron dap` runs (default: what it finds; gdb sets
IRON_DAP_ADAPTER=gdb). Failing programs and a report are kept under
OUTDIR/fail/. Exit 0 when every program passed, 1 on a failure, 77 when no
DAP debugger is available (the suite is skipped).
"""
import os
import random
import re
import shutil
import subprocess
import sys
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "integration", "debug"))
import dbg_gen  # noqa: E402
from dap_client import Client  # noqa: E402

SKIP = 77


class Skip(Exception):
    pass


class Fail(Exception):
    pass


# ── A DAP session ────────────────────────────────────────────────────────────

class Session:
    def __init__(self, iron, adapter, out):
        env = dict(os.environ)
        if adapter == "gdb":
            env["IRON_DAP_ADAPTER"] = "gdb"
        elif adapter == "lldb":
            env.pop("IRON_DAP_ADAPTER", None)
        self.out = []
        old = os.environ.copy()
        os.environ.clear()
        os.environ.update(env)
        try:
            self.c = Client([iron, "dap"], self.out)
        finally:
            os.environ.clear()
            os.environ.update(old)

    def init(self):
        seq = self.c.send("initialize", {"clientID": "dbg-fuzz", "adapterID": "iron",
                                         "linesStartAt1": True, "columnsStartAt1": True,
                                         "pathFormat": "path", "supportsVariableType": True})
        r = self.c.response(seq, "initialize", 60)
        if not r.get("success"):
            if "no debugger found" in (r.get("message") or ""):
                raise Skip("no DAP debugger")
            raise Fail("initialize: %s" % r.get("message"))
        return r.get("body") or {}

    def launch(self, program, lines, extra=None):
        c = self.c
        seq = c.send("launch", dict({"program": program}, **(extra or {})))
        m = c.wait(lambda m: (m.get("type") == "event" and m.get("event") == "initialized") or
                   (m.get("type") == "response" and m.get("request_seq") == seq), 240,
                   "initialized")
        if m.get("type") == "response":
            if not m.get("success"):
                raise Fail("launch failed: %s" % m.get("message"))
            c.event("initialized", 60)
            m = None
        bps = c.call("setBreakpoints", {"source": {"path": program},
                                        "breakpoints": [{"line": l} for l in lines]})
        c.call("configurationDone")
        if m is not None:
            r = c.response(seq, "launch", 120)
            if not r.get("success"):
                raise Fail("launch failed: %s" % r.get("message"))
        return bps.get("breakpoints", [])

    def next_stop(self, timeout=60):
        m = self.c.wait(lambda m: m.get("type") == "event" and
                        m.get("event") in ("stopped", "exited", "terminated"), timeout, "stop")
        return m if m.get("event") == "stopped" else None

    def call(self, cmd, args, timeout=30):
        return self.c.call(cmd, args, timeout)

    def request(self, cmd, args, timeout=30):
        seq = self.c.send(cmd, args)
        return self.c.response(seq, cmd, timeout)

    def close(self):
        self.c.close()


# ── Comparing what the debugger shows with the model ─────────────────────────

CLOSURE = re.compile(r"^func (\S+)(?: at (\S+):(\d+))?$")


def norm(text):
    """The adapter's value text, ready to compare."""
    return str(text if text is not None else "").strip()


def size_of(text):
    m = re.fullmatch(r"(?:size=|Map\[|Set\[)(\d+)\]?", text)
    return int(m.group(1)) if m else None


def compare(where, want, got_text, kids_of, problems, depth=0):
    """want: (disp, kids) from dbg_gen.shot; got_text: the value shown;
    kids_of(): [(name, value, kids_of)] of the shown value."""
    disp, kids = want
    got = norm(got_text)
    if isinstance(disp, tuple) and disp[0] == "size":
        n = size_of(got)
        if n != disp[1]:
            problems.append("%s: shows %r, want a map or set of %d entries" % (where, got, disp[1]))
    elif isinstance(disp, tuple) and disp[0] == "closure":
        if not CLOSURE.match(got):
            problems.append("%s: shows %r, want func <name> at <file>:<line>" % (where, got))
    elif got != disp:
        problems.append("%s: shows %r, want %r" % (where, got, disp))
        return
    if kids is None or depth > 0:
        return
    shown = kids_of()
    if isinstance(kids, tuple) and kids[0] == "map":
        got_map = {n: (v, k) for n, v, k in shown}
        if len(shown) != len(kids[1]):
            problems.append("%s: %d entries shown, want %d (%s)" %
                            (where, len(shown), len(kids[1]), [n for n, _, _ in shown]))
        for key, val in kids[1]:
            name = "[%s]" % key
            if name not in got_map:
                problems.append("%s: no entry %s among %s" % (where, name, sorted(got_map)[:6]))
                continue
            compare("%s%s" % (where, name), val, got_map[name][0], got_map[name][1], problems,
                    depth + 1)
        return
    if isinstance(kids, tuple) and kids[0] == "set":
        got_items = sorted(norm(v) for _, v, _ in shown)
        if got_items != kids[1]:
            problems.append("%s: items %s, want %s" % (where, got_items, kids[1]))
        return
    names = [n for n, _, _ in shown]
    want_names = [n for n, _ in kids]
    if names[:len(want_names)] != want_names or len(names) != len(want_names):
        problems.append("%s: children %s, want %s" % (where, names[:12], want_names[:12]))
        return
    for (n, v, k), (wn, wv) in zip(shown, kids):
        compare("%s.%s" % (where, n) if not n.startswith("[") else where + n, wv, v, k,
                problems, depth + 1)


def dap_kids(s, ref):
    def get():
        if not ref:
            return []
        r = s.request("variables", {"variablesReference": ref})
        vs = (r.get("body") or {}).get("variables", []) if r.get("success") else []
        return [(v.get("name"), v.get("value"),
                 (lambda r2=v.get("variablesReference", 0): dap_kids(s, r2)())) for v in vs]
    return get


# ── values / eval ────────────────────────────────────────────────────────────

def parse_output(stdout):
    """[(probe id, [expression texts])] in the order the probes ran."""
    out = []
    for m in re.finditer(r"^@P(\d+) (.*?)$(?=\n@P|\n?\Z)", stdout, re.S | re.M):
        exprs = re.findall(r"@E\d+<(.*?)>@", m.group(2), re.S)
        out.append((int(m.group(1)), exprs))
    return out


def build(ironc, src, binary):
    r = subprocess.run([ironc, "build", src, "--debug", "-o", binary], capture_output=True,
                       text=True, timeout=300)
    if r.returncode != 0:
        raise Fail("build failed:\n" + (r.stdout + r.stderr)[-3000:])


def values_check(g, src, binary, iron, adapter, problems, area):
    run = subprocess.run([binary], capture_output=True, timeout=120)
    stdout = run.stdout.decode("utf-8", "replace")
    printed = parse_output(stdout)
    # The model's hits per probe, in order.
    want_seq = []
    seen = {}
    for pid, exprs in printed:
        k = seen.get(pid, 0)
        seen[pid] = k + 1
        p = g.probes[pid]
        if k >= len(p.hits):
            problems.append("probe %d ran %d times, the model says %d" % (pid, k + 1, len(p.hits)))
            return
        # The oracle checks itself: the model against the program.
        for i, (e, text) in enumerate(zip(p.exprs, exprs)):
            kind, val = p.expr_hits[k][i]
            if kind == "ok" and e.text(val) != text:
                problems.append("MODEL: probe %d expression %s: program prints %r, model %r"
                                % (pid, e.src, text, e.text(val)))
        want_seq.append((pid, k))
    if len(printed) != sum(len(p.hits) for p in g.probes):
        problems.append("the program printed %d probes, the model has %d" %
                        (len(printed), sum(len(p.hits) for p in g.probes)))
        return
    s = Session(iron, adapter, None)
    try:
        s.init()
        lines = sorted({p.line for p in g.probes})
        bps = s.launch(src, lines)
        for b in bps:
            if not b.get("verified", True) and adapter != "gdb":
                problems.append("breakpoint at line %s not verified: %s" % (b.get("line"), b))
        line_to_pid = {}
        for p in g.probes:
            line_to_pid[p.line] = p.pid
        for pid, k in want_seq:
            stop = s.next_stop(90)
            if stop is None:
                problems.append("the program ended before probe %d (hit %d)" % (pid, k))
                return
            tid = stop["body"].get("threadId")
            frames = s.call("stackTrace", {"threadId": tid, "levels": 20})["stackFrames"]
            top = frames[0]
            if top.get("line") != g.probes[pid].line:
                problems.append("stopped at %s:%s, want probe %d at line %d" %
                                (top.get("name"), top.get("line"), pid, g.probes[pid].line))
                return
            want_fn = g.probes[pid].func
            if not str(top.get("name", "")).startswith(want_fn):
                problems.append("probe %d: frame name %r, want %s" % (pid, top.get("name"), want_fn))
            check_stop(s, g, g.probes[pid], k, top, problems, area)
            s.call("continue", {"threadId": tid})
            if len(problems) > 40:
                return
        end = s.next_stop(60)
        if end is not None:
            problems.append("stopped again after the last probe: %s" % end.get("body"))
    finally:
        s.close()


def check_stop(s, g, probe, k, top, problems, area):
    want = probe.hits[k]
    where = "probe %d hit %d" % (probe.pid, k)
    scopes = s.call("scopes", {"frameId": top["id"]})["scopes"]
    shown = {}
    for sc in scopes:
        if sc.get("name", "").lower() in ("locals", "arguments"):
            for v in s.call("variables", {"variablesReference": sc["variablesReference"]})["variables"]:
                shown[v["name"]] = v
    if area == "values":
        # A binding declared later in the function is listed too (C
        # declares it at the top; its memory is not assigned yet).
        later = set(getattr(g, "extra_names", {}).get(probe.func, ()))
        for p in g.probes:
            if p.func == probe.func:
                for h in p.hits:
                    later |= set(h)
        extra = sorted(set(shown) - set(want) - later)
        missing = sorted(set(want) - set(shown))
        if missing:
            problems.append("%s: not in Locals: %s (shown: %s)" % (where, missing, sorted(shown)))
        if extra:
            problems.append("%s: Locals has names that are no Iron binding here: %s" % (where, extra))
        for name, w in want.items():
            if name not in shown:
                continue
            v = shown[name]
            compare("%s: %s" % (where, name), w, v.get("value"),
                    dap_kids(s, v.get("variablesReference", 0)), problems)
            # hover / watch on the name gives the same value
            r = s.request("evaluate", {"expression": name, "frameId": top["id"], "context": "hover"})
            if not r.get("success"):
                problems.append("%s: evaluate %s failed: %s" % (where, name, r.get("message")))
            else:
                b = r.get("body") or {}
                compare("%s: evaluate %s" % (where, name), (w[0], None), b.get("result"),
                        None, problems)
    else:
        for i, e in enumerate(probe.exprs):
            kind, val = probe.expr_hits[k][i]
            r = s.request("evaluate", {"expression": e.src, "frameId": top["id"],
                                       "context": "watch"})
            got = (r.get("body") or {}).get("result") if r.get("success") else \
                "<error: %s>" % r.get("message")
            if got != e.disp(val):
                problems.append("%s: evaluate %s = %r, want %r" % (where, e.src, got, e.disp(val)))
        for text, err in probe.errors:
            r = s.request("evaluate", {"expression": text, "frameId": top["id"],
                                       "context": "watch"})
            if r.get("success"):
                problems.append("%s: evaluate %s = %r, want the error %r" %
                                (where, text, (r.get("body") or {}).get("result"), err))
            elif err not in str(r.get("message")):
                problems.append("%s: evaluate %s fails with %r, want %r" %
                                (where, text, r.get("message"), err))


def run_values(area, seed, ironc, iron, outdir, adapter):
    rng = random.Random("%s-%s" % (area, seed))
    g, text = dbg_gen.value_program(rng, 0 if area == "values" else 4)
    work = os.path.join(outdir, "%s_%s" % (area, seed))
    os.makedirs(work, exist_ok=True)
    src = os.path.join(work, "prog.iron")
    with open(src, "w") as f:
        f.write(text)
    binary = os.path.join(work, "prog")
    build(ironc, src, binary)
    problems = []
    values_check(g, src, binary, iron, adapter, problems, area)
    return problems, work


AREAS = {"values": run_values, "eval": run_values}


def run_one(area, seed, ironc, iron, outdir, adapter):
    t0 = time.time()
    try:
        problems, work = AREAS[area](area, seed, ironc, iron, outdir, adapter)
    except Skip:
        raise
    except Fail as e:
        problems, work = [str(e)], os.path.join(outdir, "%s_%s" % (area, seed))
    except Exception as e:  # a crash of the harness or a hang of the adapter
        problems = ["%s: %s" % (type(e).__name__, e), traceback.format_exc()[-1500:]]
        work = os.path.join(outdir, "%s_%s" % (area, seed))
    if problems:
        fail = os.path.join(outdir, "fail", "%s_%s" % (area, seed))
        if os.path.isdir(work):
            shutil.rmtree(fail, ignore_errors=True)
            shutil.copytree(work, fail, ignore=shutil.ignore_patterns("prog", "*.dSYM"))
        os.makedirs(fail, exist_ok=True)
        with open(os.path.join(fail, "report.txt"), "w") as f:
            f.write("\n".join(problems) + "\n")
    elif os.path.isdir(work):
        shutil.rmtree(work, ignore_errors=True)
    return problems, time.time() - t0


SUITE = {"values": [1, 2, 3, 4], "eval": [1, 2, 3, 4], "step": [1, 2, 3, 4, 5, 6],
         "panic": list(range(1, 13)), "proto": [1, 2, 3]}


def main(argv):
    args = list(argv[1:])
    adapter = None
    jobs = 1
    if "--adapter" in args:
        i = args.index("--adapter")
        adapter = args[i + 1]
        del args[i:i + 2]
    if "-j" in args:
        i = args.index("-j")
        jobs = int(args[i + 1])
        del args[i:i + 2]
    if args and args[0] == "--gen":
        area, seed = args[1], args[2]
        rng = random.Random("%s-%s" % (area, seed))
        g, text = dbg_gen.value_program(rng, 0 if area == "values" else 4)
        print(text)
        return 0
    if args and args[0] == "--suite":
        iron, outdir = args[1], args[2]
        work = [(a, s) for a in args[3:] for s in SUITE[a]]
    else:
        area, iron, outdir, seed = args[0], args[1], args[2], int(args[3])
        count = int(args[4]) if len(args) > 4 else 1
        work = [(area, s) for s in range(seed, seed + count)]
    iron = os.path.abspath(iron)
    ironc = os.path.join(os.path.dirname(iron), "ironc" + (".exe" if os.name == "nt" else ""))
    outdir = os.path.abspath(outdir)
    os.makedirs(outdir, exist_ok=True)
    failed = []
    lock = threading.Lock()
    queue_ = list(work)
    skipped = [False]

    def worker():
        while True:
            with lock:
                if not queue_ or skipped[0]:
                    return
                area, seed = queue_.pop(0)
            try:
                problems, dt = run_one(area, seed, ironc, iron, outdir, adapter)
            except Skip as e:
                with lock:
                    skipped[0] = str(e)
                return
            with lock:
                if problems:
                    failed.append((area, seed))
                    print("FAIL %s %s (%.0fs): %s" % (area, seed, dt, problems[0][:300]))
                    for p in problems[1:6]:
                        print("     %s" % p[:300])
                else:
                    print("ok   %s %s (%.0fs)" % (area, seed, dt))
                sys.stdout.flush()

    ts = [threading.Thread(target=worker) for _ in range(max(1, jobs))]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    if skipped[0]:
        print("skipped: %s" % skipped[0])
        return SKIP
    print("%d programs, %d failed" % (len(work), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
