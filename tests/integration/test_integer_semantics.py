#!/usr/bin/env python3
"""Execute boundary cases against a Python integer oracle, not another build.

The optional clang shim instruments emitted C (not just the compiler itself).
Every local has two branch-dependent stores to keep range proofs useful while
preventing constant folding from hiding the read-width regression.
"""

import argparse
import difflib
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile


def fixture():
    functions, calls, expected = [], [], []

    def case(kind, x, y, expression, oracle):
        name = f"boundary_{len(functions)}"
        literal = lambda n: f"UInt({n})" if kind == "UInt" else str(n)
        functions.append(f"""
func {name}(flag: Bool) -> {kind} {{
    var x: {kind} = {literal(x)}
    var y: {kind} = {literal(y)}
    if flag {{ x = {literal(x + 1)} }} else {{ y = {literal(y + 1)} }}
    return {expression}
}}
""")
        for flag in (True, False):
            calls.append(f'println("{{{name}({str(flag).lower()})}}")')
            expected.append(str(oracle(x + int(flag), y + int(not flag))))

    mask = (1 << 64) - 1
    signed = lambda n: ((n + (1 << 63)) & mask) - (1 << 63)
    # Cross every narrow storage boundary, including negative values.
    for x in (126, 127, 254, 255, 32766, 32767, 50000, 65534, 65535,
              2147483646, 2147483647, -129, -32769, -2147483648):
        case("Int", x, x, "x * y", lambda a, b: signed(a * b))
        case("Int", x, x, "x + y", lambda a, b: signed(a + b))
    case("Int", -2147483648, 2147483646, "x - y", lambda a, b: a - b)
    case("Int", -2147483648, 0, "-x", lambda a, b: -a)
    case("Int", -32769, 1, "x >> y", lambda a, b: a >> b)
    case("Int", 1, 40, "x << y", lambda a, b: a << b)
    case("Int", 2147483646, 1, "(x + y) * (x - y)", lambda a, b: (a + b) * (a - b))
    # Reproducible property-style coverage for combinations not hand-picked
    # above, plus the 64-bit wrap boundary to preserve existing semantics.
    case("Int", 3037000499, 3037000500, "x * y", lambda a, b: signed(a * b))
    rng = random.Random(0x1707)
    for _ in range(24):
        x = rng.randint(-(1 << 31), (1 << 31) - 2)
        y = rng.randint(-(1 << 31), (1 << 31) - 2)
        case("Int", x, y, "x * y + x - y", lambda a, b: signed(a * b + a - b))
    for x in (254, 255, 32768, 65534, 65535, 2147483647, 4294967294):
        # Divide large unsigned results into printable signed range. Wrap is
        # computed explicitly in the oracle using arbitrary-precision ints.
        # UInt requires explicit construction; bitwise syntax is Int-only.
        case("UInt", x, x, "(x * y) / UInt(65536)", lambda a, b: ((a * b) & mask) >> 16)
        case("UInt", x, x, "x + y", lambda a, b: (a + b) & mask)
        case("UInt", x, x + 2, "(x - y) / UInt(4294967296)", lambda a, b: ((a - b) & mask) >> 32)
    functions.append("""
func snapshot(flag: Bool) -> Int {
    var x = 10
    if flag { x = 20 } else { x = 30 }
    val before = x * x + 1
    x = 40
    return before + x
}
""")
    calls += ['println("{snapshot(true)}")', 'println("{snapshot(false)}")']
    expected += ["441", "941"]
    # RawPtr.of materializes address-observable scalar storage. Widening
    # reads must not turn that storage into a cast rvalue or narrow its ABI.
    calls += ["var address_value: Int = 7",
              "val raw: RawPtr = RawPtr.of(address_value)",
              "val pointer: *unchecked Int = Ptr.cast[Int](raw)",
              'println("{pointer}")']
    expected += ["7"]
    return "\n".join(functions) + "\nfunc main() {\n" + "\n".join(calls) + "\n}\n", "\n".join(expected) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--mode", choices=("optimized", "noopt", "release"), default="optimized")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    compiler = str(Path(args.compiler).resolve())
    clang = shutil.which("clang")
    if not clang:
        parser.error("clang is required")
    source, expected = fixture()
    with tempfile.TemporaryDirectory(prefix="iron_integer_semantics_") as directory:
        root = Path(directory)
        (root / "boundary.iron").write_text(source)
        env = os.environ.copy()
        if args.sanitize:
            shim_dir = root / "tools"
            shim_dir.mkdir()
            shim = shim_dir / "clang"
            shim.write_text(f"#!{sys.executable}\nimport os, sys\nos.execv({clang!r}, [{clang!r}] + sys.argv[1:] + ['-fsanitize=undefined', '-fno-sanitize-recover=all'])\n")
            shim.chmod(0o755)
            env["PATH"] = str(shim_dir) + os.pathsep + env["PATH"]
            env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        command = [compiler, "build", "boundary.iron", "-o", "boundary"]
        if args.mode != "optimized":
            command.append("--no-optimize" if args.mode == "noopt" else "--release")
        build = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True, timeout=90)
        if build.returncode:
            sys.stderr.write(build.stdout + build.stderr)
            return 1
        run = subprocess.run([str(root / "boundary")], cwd=root, env=env,
                             capture_output=True, text=True, timeout=15)
        if run.returncode or run.stdout != expected or run.stderr:
            sys.stderr.write(run.stderr)
            sys.stderr.writelines(difflib.unified_diff(expected.splitlines(True), run.stdout.splitlines(True),
                                                      fromfile="integer oracle", tofile="Iron output"))
            print(f"program exit: {run.returncode}", file=sys.stderr)
            return 1
        print(f"PASS: {len(expected.splitlines())} integer/snapshot results ({args.mode}, UBSan={args.sanitize})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
