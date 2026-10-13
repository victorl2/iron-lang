#!/usr/bin/env python3
"""Formatter fuzzer: `ironc fmt` and the language server's formatting.

Takes programs from the integration corpus and from the program fuzzers'
generators (prog_fuzz.py), perturbs them (whitespace, indentation, blank
lines, comments, CRLF line ends, trailing whitespace, a missing or extra
final newline), formats them and checks:

  * `ironc fmt` neither crashes, hangs nor prints a sanitizer report, and
    never refuses a file with an "internal formatter error";
  * it refuses exactly the files that do not parse (`ironc check` reports
    a syntax error, a code below E0200);
  * idempotence: fmt(fmt(x)) == fmt(x);
  * comments are kept: every `--` comment of x is in fmt(x), in order;
  * fmt(x) reports the same diagnostics as x under `ironc check` (codes
    and messages, positions aside);
  * a perturbation that only changes horizontal whitespace, trailing
    whitespace, line ends or the final newline formats to fmt(x);
  * with --run, a program that builds runs with the same output before
    and after formatting (generated programs also against their expected
    output);
  * with --ironls, textDocument/formatting gives the text `ironc fmt`
    writes, and textDocument/rangeFormatting over the whole document the
    same.

Usage:
    fmt_fuzz.py --ironc C [--ironls L] [--seed S] [--count N] [-j N]
                [--out DIR] [--corpus DIR ...] [--gen AREA ...] [--run]

Failing inputs are written to OUT/fail/<seed>/. Exit status 1 when any
case failed.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import glob
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)

SAN_RE = re.compile(r'ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|'
                    r'SUMMARY: UndefinedBehaviorSanitizer|internal compiler error')
DIAG_HEAD = re.compile(r'^(error|warning|note)\[([EW]\d{4})\]: (.*)$')


class Finding(Exception):
    def __init__(self, kind, detail):
        super().__init__(f'{kind}: {detail}')
        self.kind = kind
        self.detail = detail


def run(cmd, timeout=120, env=None, cwd=None):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=env, cwd=cwd)
    except subprocess.TimeoutExpired:
        raise Finding('hang', f'{" ".join(cmd)} timed out after {timeout}s')
    err = p.stderr.decode('utf-8', 'replace')
    if SAN_RE.search(err) or p.returncode < 0:
        raise Finding('crash', f'{" ".join(cmd)} exit {p.returncode}: {err[-3000:]}')
    return p.returncode, p.stdout.decode('utf-8', 'replace'), err


def write(path, text):
    with open(path, 'w', encoding='utf-8', newline='', errors='surrogatepass') as f:
        f.write(text)


def read(path):
    with open(path, encoding='utf-8', newline='', errors='surrogatepass') as f:
        return f.read()


def fmt(ironc, text, work, env):
    """(formatted text or None when refused, stderr)."""
    path = os.path.join(work, 'fmt_in.iron')
    write(path, text)
    rc, _, err = run([ironc, 'fmt', path], env=env)
    if 'internal formatter error' in err:
        raise Finding('formatter-refused', err[-2000:])
    if rc != 0:
        return None, err
    return read(path), err


def check(ironc, text, work, env):
    path = os.path.join(work, 'chk.iron')
    write(path, text)
    rc, _, err = run([ironc, 'check', path], env=env)
    diags = []
    for line in err.splitlines():
        m = DIAG_HEAD.match(line)
        if m:
            diags.append((m.group(2), m.group(3), m.group(1)))
    return rc, diags


def is_syntax(d):
    """A lexer or parser error: E0001..E0199 (E0175/E0176 also come from
    name resolution)."""
    code, _msg, level = d
    return level == 'error' and code.startswith('E') and int(code[1:]) < 200 and \
        int(code[1:]) not in (175, 176)


def comments(text):
    """The `--` comments of a source, found the way the formatter's scanner
    does (outside string literals; `{...}` in a string is code)."""
    out = []
    i, n = 0, len(text)
    stack = ['code']
    while i < n:
        c = text[i]
        top = stack[-1]
        if top == 'code' or top == 'interp':
            if c == '-' and text.startswith('--', i) and top == 'code':
                j = text.find('\n', i)
                j = n if j < 0 else j
                out.append(text[i:j].rstrip(' \t\r'))
                i = j
                continue
            if c == '"':
                if text.startswith('"""', i):
                    stack.append('tstr')
                    i += 3
                else:
                    stack.append('str')
                    i += 1
                continue
            if top == 'interp':
                if c == '{':
                    stack.append('interp')
                elif c == '}':
                    stack.pop()
            if c == '\n' and top == 'interp':
                while len(stack) > 1 and stack[-1] != 'tstr':
                    stack.pop()
            i += 1
            continue
        # in a string
        if c == '\\':
            i += 2
            continue
        if c == '{':
            stack.append('interp')
            i += 1
            continue
        if c == '"':
            if top == 'str':
                stack.pop()
                i += 1
                continue
            if text.startswith('"""', i):
                stack.pop()
                i += 3
                continue
        if c == '\n' and top == 'str':
            stack.pop()
        i += 1
    return out


# ------------------------------------------------------------- perturbations

def perturb(text, rng):
    """Returns (new_text, kind) where kind is 'ws' (only horizontal
    whitespace, trailing blanks, line ends or final newline changed) or
    'other' (blank lines or comments added)."""
    lines = text.split('\n')
    # lines inside a triple-quoted string keep their bytes
    inside = set()
    in_t = False
    for k, ln in enumerate(lines):
        if in_t:
            inside.add(k)
        if ln.count('"""') % 2 == 1:
            in_t = not in_t
    kind = 'ws'
    ops = rng.sample(['indent', 'trail', 'spaces', 'crlf', 'final', 'blank', 'comment',
                      'tabs', 'ownline'], rng.randint(1, 4))
    out = list(lines)
    for op in ops:
        if op == 'indent':
            for k in range(len(out)):
                if k in inside or (k - 1) in inside or not out[k].strip():
                    continue
                if rng.random() < 0.3:
                    out[k] = rng.choice(['', ' ', '  ', '\t', '      ', '\t  ']) + out[k].lstrip(' \t')
        elif op == 'tabs':
            for k in range(len(out)):
                if k in inside or (k - 1) in inside:
                    continue
                s = out[k]
                lead = len(s) - len(s.lstrip(' '))
                if lead and rng.random() < 0.5:
                    out[k] = '\t' * (lead // 4) + ' ' * (lead % 4) + s[lead:]
        elif op == 'trail':
            for k in range(len(out)):
                if k in inside or (k + 1) in inside:
                    continue
                if rng.random() < 0.3 and '"' not in out[k][-1:]:
                    out[k] = out[k] + rng.choice([' ', '\t', '   ', ' \t '])
        elif op == 'spaces':
            # widen runs of spaces between tokens outside strings (the
            # formatter leaves spaces inside a line alone: not canonical)
            kind = 'other'
            for k in range(len(out)):
                if k in inside or '"' in out[k] or '--' in out[k]:
                    continue
                s = out[k]
                lead = len(s) - len(s.lstrip(' \t'))
                body = s[lead:]
                if rng.random() < 0.3:
                    body = re.sub(r' +', lambda m: m.group(0) * rng.randint(1, 3), body)
                out[k] = s[:lead] + body
        elif op == 'crlf':
            pass
        elif op == 'final':
            pass
        elif op == 'blank':
            kind = 'other'
            for _ in range(rng.randint(1, 5)):
                k = rng.randrange(len(out) + 1)
                if k in inside or (k - 1) in inside or k in inside:
                    continue
                out.insert(k, rng.choice(['', '', '   ', '\t']))
                inside = {i + 1 if i >= k else i for i in inside}
        elif op == 'comment':
            kind = 'other'
            for k in range(len(out)):
                if k in inside or (k + 1) in inside or '"' in out[k] or '--' in out[k]:
                    continue
                if out[k].strip() and rng.random() < 0.2:
                    out[k] = out[k] + rng.choice([' -- c', '  -- note 😀', '\t-- x{y}"'])
        elif op == 'ownline':
            kind = 'other'
            for _ in range(rng.randint(1, 4)):
                k = rng.randrange(len(out) + 1)
                if k in inside or (k - 1) in inside:
                    continue
                out.insert(k, rng.choice(['-- own line', '    -- indented', '\t--', '-- "q" {b}']))
                inside = {i + 1 if i >= k else i for i in inside}
    if '\"\"\"' in text:
        kind = 'other'   # a multi-line string's lines are its value
    t = '\n'.join(out)
    if 'crlf' in ops:
        t = t.replace('\n', '\r\n')
    if 'final' in ops:
        t = t.rstrip('\r\n') + rng.choice(['', '\n', '\n\n\n', '\r\n'])
    return t, kind


# ------------------------------------------------------------- lsp

class Lsp:
    def __init__(self, ironls, env):
        self.p = subprocess.Popen([ironls], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=env)
        self.id = 0
        self.lock = threading.Lock()
        self.err = []
        threading.Thread(target=lambda: [self.err.append(l.decode('utf-8', 'replace'))
                                         for l in iter(self.p.stderr.readline, b'')],
                         daemon=True).start()
        r = self.req('initialize', {'processId': os.getpid(), 'rootUri': None,
                                    'capabilities': {'general': {'positionEncodings': ['utf-16']}}})
        self.note('initialized', {})
        self.n = 0

    def send(self, obj):
        data = json.dumps(obj, ensure_ascii=False).encode('utf-8', 'surrogatepass')
        self.p.stdin.write(b'Content-Length: %d\r\n\r\n' % len(data) + data)
        self.p.stdin.flush()

    def recv(self):
        f = self.p.stdout
        length = None
        while True:
            line = f.readline()
            if not line:
                raise Finding('lsp-crash', 'ironls exited: ' + ''.join(self.err)[-3000:])
            line = line.strip()
            if not line:
                break
            if line.lower().startswith(b'content-length:'):
                length = int(line.split(b':')[1])
        return json.loads(f.read(length))

    def req(self, method, params):
        self.id += 1
        rid = self.id
        self.send({'jsonrpc': '2.0', 'id': rid, 'method': method, 'params': params})
        while True:
            m = self.recv()
            if m.get('id') == rid and 'method' not in m:
                return m
            if 'method' in m and 'id' in m:
                self.send({'jsonrpc': '2.0', 'id': m['id'], 'result': None})

    def note(self, method, params):
        self.send({'jsonrpc': '2.0', 'method': method, 'params': params})

    def format(self, text, ranged=False):
        self.n += 1
        uri = f'file:///fmtfuzz/doc{self.n}.iron'
        self.note('textDocument/didOpen', {'textDocument': {'uri': uri, 'languageId': 'iron',
                                                            'version': 1, 'text': text}})
        opts = {'tabSize': 4, 'insertSpaces': True}
        if ranged:
            nl = text.count('\n') + 1
            r = self.req('textDocument/rangeFormatting', {
                'textDocument': {'uri': uri}, 'options': opts,
                'range': {'start': {'line': 0, 'character': 0},
                          'end': {'line': nl, 'character': 0}}})
        else:
            r = self.req('textDocument/formatting', {'textDocument': {'uri': uri}, 'options': opts})
        self.note('textDocument/didClose', {'textDocument': {'uri': uri}})
        if 'error' in r:
            return None
        edits = r.get('result')
        if edits is None:
            return None
        return apply_edits_utf16(text, edits)

    def close(self):
        try:
            self.req('shutdown', None)
            self.note('exit', None)
            self.p.wait(10)
        except Exception:
            self.p.kill()
        err = ''.join(self.err)
        if SAN_RE.search(err):
            raise Finding('lsp-sanitizer', err[-4000:])


def apply_edits_utf16(text, edits):
    lines = re.split(r'(?<=\n)', text)
    starts = [0]
    for ln in lines:
        starts.append(starts[-1] + len(ln))

    def idx(p):
        line, ch = p['line'], p['character']
        if line >= len(lines):
            return len(text)
        s = lines[line]
        u = 0
        for k, c in enumerate(s):
            if u >= ch or c in '\r\n':
                return starts[line] + k
            u += 2 if ord(c) > 0xFFFF else 1
        return starts[line] + len(s)

    spans = sorted(((idx(e['range']['start']), idx(e['range']['end']), e['newText']) for e in edits),
                   key=lambda t: t[:2])
    out = text
    for a, b, t in reversed(spans):
        out = out[:a] + t + out[b:]
    return out


# ------------------------------------------------------------- one case

def build_and_run(ironc, text, work, env, tag):
    src = os.path.join(work, f'run_{tag}.iron')
    write(src, text)
    exe = os.path.join(work, f'run_{tag}')
    rc, _, err = run([ironc, 'build', src, '-o', exe], timeout=300, env=env)
    if rc != 0:
        return None
    # the program's own outcome, panics and hangs included: compared, not judged
    try:
        p = subprocess.run([exe], capture_output=True, timeout=20, env=env)
    except subprocess.TimeoutExpired:
        return ('timeout',)
    return p.returncode, p.stdout.decode('utf-8', 'replace')


def one_case(args, seed, files, lsp):
    rng = random.Random(seed)
    work = tempfile.mkdtemp(prefix=f'fmtfz{seed}_', dir=args.tmp)
    env = dict(os.environ)
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=0')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    expected = None
    if args.gen and (not files or rng.random() < 0.4):
        import prog_fuzz
        area = rng.choice(args.gen)
        src, exp = prog_fuzz.make_program(area, rng.randrange(1 << 30))
        if not getattr(exp, 'errors', None) and not getattr(exp, 'panic', None):
            expected = '\n'.join(exp) + ('\n' if len(exp) else '')
        name = f'gen_{area}'
    else:
        f = rng.choice(files)
        src = read(f)
        name = os.path.relpath(f, REPO)
    info = {'seed': seed, 'source': name, 'ok': True, 'checks': []}
    stage = {'text': src}
    try:
        variants = [('orig', src, 'ws')]
        for _ in range(args.variants):
            t, kind = perturb(src, rng)
            variants.append(('perturbed', t, kind))
        base_fmt = None
        base_check = None
        for label, text, kind in variants:
            stage['text'] = text
            stage['label'] = label
            f1, err = fmt(args.ironc, text, work, env)
            rc, diags = check(args.ironc, text, work, env)
            syntax = any(is_syntax(d) for d in diags)
            if f1 is None:
                if rc == 0:
                    raise Finding('refused-valid', f'{label}: ironc fmt refused a file ironc check '
                                  f'accepts: {err[-800:]}')
                info['checks'].append('refused')
                continue
            if syntax:
                raise Finding('formatted-invalid', f'{label}: ironc fmt formatted a file ironc check '
                              f'rejects with {diags[:5]}')
            f2, err2 = fmt(args.ironc, f1, work, env)
            if f2 != f1:
                stage['formatted'] = f1
                raise Finding('not-idempotent', f'{label}: fmt(fmt(x)) != fmt(x)'
                              + (' (second run refused)' if f2 is None else ''))
            ca, cb = comments(text), comments(f1)
            if ca != cb:
                stage['formatted'] = f1
                missing = [c for c in ca if c not in cb][:5]
                raise Finding('comment-lost', f'{label}: comments differ; missing {missing}')
            rc1, d1 = check(args.ironc, f1, work, env)
            if rc1 != rc or sorted(d1) != sorted(diags):
                stage['formatted'] = f1
                raise Finding('diagnostics-changed', f'{label}: check before {rc} {sorted(diags)[:6]} '
                              f'after {rc1} {sorted(d1)[:6]}')
            if label == 'orig':
                base_fmt, base_check = f1, (rc, sorted(diags))
            elif kind == 'ws' and base_fmt is not None and f1 != base_fmt:
                stage['formatted'] = f1
                stage['base_formatted'] = base_fmt
                raise Finding('whitespace-sensitive', 'a whitespace-only perturbation formats differently')
            if lsp is not None:
                l1 = lsp.format(text)
                if l1 is not None and l1 != f1:
                    stage['formatted'] = f1
                    stage['lsp_formatted'] = l1
                    raise Finding('lsp-fmt-parity', f'{label}: textDocument/formatting differs from ironc fmt')
                l2 = lsp.format(text, ranged=True)
                crlf_doc = '\r\n' in text and text.count('\n') == text.count('\r\n')
                if l2 is not None and crlf_doc and l2.count('\n') != l2.count('\r\n'):
                    stage['formatted'] = f1
                    stage['lsp_formatted'] = l2
                    raise Finding('lsp-rangefmt-line-ends', f'{label}: rangeFormatting of a CRLF '
                                  f'document mixes line ends')
                if l2 is not None:
                    l2 = l2.replace('\r\n', '\n')
                # range mode keeps a leading blank line and the blank lines
                # that end the range (a whole-file format drops them)
                def blanks(t):
                    return re.sub(r'\n{3,}', '\n\n', t).strip('\n')
                if l2 is not None and blanks(l2) != blanks(f1.replace('\r\n', '\n')):
                    stage['formatted'] = f1
                    stage['lsp_formatted'] = l2
                    raise Finding('lsp-rangefmt-parity', f'{label}: rangeFormatting over the whole '
                                  f'document differs from ironc fmt')
            info['checks'].append(label)
            # windows, sockets and clocks make the output vary run to run
            runnable = not re.search(r'^\s*import\s+(raylib|net|http|websocket|time|os|log)\b',
                                     text, re.M)
            if args.run and runnable and rc == 0 and (label == 'orig' or rng.random() < 0.3):
                before = build_and_run(args.ironc, text, work, env, 'a')
                after = build_and_run(args.ironc, f1, work, env, 'b')
                if (before is None) != (after is None):
                    stage['formatted'] = f1
                    raise Finding('build-changed', f'{label}: builds before={before is not None} '
                                  f'after={after is not None}')
                if before is not None:
                    if before != after:
                        stage['formatted'] = f1
                        raise Finding('behaviour-changed', f'{label}: output before {before!r:.300} '
                                      f'after {after!r:.300}')
                    if expected is not None and label == 'orig' and before[1] != expected:
                        info['note'] = 'generated program output differs from its model (prog_fuzz territory)'
                    info['checks'].append('run')
    except Finding as f:
        info.update(ok=False, kind=f.kind, detail=f.detail)
        d = os.path.join(args.out, 'fail', str(seed))
        os.makedirs(d, exist_ok=True)
        write(os.path.join(d, 'input.iron'), stage['text'])
        for k in ('formatted', 'base_formatted', 'lsp_formatted'):
            if k in stage:
                write(os.path.join(d, k + '.iron'), stage[k])
        with open(os.path.join(d, 'summary.json'), 'w') as fh:
            json.dump(info, fh, indent=1, ensure_ascii=False)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return info


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ironc', required=True)
    ap.add_argument('--ironls')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--count', type=int, default=50)
    ap.add_argument('--variants', type=int, default=3, help='perturbed variants per program')
    ap.add_argument('-j', type=int, default=1)
    ap.add_argument('--out', default='fmt_fuzz_out')
    ap.add_argument('--tmp')
    ap.add_argument('--corpus', nargs='*')
    ap.add_argument('--gen', nargs='*')
    ap.add_argument('--run', action='store_true', help='build and run before and after formatting')
    ap.add_argument('--time-budget', type=float, default=0)
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args(argv)
    args.ironc = os.path.abspath(args.ironc)
    if args.corpus is None:
        args.corpus = [os.path.join(REPO, 'tests', 'integration', 'v4'),
                       os.path.join(REPO, 'tests', 'lsp', 'fixtures'),
                       os.path.join(REPO, 'examples'),
                       os.path.join(REPO, 'src', 'stdlib')]
    if args.gen is None:
        args.gen = ['stmt', 'coll', 'obj', 'closure', 'generic', 'num', 'ptr']
    files = []
    for d in args.corpus:
        files += glob.glob(os.path.join(d, '**', '*.iron'), recursive=True)
    files = sorted(f for f in set(files) if os.path.getsize(f) < 100000)
    os.makedirs(args.out, exist_ok=True)
    if args.tmp:
        os.makedirs(args.tmp, exist_ok=True)
    t0 = time.monotonic()
    fails = []
    done = 0
    nchecks = 0
    local = threading.local()
    lsps = []

    def worker(seed):
        lsp = None
        if args.ironls:
            lsp = getattr(local, 'lsp', None)
            if lsp is None:
                env = dict(os.environ)
                env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=0')
                lsp = local.lsp = Lsp(os.path.abspath(args.ironls), env)
                lsps.append(lsp)
        try:
            return one_case(args, seed, files, lsp)
        except Finding as f:
            local.lsp = None
            return {'seed': seed, 'ok': False, 'kind': f.kind, 'detail': f.detail, 'checks': []}

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as ex:
        seeds = iter(range(args.seed, args.seed + args.count))
        futs = set()

        def submit():
            if args.time_budget and time.monotonic() - t0 > args.time_budget:
                return
            s = next(seeds, None)
            if s is not None:
                futs.add(ex.submit(worker, s))

        for _ in range(args.j * 2):
            submit()
        while futs:
            fin, _ = concurrent.futures.wait(futs, return_when=concurrent.futures.FIRST_COMPLETED)
            for f in fin:
                futs.discard(f)
                r = f.result()
                done += 1
                nchecks += len(r['checks'])
                if not r['ok']:
                    fails.append(r)
                    print(f"FAIL seed={r['seed']} {r.get('source', '')} {r['kind']}: {r['detail'][:500]}",
                          flush=True)
                elif not args.quiet:
                    print(f"ok   seed={r['seed']} {r['source']} {r['checks']}", flush=True)
                submit()
    for lsp in lsps:
        try:
            lsp.close()
        except Finding as f:
            fails.append({'kind': f.kind, 'detail': f.detail})
            print(f'FAIL {f.kind}: {f.detail[:1000]}')
    print(f'fmt_fuzz: {done} programs, {nchecks} formatted variants checked, {len(fails)} failed, '
          f'{time.monotonic() - t0:.0f}s')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
