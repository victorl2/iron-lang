#!/usr/bin/env python3
"""`iron dap` on the parameters a --debug build cannot mark nodebug (#388).

Usage: dap_params_client.py <temporaries_params.iron> <adapter command...>

C has no way to leave a parameter out of the debug info, so the compiler
names the synthetic ones for iron dap instead. Checks, in the order the
program reaches them:
  - in bump(var n) (line 11) the locals are just n = 6: the pointer to the
    caller's binding is a hidden `_` name and the copy the body works on is
    `n` (not `n_2`);
  - in total(xs, scale) (line 6), whose list parameter arrives as a C array,
    xs reads as its elements [1, 2, 3] with xs_len folded into it, next to
    scale = 2 and t = 12; xs.len() and xs[1] evaluate;
  - inside the closure (line 19) the locals are d = 1 and the captured
    count and k, and not its environment parameter.
Nothing starting with `_` is listed anywhere.
Exit 0 on success, 1 on a failed check, 77 when no DAP debugger is found.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dap_eval_client import ev, fail, finish, next_stop, start  # noqa: E402


def main():
    src = os.path.abspath(sys.argv[1])
    c, _out = start(sys.argv[2:], src, [{"line": 6}, {"line": 11}, {"line": 19}])

    def variables(ref):
        return c.call("variables", {"variablesReference": ref})["variables"]

    def locals_of(top):
        out = {}
        for s in c.call("scopes", {"frameId": top["id"]})["scopes"]:
            if s["name"].lower() in ("locals", "arguments"):
                for v in variables(s["variablesReference"]):
                    out[v["name"]] = v
        return out

    def no_underscores(where, loc):
        hidden = sorted(n for n in loc if n.startswith("_") or n.startswith("iron__"))
        if hidden:
            fail("compiler names in the locals of %s: %s" % (where, hidden))

    tid, top = next_stop(c)
    if not top or top.get("line") != 11:
        fail("the first stop is %s, want bump at line 11" % top)
    loc = locals_of(top)
    no_underscores("bump", loc)
    if sorted(loc) != ["n"] or loc["n"]["value"] != "6":
        fail("bump's locals are %s, want just n = 6" %
             {n: v["value"] for n, v in loc.items()})
    print("dap: bump(var n) shows n = 6 once")

    c.call("continue", {"threadId": tid})
    tid, top = next_stop(c)
    if not top or top.get("line") != 6:
        fail("the second stop is %s, want total at line 6" % top)
    loc = locals_of(top)
    no_underscores("total", loc)
    if "xs_len" in loc:
        fail("xs_len is listed next to xs, want it folded into the list")
    xs = loc.get("xs") or {}
    elems = [v["value"] for v in variables(xs["variablesReference"])] \
        if xs.get("variablesReference") else []
    if elems != ["1", "2", "3"]:
        fail("xs is %s with elements %s, want 1, 2, 3" % (xs.get("value"), elems))
    got = {n: loc[n]["value"] for n in ("scale", "t") if n in loc}
    if got != {"scale": "2", "t": "12"}:
        fail("total's scale and t are %s, want 2 and 12" % got)
    got = {e: ev(c, top, e) for e in ("xs.len()", "xs[1]")}
    if got != {"xs.len()": "3", "xs[1]": "2"}:
        fail("Iron expressions in total: %s, want xs.len() = 3, xs[1] = 2" % got)
    print("dap: the list parameter xs shows as [1, 2, 3] and evaluates")

    c.call("continue", {"threadId": tid})
    tid, top = next_stop(c)
    if not top or top.get("line") != 19:
        fail("the third stop is %s, want the closure at line 19" % top)
    loc = locals_of(top)
    no_underscores("the closure", loc)
    got = {n: loc[n]["value"] for n in ("d", "count", "k") if n in loc}
    if got != {"d": "1", "count": "12", "k": "10"}:
        fail("the closure's locals are %s, want d = 1, count = 12, k = 10" %
             {n: v["value"] for n, v in loc.items()})
    print("dap: the closure shows d and the captured count and k, not its environment")
    finish(c)
    print("PASS")


if __name__ == "__main__":
    main()
