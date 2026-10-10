#!/usr/bin/env python3
"""The Iron expression evaluator of `iron dap` against a fake debugger:
parsing, precedence, Iron arithmetic and the list / map / string methods,
without starting a program.

Usage: test_iron_eval.py <src/debug/iron_dap.py>
"""
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("iron_dap", sys.argv[1])
dap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dap)

# What the debugger answers for a path (LLDB's summaries), and the children
# of a variablesReference.
VALUES = {"p": ("Point {x = 3, y = 4}", 1), "p.x": ("3", 0), "p.y": ("4", 0),
          "xs": ("[10, 20, 30]", 2), "xs[0]": ("10", 0), "xs[1]": ("20", 0), "xs[2]": ("30", 0),
          "name": ('"iron"', 3), "ages": ("size=1", 4), "maybe": ("7", 0), "none": ("null", 0),
          "flag": ("true", 0), "i": ("1", 0), "gl": ("[3]", 5), "carr": ("0x16fdfdce0", 0),
          "carr_len": ("2", 0), "carr[1]": ("8", 0)}
CHILDREN = {2: [{"name": "[%d]" % i, "value": v} for i, v in enumerate(("10", "20", "30"))],
            4: [{"name": '["ann"]', "value": "31"}],
            5: [{"name": "[%d]" % i, "value": v} for i, v in enumerate(('"a"', '"bb"', '"c"'))]}


class FakeDebugger:
    def request(self, command, args, timeout=10):
        if command == "evaluate":
            v = VALUES.get(args["expression"])
            if not v:
                return {"success": False, "message": "error: no such name"}
            return {"success": True, "body": {"result": v[0], "variablesReference": v[1]}}
        if command == "variables":
            return {"success": True,
                    "body": {"variables": CHILDREN.get(args["variablesReference"], [])}}
        return {"success": False}

    def struct_text(self, value, ref, type_name):
        return value


CASES = [
    ("xs.len()", "3"), ("len(xs)", "3"), ("xs.is_empty()", "false"), ("xs[1] * 2", "40"),
    ("xs[i] + xs[2]", "50"), ("xs.contains(20)", "true"), ("xs.contains(7)", "false"),
    ("name.len()", "4"), ('name == "iron"', "true"), ('name != "x"', "true"), ("name[1]", '"r"'),
    ('name + "!"', '"iron!"'), ('name.starts_with("ir")', "true"),
    ('ages.get("ann")', "31"), ('ages.has("bob")', "false"), ('ages.get_or("bob", 0)', "0"),
    ("p", "Point {x = 3, y = 4}"), ("p.x + p.y * 2", "11"), ("(p.x + p.y) * 2", "14"),
    ("p.x > 2 and flag", "true"), ("not flag", "false"), ("flag or 1 / 0 == 1", "true"),
    ("p.x > 2 && !flag", "false"), ("-7 / 2", "-3"), ("-7 % 2", "-1"), ("7 % -2", "1"),
    ("maybe != null", "true"), ("none == null", "true"), ("1 < 2 == true", "true"),
    ("2.5 * 2.0", "5.0"),
    # gdb's list form [3]: length and elements from the children.
    ("gl.len()", "3"), ("gl[1]", '"bb"'),
    # A list kept as a C array: a pointer and carr_len.
    ("carr.len()", "2"), ("carr[1] + 1", "9"),
]
ERRORS = [("nosuch", "no variable `nosuch` here"), ("p.z", "`p.z` has no value here"),
          ("xs[5]", "out of range"), ("1 / 0", "division by zero"), ("not 3", "expected a Bool"),
          ("p.x +", "expected more"), ("ages.get(\"zed\")", "no entry")]

failed = 0
for expr, want in CASES:
    try:
        got = dap.iron_evaluate(FakeDebugger(), 1, expr)[0]
    except dap.IronEvalError as e:
        got = "error: %s" % e
    if got != want:
        failed += 1
        print("FAIL: %s = %s, want %s" % (expr, got, want))
for expr, want in ERRORS:
    try:
        got = "value %s" % dap.iron_evaluate(FakeDebugger(), 1, expr)[0]
    except dap.IronEvalError as e:
        got = str(e)
    if want not in got:
        failed += 1
        print("FAIL: %s gives %r, want an error with %r" % (expr, got, want))
# The `test` launch argument: Zed's run button passes the block's string
# literal with its quotes ($ZED_CUSTOM_test_name), other clients the name.
TEST_NAMES = [('"area of a square"', "area of a square"), ("adds", "adds"),
              ('"says \\"hi\\""', 'says "hi"'), ('"', '"'), (None, None)]
for given, want in TEST_NAMES:
    got = dap.test_name(given)
    if got != want:
        failed += 1
        print("FAIL: test_name(%r) = %r, want %r" % (given, got, want))
# Frame names: a test block's C function is shown as the test.
FRAMES = [(("Iron_main", None), "main"), (("iron__test_1796", "adds"), 'test "adds"'),
          (("Iron_iron__test_12", None), "test"), (("__lambda_3", None), "func")]
for (c_name, test), want in FRAMES:
    got = dap.iron_function_name(c_name, test)
    if got != want:
        failed += 1
        print("FAIL: iron_function_name(%r, %r) = %r, want %r" % (c_name, test, got, want))
if failed:
    sys.exit(1)
print("iron eval: %d expressions, %d errors: PASS" % (len(CASES), len(ERRORS)))
