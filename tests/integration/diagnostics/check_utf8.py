#!/usr/bin/env python3
"""Run `ironc check` on a source and require its diagnostics to be valid
UTF-8 and to show the diagnostic WANT.

Usage: check_utf8.py <ironc> <source.iron> <WANT>
"""
import subprocess
import sys

r = subprocess.run([sys.argv[1], "check", sys.argv[2]], capture_output=True)
out = r.stdout + r.stderr
try:
    text = out.decode("utf-8")
except UnicodeDecodeError as e:
    print("FAIL: the output is not UTF-8: %s" % e)
    sys.exit(1)
if sys.argv[3] not in text:
    print("FAIL: no %s in the output:\n%s" % (sys.argv[3], text))
    sys.exit(1)
print("PASS")
