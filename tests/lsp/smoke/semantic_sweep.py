"""Semantic tokens and inlay hints over every program in tests/integration/v4.

Usage: semantic_sweep.py <ironls> <source root>

Every token must decode to an identifier spelled at its position, tokens
must be strictly ordered, every inlay hint must be a type (": T") or a
parameter name ("w:"),
and no request may fail. Runs on a plain Python 3 (no pytest-lsp).
"""
import glob
import json
import os
import re
import subprocess
import sys

ls, root = sys.argv[1], sys.argv[2]
proc = subprocess.Popen([ls], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL)


def send(obj):
    body = json.dumps(obj).encode()
    proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    proc.stdin.flush()


def recv():
    header = b""
    while not header.endswith(b"\r\n\r\n"):
        c = proc.stdout.read(1)
        if not c:
            sys.exit("ironls exited")
        header += c
    n = int([h for h in header.split(b"\r\n")
             if h.lower().startswith(b"content-length")][0].split(b":")[1])
    return json.loads(proc.stdout.read(n))


next_id = [10]


def request(method, params):
    next_id[0] += 1
    send({"jsonrpc": "2.0", "id": next_id[0], "method": method, "params": params})
    while True:
        msg = recv()
        if msg.get("id") == next_id[0]:
            if "error" in msg:
                sys.exit(f"{method} failed: {msg['error']}")
            return msg["result"]


send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
      "params": {"processId": None, "rootUri": None, "capabilities": {}}})
while recv().get("id") != 1:
    pass
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

ident = re.compile(r"[A-Za-z_][A-Za-z0-9_]*$")
files = sorted(glob.glob(os.path.join(root, "tests/integration/v4/**/*.iron"), recursive=True))
bad = tokens = hints = 0
for k, path in enumerate(files):
    with open(path, encoding="utf-8") as f:
        src = f.read()
    uri = "file:///sweep/%d.iron" % k
    send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
        "uri": uri, "languageId": "iron", "version": 1, "text": src}}})
    data = request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})["data"]
    lines = src.split("\n")
    line = ch = 0
    prev = (-1, -1)
    for i in range(0, len(data), 5):
        dl, dc, ln = data[i:i + 3]
        if dl:
            line += dl
            ch = dc
        else:
            ch += dc
        text = lines[line][ch:ch + ln] if line < len(lines) else ""
        tokens += 1
        if not ident.match(text) or (line, ch) <= prev:
            bad += 1
            print(f"bad token {path}:{line + 1}:{ch + 1} {text!r}")
        prev = (line, ch)
    for h in request("textDocument/inlayHint", {"textDocument": {"uri": uri}, "range": {
            "start": {"line": 0, "character": 0}, "end": {"line": len(lines), "character": 0}}}):
        hints += 1
        ok = (h["label"].startswith(": ") if h.get("kind") == 1
              else re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*:", h["label"]) is not None)
        if not ok:
            bad += 1
            print(f"bad hint {path}: {h}")
    send({"jsonrpc": "2.0", "method": "textDocument/didClose",
          "params": {"textDocument": {"uri": uri}}})

print(f"files={len(files)} tokens={tokens} hints={hints} bad={bad}")
if len(files) < 500:
    sys.exit("fewer than 500 v4 programs: wrong source root?")
sys.exit(1 if bad else 0)
