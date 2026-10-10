#!/usr/bin/env python3
"""`iron dap` once the compiler's temporaries carry no debug info (#388).

Usage: dap_temporaries_client.py <temporaries.iron> <adapter command...>

temporaries.iron keeps xs = [10, 20, 30] as a C array (xs plus xs_len) and
has a closure that changes the var `count` it captures (reached through
the `_ref_count` pointer, which keeps its debug info). Checks:
  - inside the closure (line 17) the locals are d = 3, doubled = 8 and
    count = 4, and nothing starting with `_`;
  - in main (line 20) count = 4, xs reads as its three elements, xs_len is
    folded into it, and nothing starting with `_` is listed;
  - Iron expressions that read xs_len still evaluate:
    xs.len(), xs[2], r * 2. (A captured var by its name in an expression
    is not resolved yet, with or without the temporaries.)
Exit 0 on success, 1 on a failed check, 77 when no DAP debugger is found.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dap_eval_client import ev, fail, finish, next_stop, start  # noqa: E402


def main():
    src = os.path.abspath(sys.argv[1])
    c, _out = start(sys.argv[2:], src, [{"line": 17}, {"line": 20}])

    def variables(ref):
        return c.call("variables", {"variablesReference": ref})["variables"]

    def locals_of(top):
        out = {}
        for s in c.call("scopes", {"frameId": top["id"]})["scopes"]:
            if s["name"].lower() in ("locals", "arguments"):
                for v in variables(s["variablesReference"]):
                    out[v["name"]] = v
        return out

    def no_temporaries(where, loc):
        hidden = sorted(n for n in loc if n.startswith("_") or n.startswith("iron__"))
        if hidden:
            fail("compiler temporaries in the locals of %s: %s" % (where, hidden))

    tid, top = next_stop(c)
    if not top or top.get("line") != 17:
        fail("the first stop is %s, want the closure at line 17" % top)
    loc = locals_of(top)
    got = {n: loc[n]["value"] for n in ("d", "doubled", "count") if n in loc}
    if got != {"d": "3", "doubled": "8", "count": "4"}:
        fail("the closure's locals are %s, want d = 3, doubled = 8, count = 4" %
             {n: v["value"] for n, v in loc.items()})
    no_temporaries("the closure", loc)
    print("dap: the closure shows d, doubled and the captured count, no temporaries")

    c.call("continue", {"threadId": tid})
    tid, top = next_stop(c)
    if not top or top.get("line") != 20:
        fail("the second stop is %s, want main at line 20" % top)
    loc = locals_of(top)
    no_temporaries("main", loc)
    if loc.get("count", {}).get("value") != "4":
        fail("main's count is %s, want 4" % loc.get("count"))
    if "xs_len" in loc:
        fail("xs_len is listed next to xs, want it folded into the list")
    xs = loc.get("xs") or {}
    elems = [v["value"] for v in variables(xs["variablesReference"])] \
        if xs.get("variablesReference") else []
    if elems != ["10", "20", "30"]:
        fail("xs is %s with elements %s, want 10, 20, 30" % (xs.get("value"), elems))
    print("dap: main shows xs as a list and count, no temporaries")

    got = {e: ev(c, top, e) for e in ("xs.len()", "xs[2]", "r * 2")}
    if got != {"xs.len()": "3", "xs[2]": "30", "r * 2": "16"}:
        fail("Iron expressions in main: %s, want xs.len() = 3, xs[2] = 30, r * 2 = 16" % got)
    print("dap: Iron expressions read xs_len")
    finish(c)
    print("PASS")


if __name__ == "__main__":
    main()
