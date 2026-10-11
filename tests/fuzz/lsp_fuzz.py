#!/usr/bin/env python3
"""Editing-session fuzzer for ironls, the Iron language server.

Starts ironls, opens documents taken from the integration corpus and from
the program fuzzers' generators (prog_fuzz.py), and then plays a random
editing session against them: incremental edits (insert, delete and replace
random spans, typing a fragment one character at a time, pasting fragments
of other files, unbalanced braces, half-typed strings and interpolations,
text outside the BMP so positions fall on surrogate pairs), interleaved with
requests at random positions (hover, completion, definition, references,
rename, document symbols, folding ranges, semantic tokens, code actions,
formatting, signature help, inlay hints, pull diagnostics and more), plus
didClose/didOpen churn and $/cancelRequest.

Each session negotiates utf-16 or utf-8 positions. The checks:

  * ironls does not exit, crash or print a sanitizer report (run it against
    an ASan+UBSan build; stderr is scanned), and does not report an analysis
    crash through window/showMessage (the server recovers from an abort in
    a document worker, so a crash otherwise goes unseen).
  * every request is answered within the deadline (a hang otherwise);
  * every response is well formed: ranges lie inside the document and on
    character boundaries of the negotiated encoding, start <= end, edits do
    not overlap, semantic tokens decode, no diagnostic is published twice;
  * after an edit sequence settles, the published diagnostics agree with
    `ironc check` on the same text (see parity_check for what "agree" is).
  * shutdown/exit is clean.

Usage:
    lsp_fuzz.py --ironls B --ironc C [--seed S] [--sessions N] [--steps N]
                [-j N] [--out DIR] [--encoding utf-16|utf-8|both]
                [--corpus DIR ...] [--gen AREA ...] [--no-parity]
    lsp_fuzz.py --replay DIR/fail/<name>/session.jsonl --ironls B ...

A failing session is written to OUT/fail/<seed>/ with its message log
(session.jsonl, replayable with --replay), the server's stderr and a
summary. Sessions are deterministic for a seed (apart from timing).

Exit status 1 when any session failed.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import glob
import hashlib
import json
import os
import queue
import random
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))

SAN_RE = re.compile(r'content-hash drift|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|'
                    r'SUMMARY: UndefinedBehaviorSanitizer|internal compiler error|'
                    r'ironls: SIGABRT on doc|ERROR: ThreadSanitizer')

# Response error codes a request may legitimately get.
OK_ERRORS = {-32800, -32801, -32803}  # cancelled, content modified, request failed


class Finding(Exception):
    def __init__(self, kind, detail):
        super().__init__(f'{kind}: {detail}')
        self.kind = kind
        self.detail = detail


# ------------------------------------------------------------- text model

def split_lines(text):
    """Lines as LSP sees them: terminated by \\n, \\r\\n or \\r. Returns a
    list of (start_index, line_text_without_eol)."""
    out = []
    i = 0
    n = len(text)
    start = 0
    while i < n:
        c = text[i]
        if c == '\n' or c == '\r':
            out.append((start, text[start:i]))
            if c == '\r' and i + 1 < n and text[i + 1] == '\n':
                i += 1
            i += 1
            start = i
            continue
        i += 1
    out.append((start, text[start:]))
    return out


def units(ch, enc):
    o = ord(ch)
    if enc == 'utf-16':
        return 2 if o > 0xFFFF else 1
    if o < 0x80:
        return 1
    if o < 0x800:
        return 2
    if o < 0x10000:
        return 3
    return 4


def line_units(s, enc):
    return sum(units(c, enc) for c in s)


def boundaries(s, enc):
    """Unit offsets of every character boundary in line s."""
    b = [0]
    u = 0
    for c in s:
        u += units(c, enc)
        b.append(u)
    return b


class Doc:
    def __init__(self, uri, path, text, enc):
        self.uri = uri
        self.path = path
        self.text = text
        self.enc = enc
        self.version = 1
        self.open = True
        self._lines = None
        self.history = {1: text}   # version -> text

    @property
    def lines(self):
        if self._lines is None:
            self._lines = split_lines(self.text)
        return self._lines

    def set_text(self, t):
        self.text = t
        self._lines = None

    def pos_to_index(self, line, char):
        """String index of an LSP position on a character boundary."""
        ls = self.lines
        if line >= len(ls):
            return len(self.text)
        start, s = ls[line]
        u = 0
        for k, c in enumerate(s):
            if u >= char:
                return start + k
            u += units(c, self.enc)
        return start + len(s)

    def index_to_pos(self, idx):
        ls = self.lines
        lo, hi = 0, len(ls) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if ls[mid][0] <= idx:
                lo = mid
            else:
                hi = mid - 1
        start, s = ls[lo]
        k = min(idx - start, len(s))
        return lo, line_units(s[:k], self.enc)

    def valid_pos(self, p, allow_eof_line=True):
        """None when p is a valid position, else why not."""
        try:
            line, ch = p['line'], p['character']
        except (TypeError, KeyError):
            return f'malformed position {p!r}'
        if not isinstance(line, int) or not isinstance(ch, int) or line < 0 or ch < 0:
            return f'bad position {p!r}'
        ls = self.lines
        if line >= len(ls):
            return f'line {line} past the last line {len(ls) - 1}'
        s = ls[line][1]
        b = boundaries(s, self.enc)
        if ch > b[-1]:
            return f'character {ch} past the end of line {line} ({b[-1]} {self.enc} units)'
        if ch not in b:
            return f'character {ch} inside a character on line {line} ({self.enc})'
        return None

    def valid_range(self, r):
        try:
            s, e = r['start'], r['end']
        except (TypeError, KeyError):
            return f'malformed range {r!r}'
        why = self.valid_pos(s) or self.valid_pos(e)
        if why:
            return why
        if (s['line'], s['character']) > (e['line'], e['character']):
            return f'range start after end {r!r}'
        return None


def apply_edits(doc_text, edits, doc):
    """Apply non-overlapping TextEdits (LSP positions in doc's encoding) to
    doc_text (which must be doc.text). Raises Finding on overlap."""
    spans = []
    for e in edits:
        why = doc.valid_range(e.get('range'))
        if why:
            raise Finding('bad-edit-range', f'{why} in {e!r}')
        r = e['range']
        a = doc.pos_to_index(r['start']['line'], r['start']['character'])
        b = doc.pos_to_index(r['end']['line'], r['end']['character'])
        if not isinstance(e.get('newText'), str):
            raise Finding('bad-edit', f'newText missing in {e!r}')
        spans.append((a, b, e['newText']))
    spans.sort(key=lambda t: (t[0], t[1]))
    for (a1, b1, _), (a2, b2, _) in zip(spans, spans[1:]):
        if a2 < b1:
            raise Finding('overlapping-edits', f'{spans}')
    out = doc_text
    for a, b, t in reversed(spans):
        out = out[:a] + t + out[b:]
    return out


# ------------------------------------------------------------- jsonrpc

class Server:
    def __init__(self, cmd, env, log):
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, env=env)
        self.log = log
        self.next_id = 1
        self.responses = {}
        self.cv = threading.Condition()
        self.notes = []      # (method, params) from the server
        self.stderr_buf = []
        self.closed = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.err_reader = threading.Thread(target=self._read_err, daemon=True)
        self.err_reader.start()

    def _read_err(self):
        for line in iter(self.proc.stderr.readline, b''):
            self.stderr_buf.append(line.decode('utf-8', 'replace'))

    def _read(self):
        f = self.proc.stdout
        try:
            while True:
                length = None
                while True:
                    line = f.readline()
                    if not line:
                        raise EOFError
                    line = line.strip()
                    if not line:
                        break
                    k, _, v = line.partition(b':')
                    if k.strip().lower() == b'content-length':
                        length = int(v.strip())
                if length is None:
                    with self.cv:
                        self.notes.append(('<protocol>', 'frame without Content-Length'))
                        self.cv.notify_all()
                    continue
                body = f.read(length)
                if len(body) < length:
                    raise EOFError
                try:
                    msg = json.loads(body.decode('utf-8'))
                except (UnicodeDecodeError, json.JSONDecodeError) as e:
                    with self.cv:
                        self.notes.append(('<protocol>', f'bad json: {e}: {body[:200]!r}'))
                        self.cv.notify_all()
                    continue
                self._dispatch(msg)
        except (EOFError, ValueError, OSError):
            pass
        with self.cv:
            self.closed = True
            self.cv.notify_all()

    def _dispatch(self, msg):
        if 'method' in msg and 'id' in msg:
            # server -> client request: answer null
            self._write({'jsonrpc': '2.0', 'id': msg['id'], 'result': None}, log=False)
            with self.cv:
                self.notes.append((msg['method'], msg.get('params')))
                self.cv.notify_all()
            return
        if 'method' in msg:
            with self.cv:
                self.notes.append((msg['method'], msg.get('params')))
                self.cv.notify_all()
            return
        with self.cv:
            self.responses[msg.get('id')] = msg
            self.cv.notify_all()

    def _write(self, obj, log=True):
        if log:
            self.log.append(obj)
        data = json.dumps(obj, ensure_ascii=False).encode('utf-8', 'surrogatepass')
        try:
            self.proc.stdin.write(b'Content-Length: %d\r\n\r\n' % len(data) + data)
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def request(self, method, params):
        rid = self.next_id
        self.next_id += 1
        self._write({'jsonrpc': '2.0', 'id': rid, 'method': method, 'params': params})
        return rid

    def notify(self, method, params):
        self._write({'jsonrpc': '2.0', 'method': method, 'params': params})

    def wait(self, rid, timeout):
        end = time.monotonic() + timeout
        with self.cv:
            while rid not in self.responses:
                if self.closed:
                    return None
                left = end - time.monotonic()
                if left <= 0:
                    raise Finding('hang', f'no response to request {rid} in {timeout:.0f}s')
                self.cv.wait(left)
            return self.responses.pop(rid)

    def alive(self):
        return self.proc.poll() is None and not self.closed


# ------------------------------------------------------------- inputs

FRAGMENTS = [
    '{', '}', '(', ')', '[', ']', '"', "'", '"{', '}"', '{x}', '"a{', '\\', '\\n', '\\{',
    ',', '.', '..', ':', '::', ';', '->', '=>', '=', '==', '!', '?', '?.', '&', '|', '*', '/',
    '//', '/*', '*/', '#', '@', '-', '+', '<', '>', '<T>', '0', '1_000', '0x', '0b2', '1.5e',
    'val ', 'var ', 'func ', 'object ', 'enum ', 'interface ', 'if ', 'else ', 'elif ', 'for ',
    'while ', 'match ', 'return ', 'import ', 'pub ', 'heap ', 'rc ', 'weak ', 'defer ', 'spawn ',
    'await ', 'comptime ', 'arena ', 'in ', 'leak ', 'drop ', 'copy ', 'test ', 'self', 'self.',
    'Int', 'String', 'Bool', 'Float', '[Int]', 'Map', '?Int', '*Int', 'println(', 'print("',
    'func f(a: Int) -> Int {\n  return a\n}\n', 'val x = ', 'x.', 'x.len()', 'main', 'Some',
    '\n', '\n\n', '\t', '    ', ' ', '\r\n',
    'é', 'ñ', 'ü', '中文', '𝕏', '😀', '👍🏽', '​', 'é', ' ', 'ß', 'Ω', ' ',
    '"😀"', '"{😀}"', '-- 😀 comment\n', '-- c', '///', '/// doc\n', '-- @file: other.iron\n',
    '"""', '"""\nraw {x}\n"""', '"{"{1}"}"', '"\\{"',
]


def corpus_files(dirs):
    out = []
    for d in dirs:
        if os.path.isfile(d):
            out.append(d)
            continue
        out += glob.glob(os.path.join(d, '**', '*.iron'), recursive=True)
    out = [f for f in sorted(set(out)) if os.path.getsize(f) < 60000]
    return out


def gen_program(area, seed):
    sys.path.insert(0, HERE)
    import prog_fuzz  # noqa: E402
    src, _ = prog_fuzz.make_program(area, seed)
    return src


def read_text(path):
    with open(path, 'rb') as f:
        data = f.read()
    return data.decode('utf-8', 'replace')


# ------------------------------------------------------------- validation

def check_range(doc, r, what):
    why = doc.valid_range(r)
    if why:
        raise Finding('bad-range', f'{what}: {why}')


def check_location(doc, loc, what):
    if not isinstance(loc, dict):
        raise Finding('malformed', f'{what}: location {loc!r}')
    uri = loc.get('uri') or loc.get('targetUri')
    if not isinstance(uri, str):
        raise Finding('malformed', f'{what}: no uri in {loc!r}')
    if uri == doc.uri:
        if 'targetUri' in loc:
            check_range(doc, loc.get('targetRange'), what)
            check_range(doc, loc.get('targetSelectionRange'), what)
        else:
            check_range(doc, loc.get('range'), what)


def check_workspace_edit(doc, we, what):
    if we is None:
        return
    if not isinstance(we, dict):
        raise Finding('malformed', f'{what}: {we!r}')
    changes = we.get('changes') or {}
    for uri, edits in changes.items():
        if uri == doc.uri:
            apply_edits(doc.text, edits, doc)
    for dc in we.get('documentChanges') or []:
        if isinstance(dc, dict) and 'edits' in dc and dc.get('textDocument', {}).get('uri') == doc.uri:
            apply_edits(doc.text, dc['edits'], doc)


def check_symbols(doc, syms, what, depth=0):
    if depth > 200:
        raise Finding('malformed', f'{what}: symbol nesting deeper than 200')
    for s in syms:
        if not isinstance(s, dict) or not isinstance(s.get('name'), str):
            raise Finding('malformed', f'{what}: symbol {s!r}')
        if not s['name']:
            raise Finding('malformed', f'{what}: empty symbol name {s!r}')
        if 'location' in s:
            check_location(doc, s['location'], what)
            continue
        check_range(doc, s.get('range'), what)
        check_range(doc, s.get('selectionRange'), what)
        r, sr = s['range'], s['selectionRange']
        k = lambda p: (p['line'], p['character'])  # noqa: E731
        if not (k(r['start']) <= k(sr['start']) and k(sr['end']) <= k(r['end'])):
            raise Finding('bad-symbol', f'{what}: selectionRange outside range in {s.get("name")!r}: {r} {sr}')
        check_symbols(doc, s.get('children') or [], what, depth + 1)


def check_diagnostics(doc, items, what):
    seen = set()
    for d in items:
        if not isinstance(d, dict) or not isinstance(d.get('message'), str):
            raise Finding('malformed', f'{what}: diagnostic {d!r}')
        check_range(doc, d.get('range'), what)
        key = (json.dumps(d['range'], sort_keys=True), d.get('severity'), d.get('code'), d['message'])
        if key in seen:
            raise Finding('duplicate-diagnostic', f'{what}: {d!r}')
        seen.add(key)


def check_semantic_tokens(doc, res, legend_n, what):
    if res is None:
        return
    data = res.get('data') if isinstance(res, dict) else None
    if not isinstance(data, list) or len(data) % 5:
        raise Finding('malformed', f'{what}: semantic tokens {str(res)[:200]}')
    line = 0
    ch = 0
    prev_end = None
    ls = doc.lines
    for i in range(0, len(data), 5):
        dl, dc, ln, tt, _mods = data[i:i + 5]
        if dl < 0 or dc < 0 or ln <= 0:
            raise Finding('bad-semantic-token', f'{what}: token {i // 5} {data[i:i + 5]}')
        if dl:
            line += dl
            ch = dc
            prev_end = None
        else:
            ch += dc
        if line >= len(ls):
            raise Finding('bad-semantic-token', f'{what}: token {i // 5} on line {line} past EOF')
        b = boundaries(ls[line][1], doc.enc)
        if ch not in b or ch + ln not in b:
            raise Finding('bad-semantic-token',
                          f'{what}: token {i // 5} at {line}:{ch} len {ln} not on character '
                          f'boundaries of line {line} ({b[-1]} units) {ls[line][1]!r}')
        if prev_end is not None and ch < prev_end:
            raise Finding('bad-semantic-token', f'{what}: token {i // 5} overlaps the previous one at {line}:{ch}')
        if legend_n and tt >= legend_n:
            raise Finding('bad-semantic-token', f'{what}: token type {tt} outside the legend')
        prev_end = ch + ln


def check_result(doc, method, params, res, ctx):
    """Validate a successful result of `method`."""
    what = method
    if method == 'textDocument/hover':
        if res is None:
            return
        if not isinstance(res, dict) or 'contents' not in res:
            raise Finding('malformed', f'{what}: {res!r}')
        if 'range' in res:
            check_range(doc, res['range'], what)
    elif method == 'textDocument/completion':
        items = res.get('items') if isinstance(res, dict) else res
        if items is None:
            return
        if not isinstance(items, list):
            raise Finding('malformed', f'{what}: {str(res)[:200]}')
        for it in items:
            if not isinstance(it, dict) or not isinstance(it.get('label'), str):
                raise Finding('malformed', f'{what}: item {it!r}')
            te = it.get('textEdit')
            if te:
                for k in ('range', 'insert', 'replace'):
                    if k in te:
                        check_range(doc, te[k], what + ' textEdit')
            for e in it.get('additionalTextEdits') or []:
                check_range(doc, e.get('range'), what + ' additionalTextEdits')
        if items:
            ctx['last_completion'] = items[0]
    elif method in ('textDocument/definition', 'textDocument/declaration',
                    'textDocument/typeDefinition', 'textDocument/implementation',
                    'textDocument/references'):
        if res is None:
            return
        locs = res if isinstance(res, list) else [res]
        for loc in locs:
            check_location(doc, loc, what)
    elif method == 'textDocument/documentHighlight':
        for h in res or []:
            check_range(doc, h.get('range'), what)
    elif method == 'textDocument/rename':
        check_workspace_edit(doc, res, what)
    elif method == 'textDocument/prepareRename':
        if res is None:
            return
        if 'range' in res:
            check_range(doc, res['range'], what)
        elif 'start' in res:
            check_range(doc, res, what)
    elif method == 'textDocument/documentSymbol':
        check_symbols(doc, res or [], what)
    elif method == 'workspace/symbol':
        for s in res or []:
            if isinstance(s, dict) and isinstance(s.get('location'), dict) and 'range' in s['location']:
                check_location(doc, s['location'], what)
    elif method == 'textDocument/foldingRange':
        n = len(doc.lines)
        for fr in res or []:
            sl, el = fr.get('startLine'), fr.get('endLine')
            if not isinstance(sl, int) or not isinstance(el, int) or not 0 <= sl <= el < n:
                raise Finding('bad-folding-range', f'{what}: {fr!r} with {n} lines')
    elif method == 'textDocument/semanticTokens/full':
        check_semantic_tokens(doc, res, ctx.get('legend_n'), what)
    elif method == 'textDocument/codeAction':
        for a in res or []:
            if not isinstance(a, dict) or not isinstance(a.get('title'), str):
                raise Finding('malformed', f'{what}: {a!r}')
            check_workspace_edit(doc, a.get('edit'), what)
        if res:
            ctx['last_action'] = res[0]
    elif method in ('textDocument/formatting', 'textDocument/rangeFormatting',
                    'textDocument/onTypeFormatting'):
        if res is None:
            return
        if not isinstance(res, list):
            raise Finding('malformed', f'{what}: {str(res)[:200]}')
        out = apply_edits(doc.text, res, doc)
        if method == 'textDocument/formatting':
            ctx['formatted'] = out
    elif method == 'textDocument/signatureHelp':
        if res is None:
            return
        sigs = res.get('signatures')
        if not isinstance(sigs, list):
            raise Finding('malformed', f'{what}: {res!r}')
        a = res.get('activeSignature')
        if sigs and a is not None and not 0 <= a < len(sigs):
            raise Finding('bad-signature-help', f'{what}: activeSignature {a} of {len(sigs)}')
        for s in sigs:
            lab = s.get('label')
            if not isinstance(lab, str):
                raise Finding('malformed', f'{what}: {s!r}')
            for p in s.get('parameters') or []:
                pl = p.get('label')
                if isinstance(pl, list):
                    if len(pl) != 2 or not 0 <= pl[0] <= pl[1] <= len(lab.encode('utf-16-le')) // 2:
                        raise Finding('bad-signature-help', f'{what}: parameter label {pl} in {lab!r}')
    elif method == 'textDocument/inlayHint':
        rng = params['range']
        for h in res or []:
            why = doc.valid_pos(h.get('position'))
            if why:
                raise Finding('bad-range', f'{what}: {why}')
    elif method == 'textDocument/selectionRange':
        if res is None:
            return
        if len(res) != len(params['positions']):
            raise Finding('bad-selection-range', f'{what}: {len(res)} results for {len(params["positions"])} positions')
        k = lambda p: (p['line'], p['character'])  # noqa: E731
        for sr in res:
            depth = 0
            while sr:
                check_range(doc, sr.get('range'), what)
                par = sr.get('parent')
                if par:
                    check_range(doc, par.get('range'), what)
                    a, b = sr['range'], par['range']
                    if not (k(b['start']) <= k(a['start']) and k(a['end']) <= k(b['end'])):
                        raise Finding('bad-selection-range', f'{what}: parent {b} does not contain {a}')
                sr = par
                depth += 1
                if depth > 500:
                    raise Finding('bad-selection-range', f'{what}: parent chain deeper than 500')
    elif method == 'textDocument/diagnostic':
        if not isinstance(res, dict) or res.get('kind') not in ('full', 'unchanged'):
            raise Finding('malformed', f'{what}: {str(res)[:200]}')
        if res['kind'] == 'full':
            check_diagnostics(doc, res.get('items') or [], what)
    elif method == 'textDocument/prepareTypeHierarchy':
        for it in res or []:
            check_location(doc, {'uri': it.get('uri'), 'range': it.get('range')}, what)
            if it.get('uri') == doc.uri:
                check_range(doc, it.get('selectionRange'), what)
        if res:
            ctx['last_type_item'] = res[0]


# ------------------------------------------------------------- parity

DIAG_HEAD = re.compile(r'^(error|warning|note)\[([EW]\d{4})\]: (.*)$')
DIAG_LOC = re.compile(r'^\s+--> (.*):(\d+):(\d+)$')


def ironc_check(ironc, path, env):
    try:
        p = subprocess.run([ironc, 'check', path], capture_output=True, timeout=120, env=env)
    except subprocess.TimeoutExpired:
        raise Finding('ironc-hang', f'ironc check {path} timed out')
    err = p.stderr.decode('utf-8', 'replace')
    if p.returncode not in (0, 1) or SAN_RE.search(err):
        raise Finding('ironc-crash', f'ironc check exit {p.returncode}: {err[-2000:]}')
    out = []
    cur = None
    for line in err.splitlines():
        m = DIAG_HEAD.match(line)
        if m:
            cur = [m.group(1), m.group(2), m.group(3)]
            continue
        m = DIAG_LOC.match(line)
        if m and cur:
            out.append((cur[0], cur[1], cur[2], m.group(1), int(m.group(2)), int(m.group(3))))
            cur = None
    return p.returncode, out


SEV = {1: 'error', 2: 'warning', 3: 'note', 4: 'note'}


def parity_check(doc, lsp_diags, ironc, env):
    """Compare the diagnostics ironls published for doc (at its current
    version) with `ironc check` on the same text.

    The two differ by design on a file that does not parse: `ironc check`
    reports only the first error of a broken statement and drops the
    statement, while the language server reports every recovery error and
    keeps the partial tree (so the follow-on errors differ both ways).
    What must hold:
      * when ironc reports a syntax error (a code below E0200), the server
        reports the first of them too (same code and start);
      * when ironc reports none, both report the same diagnostics (code,
        severity, start position and message);
      * the server reports an error exactly when ironc does.
    Returns None or a description of the mismatch."""
    with open(doc.path, 'w', encoding='utf-8', newline='', errors='surrogatepass') as f:
        f.write(doc.text)
    rc, cd = ironc_check(ironc, doc.path, env)
    mine = [d for d in cd if os.path.realpath(d[3]) == os.path.realpath(doc.path)]
    raw = doc.text.encode('utf-8', 'surrogatepass')
    blines = raw.split(b'\n')

    def to_pos(line, col):
        # a byte column inside a character moves to its end, as the server
        # converts it
        ln = line - 1
        if ln < 0:
            return (0, 0)
        if ln >= len(blines):
            ln = len(blines) - 1
            col = len(blines[ln]) + 1
        b = blines[ln]
        if b.endswith(b'\r'):
            b = b[:-1]
        k = min(max(col - 1, 0), len(b))
        while k < len(b) and (b[k] & 0xC0) == 0x80:
            k += 1
        return (ln, line_units(b[:k].decode('utf-8', 'replace'), doc.enc))

    def code_of(sev, code):
        if sev == 'warning' and isinstance(code, str) and code.startswith('E'):
            return 'W' + code[1:]
        return code

    c_full = {(d[1], d[0], to_pos(d[4], d[5]), d[2]) for d in mine}
    l_full = set()
    for d in lsp_diags:
        sev = SEV.get(d.get('severity'), 'error')
        st = d['range']['start']
        l_full.add((code_of(sev, d.get('code')), sev, (st['line'], st['character']), d['message']))
    c_err = any(d[0] == 'error' for d in mine) or (rc != 0 and not cd)
    l_err = any(d.get('severity', 1) == 1 for d in lsp_diags)
    if c_err != l_err:
        return f'ironc errors={c_err} server errors={l_err} (ironc: {sorted(c_full)}, server: {sorted(l_full)})'
    c_syn = {(c, p) for c, sv, p, m in c_full if c.startswith('E') and int(c[1:]) < 200}
    l_syn = {(c, p) for c, sv, p, m in l_full if isinstance(c, str) and c.startswith('E') and int(c[1:]) < 200}
    if c_syn:
        # Recovery differs after the first error (the CLI stops reporting
        # inside a broken statement, so the parse of what follows can be
        # reported differently): the first syntax error must agree.
        # the first one ironc reports (lexer errors come before parser ones)
        first = next((d[1], to_pos(d[4], d[5])) for d in mine
                     if d[1].startswith('E') and int(d[1][1:]) < 200)
        if first not in l_syn:
            return f'the server does not report the first syntax error {first} (server: {sorted(l_syn)[:12]})'
        return None
    if c_full != l_full:
        return (f'no syntax error, but the diagnostics differ: only ironc {sorted(c_full - l_full)[:8]}, '
                f'only the server {sorted(l_full - c_full)[:8]}')
    return None


# ------------------------------------------------------------- session

METHODS_POS = ['textDocument/hover', 'textDocument/completion', 'textDocument/definition',
               'textDocument/declaration', 'textDocument/typeDefinition',
               'textDocument/implementation', 'textDocument/references',
               'textDocument/documentHighlight', 'textDocument/prepareRename',
               'textDocument/rename', 'textDocument/signatureHelp',
               'textDocument/onTypeFormatting', 'textDocument/selectionRange',
               'textDocument/prepareTypeHierarchy']
METHODS_DOC = ['textDocument/documentSymbol', 'textDocument/foldingRange',
               'textDocument/semanticTokens/full', 'textDocument/formatting',
               'textDocument/diagnostic', 'textDocument/codeAction',
               'textDocument/rangeFormatting', 'textDocument/inlayHint',
               'textDocument/semanticTokens/full/delta', 'textDocument/semanticTokens/range',
               'workspace/symbol', 'textDocument/documentLink', 'textDocument/codeLens']


class Session:
    def __init__(self, args, seed, files):
        self.args = args
        self.seed = seed
        self.rng = random.Random(seed)
        self.files = files
        self.enc = args.encoding if args.encoding != 'both' else self.rng.choice(['utf-16', 'utf-8'])
        self.log = []
        self.tmp = tempfile.mkdtemp(prefix=f'lspfz{seed}_', dir=args.tmp)
        self.docs = []
        self.ctx = {}
        self.stats = {'requests': 0, 'edits': 0, 'parity': 0, 'parity_skipped': 0}
        self.pending = []   # (rid, method, params, doc, version, sent_at)

    # ---- inputs
    def pick_source(self):
        r = self.rng
        if self.args.gen and (not self.files or r.random() < 0.4):
            area = r.choice(self.args.gen)
            try:
                return f'gen_{area}', gen_program(area, r.randrange(1 << 30))
            except Exception:
                pass
        f = r.choice(self.files)
        return os.path.basename(f), read_text(f)

    def fragment(self):
        # a lone \r would be a line break to LSP but not to every reader;
        # keep \r only in \r\n pairs
        return re.sub(r'\r(?!\n)', '', self._fragment())

    def _fragment(self):
        r = self.rng
        k = r.random()
        if k < 0.55:
            return r.choice(FRAGMENTS)
        if k < 0.85 and self.files:
            t = read_text(r.choice(self.files))
            if not t:
                return 'x'
            a = r.randrange(len(t))
            return t[a:a + r.choice([1, 3, 10, 40, 120, 400])]
        if k < 0.92:
            return ''.join(r.choice('abcxyz_019 .(){}"\n') for _ in range(r.randint(1, 12)))
        return chr(r.choice([0x1F600, 0x10000, 0x1D54F, 0xFFFF, 0xFEFF, 0x7F, 0x1, 0x85, 0x2029]))

    def rand_index(self, doc):
        r = self.rng
        n = len(doc.text)
        if n == 0 or r.random() < 0.03:
            return 0 if r.random() < 0.5 else n
        i = r.randrange(n + 1)
        # never split a \r\n pair (an edit there would make a lone \r)
        if 0 < i < n and doc.text[i - 1] == '\r' and doc.text[i] == '\n':
            i -= 1
        return i

    def rand_pos(self, doc):
        """A request position: mostly valid, sometimes inside a character
        or past the end of a line or of the document."""
        r = self.rng
        k = r.random()
        if k < 0.80:
            return dict(zip(('line', 'character'), doc.index_to_pos(self.rand_index(doc))))
        ls = doc.lines
        line = r.randrange(len(ls))
        s = ls[line][1]
        if k < 0.90:
            b = boundaries(s, doc.enc)
            inside = [u for u in range(b[-1]) if u not in b]
            if inside:
                return {'line': line, 'character': r.choice(inside)}
            return {'line': line, 'character': b[-1] + r.randint(1, 50)}
        if k < 0.96:
            return {'line': line, 'character': line_units(s, doc.enc) + r.choice([1, 2, 100, 1 << 20])}
        return {'line': len(ls) + r.choice([0, 1, 5, 1 << 20]), 'character': r.choice([0, 3])}

    # ---- protocol
    def start(self):
        env = dict(os.environ)
        env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=0:symbolize=1')
        env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
        env['HOME'] = self.tmp
        env['XDG_STATE_HOME'] = os.path.join(self.tmp, 'state')
        self.env = env
        self.srv = Server([self.args.ironls], env, self.log)
        caps = {
            'general': {'positionEncodings': [self.enc]},
            'textDocument': {
                'publishDiagnostics': {'relatedInformation': True},
                'synchronization': {'didSave': True},
                'completion': {'completionItem': {'snippetSupport': True}},
                'hover': {'contentFormat': ['markdown', 'plaintext']},
                'semanticTokens': {'requests': {'full': {'delta': True}, 'range': True},
                                   'tokenTypes': [], 'tokenModifiers': [], 'formats': ['relative']},
            },
            'workspace': {'didChangeWatchedFiles': {'dynamicRegistration': True},
                          'workspaceFolders': True},
        }
        root = 'file://' + self.tmp
        rid = self.srv.request('initialize', {
            'processId': os.getpid(), 'rootUri': root, 'capabilities': caps,
            'workspaceFolders': [{'uri': root, 'name': 'ws'}],
            'clientInfo': {'name': 'lsp_fuzz', 'version': '1'}})
        res = self.srv.wait(rid, self.args.deadline)
        if res is None or 'result' not in res:
            raise Finding('init', f'initialize failed: {res!r}')
        caps = res['result'].get('capabilities', {})
        if caps.get('positionEncoding', 'utf-16') != self.enc:
            raise Finding('init', f'asked for {self.enc}, server chose {caps.get("positionEncoding")}')
        legend = (caps.get('semanticTokensProvider') or {}).get('legend') or {}
        self.ctx['legend_n'] = len(legend.get('tokenTypes') or [])
        self.srv.notify('initialized', {})

    def open_doc(self, name=None, text=None):
        if text is None:
            name, text = self.pick_source()
        k = len(self.docs)
        sub = os.path.join(self.tmp, f'd{k}')
        os.makedirs(sub, exist_ok=True)
        base = re.sub(r'[^A-Za-z0-9_.]', '_', name)
        if not base.endswith('.iron'):
            base += '.iron'
        path = os.path.join(sub, base)
        with open(path, 'w', encoding='utf-8', newline='', errors='surrogatepass') as f:
            f.write(text)
        doc = Doc('file://' + path, path, text, self.enc)
        self.docs.append(doc)
        self.srv.notify('textDocument/didOpen', {'textDocument': {
            'uri': doc.uri, 'languageId': 'iron', 'version': doc.version, 'text': doc.text}})
        return doc

    def send_change(self, doc, changes):
        doc.version += 1
        doc.history[doc.version] = doc.text
        doc.history.pop(doc.version - 300, None)
        # contentHash is ironls's own extension: it logs a drift when its
        # copy of the text differs from the client's (checked in stderr)
        self.srv.notify('textDocument/didChange', {
            'textDocument': {'uri': doc.uri, 'version': doc.version},
            'contentChanges': changes,
            'contentHash': hashlib.sha256(doc.text.encode('utf-8', 'surrogatepass')).hexdigest()})
        self.stats['edits'] += 1

    def edit_change(self, doc, a, b, new):
        """Change replacing doc.text[a:b] with new; updates the model."""
        sl, sc = doc.index_to_pos(a)
        el, ec = doc.index_to_pos(b)
        ch = {'range': {'start': {'line': sl, 'character': sc}, 'end': {'line': el, 'character': ec}},
              'text': new}
        doc.set_text(doc.text[:a] + new + doc.text[b:])
        return ch

    def random_edit(self, doc):
        r = self.rng
        k = r.random()
        if k < 0.30:      # insert
            i = self.rand_index(doc)
            self.send_change(doc, [self.edit_change(doc, i, i, self.fragment())])
        elif k < 0.50:    # delete a span
            a = self.rand_index(doc)
            b = min(len(doc.text), a + r.choice([1, 1, 2, 5, 20, 80, 400]))
            if 0 < b < len(doc.text) and doc.text[b - 1] == '\r' and doc.text[b] == '\n':
                b += 1
            self.send_change(doc, [self.edit_change(doc, a, b, '')])
        elif k < 0.62:    # replace
            a = self.rand_index(doc)
            b = min(len(doc.text), a + r.randint(0, 30))
            if 0 < b < len(doc.text) and doc.text[b - 1] == '\r' and doc.text[b] == '\n':
                b += 1
            self.send_change(doc, [self.edit_change(doc, a, b, self.fragment())])
        elif k < 0.80:    # type a fragment one character at a time, with requests
            i = self.rand_index(doc)
            frag = self.fragment()[:60]
            for c in frag:
                if c == '\r':
                    continue
                self.send_change(doc, [self.edit_change(doc, i, i, c)])
                i += 1
                if r.random() < 0.3:
                    meth = r.choice(['textDocument/completion', 'textDocument/signatureHelp',
                                     'textDocument/hover', 'textDocument/semanticTokens/full'])
                    l, ch = doc.index_to_pos(i)
                    self.send_request(doc, meth, pos={'line': l, 'character': ch})
                if r.random() < 0.1:
                    self.drain()
        elif k < 0.88:    # several changes in one notification
            changes = []
            for _ in range(r.randint(2, 5)):
                a = self.rand_index(doc)
                b = min(len(doc.text), a + r.randint(0, 10))
                if 0 < b < len(doc.text) and doc.text[b - 1] == '\r' and doc.text[b] == '\n':
                    b += 1
                changes.append(self.edit_change(doc, a, b, self.fragment()))
            self.send_change(doc, changes)
        elif k < 0.92:    # full replacement
            if r.random() < 0.5:
                _, t = self.pick_source()
            else:
                t = doc.text
            doc.set_text(t)
            self.send_change(doc, [{'text': t}])
        elif k < 0.96:    # unbalanced brace or quote at a line start
            i = self.rand_index(doc)
            l, _ = doc.index_to_pos(i)
            j = doc.lines[l][0]
            self.send_change(doc, [self.edit_change(doc, j, j, r.choice(['{', '}', '"', '(', '/*', '"{']))])
        else:             # move a line
            ls = doc.lines
            l = r.randrange(len(ls))
            a = ls[l][0]
            b = ls[l + 1][0] if l + 1 < len(ls) else len(doc.text)
            seg = doc.text[a:b]
            c1 = self.edit_change(doc, a, b, '')
            j = self.rand_index(doc)
            c2 = self.edit_change(doc, j, j, seg)
            self.send_change(doc, [c1, c2])

    def params_for(self, doc, method, pos=None):
        td = {'uri': doc.uri}
        r = self.rng
        if pos is None:
            pos = self.rand_pos(doc)
        if method in METHODS_POS:
            p = {'textDocument': td, 'position': pos}
            if method == 'textDocument/rename':
                p['newName'] = r.choice(['renamed', 'x', 'a1', '😀', '', 'val', 'two words', 'x' * 300])
            elif method == 'textDocument/references':
                p['context'] = {'includeDeclaration': r.random() < 0.5}
            elif method == 'textDocument/onTypeFormatting':
                p['ch'] = r.choice(['}', '\n', ';'])
                p['options'] = {'tabSize': 4, 'insertSpaces': True}
            elif method == 'textDocument/selectionRange':
                p = {'textDocument': td, 'positions': [pos] + [self.rand_pos(doc) for _ in range(r.randint(0, 3))]}
            elif method == 'textDocument/completion' and r.random() < 0.5:
                p['context'] = {'triggerKind': 2, 'triggerCharacter': r.choice(['.', ':', '/'])}
            elif method == 'textDocument/signatureHelp' and r.random() < 0.5:
                p['context'] = {'triggerKind': 2, 'triggerCharacter': r.choice(['(', ',']),
                                'isRetrigger': False}
            return p
        if method == 'workspace/symbol':
            return {'query': r.choice(['', 'main', 'a', 'x', 'S', '😀'])}
        p = {'textDocument': td}
        if method in ('textDocument/formatting', 'textDocument/rangeFormatting'):
            p['options'] = {'tabSize': r.choice([2, 4, 8]), 'insertSpaces': r.random() < 0.8,
                            'trimTrailingWhitespace': True}
        if method in ('textDocument/rangeFormatting', 'textDocument/codeAction',
                      'textDocument/inlayHint', 'textDocument/semanticTokens/range'):
            a, b = sorted([self.rand_pos(doc), self.rand_pos(doc)],
                          key=lambda q: (q['line'], q['character']))
            p['range'] = {'start': a, 'end': b}
        if method == 'textDocument/codeAction':
            p['context'] = {'diagnostics': self.ctx.get('diags', {}).get(doc.uri, [])[:3]}
        if method == 'textDocument/semanticTokens/full/delta':
            p['previousResultId'] = r.choice(['1', 'v1', '', 'x'])
        return p

    def send_request(self, doc, method=None, pos=None, cancel=False):
        r = self.rng
        if method is None:
            k = r.random()
            if k < 0.04 and self.ctx.get('last_completion'):
                method = 'completionItem/resolve'
            elif k < 0.06 and self.ctx.get('last_action'):
                method = 'codeAction/resolve'
            elif k < 0.07 and self.ctx.get('last_type_item'):
                method = r.choice(['typeHierarchy/supertypes', 'typeHierarchy/subtypes'])
            elif k < 0.55:
                method = r.choice(METHODS_POS)
            else:
                method = r.choice(METHODS_DOC)
        if method == 'completionItem/resolve':
            params = self.ctx['last_completion']
        elif method == 'codeAction/resolve':
            params = self.ctx['last_action']
        elif method.startswith('typeHierarchy/'):
            params = {'item': self.ctx['last_type_item']}
        else:
            params = self.params_for(doc, method, pos)
        rid = self.srv.request(method, params)
        self.stats['requests'] += 1
        # the history dict of this incarnation of the document: a reopen
        # starts a new one, and its versions repeat the old numbers
        self.pending.append((rid, method, params, doc, doc.version, doc.text, doc.open,
                             time.monotonic(), doc.history))
        if cancel or r.random() < 0.07:
            self.srv.notify('$/cancelRequest', {'id': rid})
        return rid

    def drain(self, all_=False):
        """Collect answered requests and validate them. With all_, wait for
        every pending request (deadline each)."""
        keep = []
        for item in self.pending:
            rid, method, params, doc, version, text, was_open, sent, _hist = item
            if not all_:
                with self.srv.cv:
                    if rid not in self.srv.responses:
                        if time.monotonic() - sent > self.args.deadline:
                            raise Finding('hang', f'{method} (id {rid}) unanswered after {self.args.deadline:.0f}s')
                        keep.append(item)
                        continue
            left = max(1.0, self.args.deadline - (time.monotonic() - sent))
            resp = self.srv.wait(rid, left)
            if resp is None:
                raise Finding('crash', f'server exited while {method} (id {rid}) was pending')
            self.check_response(item, resp)
        self.pending = keep
        self.stderr_check()
        self.check_notes()

    def check_response(self, item, resp):
        rid, method, params, doc, version, text, was_open, _, history = item
        if 'error' in resp:
            code = resp['error'].get('code')
            if code in OK_ERRORS:
                return
            if method in ('textDocument/semanticTokens/full/delta', 'textDocument/semanticTokens/range',
                          'textDocument/documentLink', 'textDocument/codeLens') and code == -32601:
                return
            if code == -32602 and (not was_open or 'rename' in method):
                return
            if not was_open:
                return
            self.ctx.setdefault('error_responses', []).append((method, code, resp['error'].get('message')))
            return
        if 'result' not in resp:
            raise Finding('malformed', f'{method}: response without result or error: {resp!r}')
        if not was_open:
            return
        # validate against the text the request was made on; a pull
        # diagnostic report is computed later, on the version its
        # resultId names
        if method == 'textDocument/diagnostic':
            m = re.fullmatch(r'v(\d+)', str((resp['result'] or {}).get('resultId', '')))
            if not m or int(m.group(1)) not in history:
                return
            text = history[int(m.group(1))]
            params = dict(params, _resultId=m.group(0), _sent_version=version)
        snap = Doc(doc.uri, doc.path, text, doc.enc)
        try:
            check_result(snap, method, params, resp['result'], self.ctx)
        except Finding as f:
            f.detail = f'{f.detail} [request {rid} {method} {json.dumps(params, ensure_ascii=False)[:300]}]'
            if self.ignored(f):
                return
            self.ctx['fail_text'] = (doc.path, text)
            raise

    def ignored(self, f):
        if self.args.ignore and re.search(self.args.ignore, f'{f.kind}: {f.detail}'):
            self.stats['ignored'] = self.stats.get('ignored', 0) + 1
            return True
        return False

    def check_notes(self):
        with self.srv.cv:
            notes = self.srv.notes
            self.srv.notes = []
        for method, params in notes:
            if method == '<protocol>':
                raise Finding('protocol', params)
            if method == 'window/showMessage' or method == 'window/logMessage':
                msg = (params or {}).get('message', '')
                if 'crash' in msg or 'quarantined' in msg:
                    raise Finding('analysis-crash', msg)
            if method == 'textDocument/publishDiagnostics':
                uri = params.get('uri')
                doc = next((d for d in self.docs if d.uri == uri), None)
                ver = params.get('version')
                if ver is None:
                    self.ctx.setdefault('cleared', {})[uri] = True
                    continue
                self.ctx.setdefault('diags', {})[uri] = params.get('diagnostics') or []
                self.ctx.setdefault('diag_ver', {})[uri] = ver
                if doc and doc.open and ver == doc.version:
                    try:
                        check_diagnostics(doc, params.get('diagnostics') or [], 'publishDiagnostics')
                    except Finding as f:
                        if not self.ignored(f):
                            self.ctx['fail_text'] = (doc.path, doc.text)
                            # which earlier versions would the ranges fit?
                            fits = []
                            for v, t in sorted(doc.history.items()):
                                try:
                                    check_diagnostics(Doc(doc.uri, doc.path, t, doc.enc),
                                                      params.get('diagnostics') or [], 'x')
                                    fits.append(v)
                                except Finding:
                                    pass
                            f.detail += f' [published version {ver}; ranges fit versions {fits[-8:]}]'
                            raise

    def settle(self, doc, timeout=None):
        """Wait until diagnostics for doc's current version arrive."""
        timeout = timeout or self.args.deadline
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.check_notes()
            if self.ctx.get('diag_ver', {}).get(doc.uri) == doc.version:
                return self.ctx['diags'][doc.uri]
            if not self.srv.alive():
                raise Finding('crash', 'server exited')
            with self.srv.cv:
                self.srv.cv.wait(0.05)
        raise Finding('hang', f'no diagnostics for version {doc.version} of {doc.uri} in {timeout:.0f}s')

    def stderr_check(self):
        err = ''.join(self.srv.stderr_buf)
        m = SAN_RE.search(err)
        if m:
            i = max(0, m.start() - 200)
            raise Finding('sanitizer', err[i:i + 6000])

    def run(self):
        r = self.rng
        self.start()
        for _ in range(r.randint(1, 3)):
            self.open_doc()
        steps = self.args.steps
        for step in range(steps):
            if not self.srv.alive():
                raise Finding('crash', f'server exited at step {step} (code {self.srv.proc.poll()})')
            opened = [d for d in self.docs if d.open]
            k = r.random()
            if not opened or k < 0.02:
                d = r.choice(self.docs)
                if not d.open:
                    # A publish from before the close may carry the version
                    # the reopened document starts with: first wait for the
                    # cleared (unversioned) publish the server sends after
                    # stopping the closed document's worker.
                    end = time.monotonic() + 10
                    while not self.ctx.get('cleared', {}).get(d.uri) and time.monotonic() < end:
                        self.check_notes()
                        with self.srv.cv:
                            self.srv.cv.wait(0.02)
                    self.ctx.get('diag_ver', {}).pop(d.uri, None)
                    d.open = True
                    d.version = r.choice([1, d.version + 1])
                    d.history = {d.version: d.text}
                    self.srv.notify('textDocument/didOpen', {'textDocument': {
                        'uri': d.uri, 'languageId': 'iron', 'version': d.version, 'text': d.text}})
                elif len(self.docs) < 6:
                    self.open_doc()
                continue
            doc = r.choice(opened)
            if k < 0.04:
                doc.open = False
                self.ctx.setdefault('cleared', {})[doc.uri] = False
                self.srv.notify('textDocument/didClose', {'textDocument': {'uri': doc.uri}})
                if r.random() < 0.3:
                    self.send_request(doc)  # request on a closed document
                continue
            if k < 0.05:
                self.srv.notify('textDocument/didSave', {'textDocument': {'uri': doc.uri}})
                continue
            if k < 0.45:
                self.random_edit(doc)
            else:
                for _ in range(r.randint(1, 4)):
                    self.send_request(doc)
            if r.random() < 0.3:
                self.drain()
            if self.args.parity and r.random() < self.args.parity_rate:
                self.drain(all_=True)
                self.parity(doc)
        self.drain(all_=True)
        if self.args.parity:
            for d in self.docs:
                if d.open:
                    self.parity(d)
        self.stderr_check()
        self.finish()

    def parity(self, doc):
        diags = self.settle(doc)
        # comptime file access and imports differ by design; `-- @file:` is
        # the test harness's file marker (it moves the diagnostics after it
        # to another file, outside the comparison)
        if 'comptime' in doc.text or 'import' in doc.text or '@file:' in doc.text:
            self.stats['parity_skipped'] += 1
            return
        why = parity_check(doc, diags, self.args.ironc, self.env)
        self.stats['parity'] += 1
        if why and not self.ignored(Finding('parity', why)):
            raise Finding('parity', why)

    def finish(self):
        rid = self.srv.request('shutdown', None)
        res = self.srv.wait(rid, self.args.deadline)
        if res is None or 'error' in res:
            raise Finding('shutdown', f'shutdown answered {res!r}')
        self.srv.notify('exit', None)
        try:
            code = self.srv.proc.wait(timeout=self.args.deadline)
        except subprocess.TimeoutExpired:
            raise Finding('hang', 'server did not exit after exit')
        self.srv.err_reader.join(5)
        self.stderr_check()
        if code != 0:
            raise Finding('exit', f'server exit code {code} after shutdown/exit')

    def kill(self):
        try:
            if self.srv.proc.poll() is None:
                self.srv.proc.kill()
                self.srv.proc.wait(5)
        except Exception:
            pass


def run_session(args, seed, files):
    s = Session(args, seed, files)
    t0 = time.monotonic()
    result = {'seed': seed, 'enc': s.enc, 'ok': True}
    try:
        s.run()
    except Finding as f:
        result.update(ok=False, kind=f.kind, detail=f.detail)
    except Exception:
        result.update(ok=False, kind='harness', detail=traceback.format_exc())
    finally:
        s.kill()
    result['stats'] = s.stats
    result['secs'] = round(time.monotonic() - t0, 1)
    result['error_responses'] = s.ctx.get('error_responses', [])[:20]
    if not result['ok']:
        if s.srv.proc.returncode not in (None, 0) and result['kind'] in ('hang', 'crash'):
            pass
        time.sleep(0.2)
        d = os.path.join(args.out, 'fail', str(seed))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'session.jsonl'), 'w', encoding='utf-8', errors='surrogatepass') as f:
            for m in s.log:
                f.write(json.dumps(m, ensure_ascii=False) + '\n')
        with open(os.path.join(d, 'stderr.txt'), 'w') as f:
            f.write(''.join(s.srv.stderr_buf))
        with open(os.path.join(d, 'summary.json'), 'w') as f:
            json.dump(result, f, indent=1, ensure_ascii=False)
        for doc in s.docs:
            with open(os.path.join(d, os.path.basename(doc.path)), 'w', encoding='utf-8',
                      newline='', errors='surrogatepass') as f:
                f.write(doc.text)
        # the texts the requests still pending were made on: a crash or a
        # hang happened on one of them
        for item in s.pending[:20]:
            rid, method, _params, pdoc, _v, ptext = item[:6]
            with open(os.path.join(d, f'pending_{rid}_{method.split("/")[-1]}_'
                                   f'{os.path.basename(pdoc.path)}'), 'w', encoding='utf-8',
                      newline='', errors='surrogatepass') as f:
                f.write(ptext)
        if 'fail_text' in s.ctx:
            path, text = s.ctx['fail_text']
            with open(os.path.join(d, 'at_failure_' + os.path.basename(path)), 'w', encoding='utf-8',
                      newline='', errors='surrogatepass') as f:
                f.write(text)
    shutil.rmtree(s.tmp, ignore_errors=True)
    return result


def peak_rss_mb(pid):
    try:
        with open(f'/proc/{pid}/status') as f:
            for line in f:
                if line.startswith('VmHWM:'):
                    return int(line.split()[1]) // 1024
    except OSError:
        pass
    return None


def replay(args):
    """Resend a recorded session and report what the server did: requests
    left unanswered, crash notices, sanitizer reports, peak RSS."""
    msgs = [json.loads(l) for l in open(args.replay, encoding='utf-8')]
    env = dict(os.environ)
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=0:symbolize=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    srv = Server([args.ironls], env, [])
    ids = []
    for m in msgs:
        if not srv.alive():
            break
        if m.get('method') in ('shutdown', 'exit'):
            continue
        srv._write(m, log=False)
        if 'id' in m and 'method' in m:
            ids.append((m['id'], m['method']))
        if m.get('method') == 'initialize':
            srv.wait(m['id'], args.deadline)
            ids.pop()
        if args.replay_delay:
            time.sleep(args.replay_delay)
    missing = []
    for rid, method in ids:
        try:
            if srv.wait(rid, args.deadline) is None:
                missing.append((rid, method, 'server exited'))
        except Finding:
            missing.append((rid, method, 'no response'))
    rss = peak_rss_mb(srv.proc.pid)
    code = None
    if srv.alive():
        rid = srv.request('shutdown', None)
        try:
            srv.wait(rid, args.deadline)
        except Finding:
            pass
        srv.notify('exit', None)
    try:
        code = srv.proc.wait(timeout=args.deadline)
    except subprocess.TimeoutExpired:
        srv.proc.kill()
        code = 'hang'
    srv.err_reader.join(5)
    err = ''.join(srv.stderr_buf)
    with srv.cv:
        notes = list(srv.notes)
    bad = [n for n in notes if n[0] in ('window/showMessage',) and
           ('crash' in str(n[1]) or 'quarantined' in str(n[1]))]
    print(f'exit: {code}; requests: {len(ids)}; unanswered: {missing[:10]}; '
          f'crash notices: {len(bad)}; peak RSS: {rss} MB')
    m = SAN_RE.search(err)
    if m:
        print(err[max(0, m.start() - 200):m.start() + 4000])
        return 1
    return 1 if bad or missing or code not in (0,) else 0


def show(args):
    """Print the diagnostics ironls publishes for a file next to what
    `ironc check` reports (for triaging a parity finding)."""
    args.encoding = args.encoding if args.encoding != 'both' else 'utf-16'
    s = Session(args, 0, [])
    try:
        s.start()
        text = read_text(args.show)
        doc = s.open_doc(os.path.basename(args.show), text)
        diags = s.settle(doc)
        print('server:')
        for d in sorted(diags, key=lambda d: (d['range']['start']['line'], d['range']['start']['character'])):
            r = d['range']['start']
            e = d['range']['end']
            print(f"  {d.get('code')} {r['line'] + 1}:{r['character'] + 1}-{e['line'] + 1}:{e['character'] + 1} {d['message']}")
        if args.ironc:
            rc, cd = ironc_check(args.ironc, doc.path, s.env)
            print(f'ironc check (exit {rc}):')
            for d in cd:
                print(f'  {d[1]} {d[4]}:{d[5]} {d[2]}')
            print('parity:', parity_check(doc, diags, args.ironc, s.env) or 'ok')
    finally:
        s.kill()
        shutil.rmtree(s.tmp, ignore_errors=True)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ironls', required=True)
    ap.add_argument('--ironc')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--sessions', type=int, default=10)
    ap.add_argument('--steps', type=int, default=200)
    ap.add_argument('-j', type=int, default=1)
    ap.add_argument('--out', default='lsp_fuzz_out')
    ap.add_argument('--tmp', default=None)
    ap.add_argument('--encoding', default='both', choices=['utf-16', 'utf-8', 'both'])
    ap.add_argument('--corpus', nargs='*', default=None)
    ap.add_argument('--gen', nargs='*', default=None,
                    help='prog_fuzz areas to draw generated programs from')
    ap.add_argument('--no-parity', dest='parity', action='store_false')
    ap.add_argument('--parity-rate', type=float, default=0.02)
    ap.add_argument('--deadline', type=float, default=30.0, help='seconds per request')
    ap.add_argument('--time-budget', type=float, default=0, help='stop starting sessions after this many seconds')
    ap.add_argument('--replay')
    ap.add_argument('--show', help='print the server and ironc diagnostics of one file')
    ap.add_argument('--replay-delay', type=float, default=0.0)
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--ignore', help='regex: findings ("kind: detail") it matches are counted, not failed')
    args = ap.parse_args(argv)
    args.ironls = os.path.abspath(args.ironls)
    if args.replay:
        return replay(args)
    if args.show:
        return show(args)
    if args.parity and not args.ironc:
        ap.error('--ironc is needed for the parity check (or pass --no-parity)')
    if args.ironc:
        args.ironc = os.path.abspath(args.ironc)
    if args.corpus is None:
        args.corpus = [os.path.join(REPO, 'tests', 'integration', 'v4'),
                       os.path.join(REPO, 'tests', 'lsp', 'fixtures'),
                       os.path.join(REPO, 'examples')]
    if args.gen is None:
        args.gen = ['stmt', 'coll', 'obj', 'closure', 'generic', 'num', 'ptr']
    files = corpus_files(args.corpus)
    os.makedirs(args.out, exist_ok=True)
    if args.tmp:
        os.makedirs(args.tmp, exist_ok=True)
    seeds = list(range(args.seed, args.seed + args.sessions))
    t0 = time.monotonic()
    fails = []
    totals = {'requests': 0, 'edits': 0, 'parity': 0, 'parity_skipped': 0}
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as ex:
        futs = {}
        it = iter(seeds)

        def submit():
            if args.time_budget and time.monotonic() - t0 > args.time_budget:
                return False
            s = next(it, None)
            if s is None:
                return False
            futs[ex.submit(run_session, args, s, files)] = s
            return True

        for _ in range(args.j):
            submit()
        while futs:
            fin, _ = concurrent.futures.wait(list(futs), return_when=concurrent.futures.FIRST_COMPLETED)
            for f in fin:
                del futs[f]
                res = f.result()
                done += 1
                for k in totals:
                    totals[k] += res['stats'].get(k, 0)
                if not res['ok']:
                    fails.append(res)
                    print(f"FAIL seed={res['seed']} enc={res['enc']} {res['kind']}: {res['detail'][:600]}",
                          flush=True)
                elif not args.quiet:
                    print(f"ok   seed={res['seed']} enc={res['enc']} {res['stats']} {res['secs']}s"
                          + (f" errors={res['error_responses'][:3]}" if res['error_responses'] else ''),
                          flush=True)
                submit()
    print(f'lsp_fuzz: {done} sessions, {totals["edits"]} edits, {totals["requests"]} requests, '
          f'{totals["parity"]} parity checks ({totals["parity_skipped"]} skipped), '
          f'{len(fails)} failed, {time.monotonic() - t0:.0f}s')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
