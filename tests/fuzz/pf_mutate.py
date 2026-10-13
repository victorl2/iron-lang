"""Near-valid mutants of generated programs for cg_fuzz.py (`mutate`).

A program from any prog_fuzz.py generator is mutated a few times and each
mutant goes through ironc. ironc must either reject it with a diagnostic
code (error[Ennnn]) or build it, and a built program must not die of a
signal a safe program cannot raise. A failure is:

    crash      ironc killed by a signal, an internal compiler error, an
               assertion, or an AddressSanitizer / UBSan report (run an
               ironc built with IRON_ENABLE_SANITIZERS)
    hang       ironc still running after PF_MUTATE_TIMEOUT seconds (60)
    cc         the C compiler rejects or warns on the C ironc wrote
    nocode     ironc fails without reporting any error[Ennnn]
    signal     the built program dies of SIGSEGV, SIGBUS, SIGILL or SIGFPE

Mutations (one to three per mutant):
    token level   delete, duplicate or swap tokens; replace an identifier
                  with another one of the program, a type with another
                  type, a keyword with another keyword, a literal with a
                  literal of another type, an operator with another
    statement     delete, duplicate, swap or move a whole statement (lines
                  with balanced braces); duplicate a top-level declaration
    calls         drop or add an argument (wrong arity)
    names         shadow a binding (`val x = ...` again in the block),
                  misplace a keyword (return, break, continue, defer,
                  spawn, await, else, match, free, leak) at a statement
"""
import os
import random
import re
import shutil
import signal
import subprocess

import cg_fuzz

MUTANTS = 8

TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|[A-Za-z_][A-Za-z0-9_]*|\d+\.\d+|\d+|->|==|!=|<=|>=|<<|>>|'
                   r'\+=|-=|\*=|/=|\S')
TYPES = ['Int', 'Int8', 'UInt8', 'Int32', 'UInt64', 'Float', 'Float32', 'Bool', 'String', '[Int]',
         '[String]', '?Int', 'Map[String, Int]', 'Set[Int]', '(Int, String)', 'func(Int) -> Int',
         '[Int; 3]', '[Int; <=4]', 'rc Int', '*Int', '?String', '[[Int]]', 'Void']
KEYWORDS = ['val', 'var', 'func', 'if', 'elif', 'else', 'while', 'for', 'in', 'return', 'break',
            'continue', 'match', 'object', 'enum', 'interface', 'impl', 'defer', 'free', 'leak',
            'heap', 'rc', 'weak', 'spawn', 'await', 'parallel', 'comptime', 'readonly', 'pure',
            'self', 'null', 'true', 'false', 'not', 'and', 'or', 'is', 'drop', 'copy', 'nocopy',
            'init', 'pub', 'unchecked', 'range', 'super', 'extern', 'patch']
LITERALS = ['0', '1', '-1', '255', '9223372036854775807', '-9223372036854775808', '1.5', '0.0',
            '"s"', '""', 'true', 'false', 'null', '[]', '[1, 2]', '(1, "a")', '"{1}"', '1e308',
            '18446744073709551615', '"\\u{1F600}"']
OPERATORS = ['+', '-', '*', '/', '%', '&', '|', '^', '<<', '>>', '==', '!=', '<', '>', '<=', '>=',
             'and', 'or', '=', '+=', '->', '.', ',', ':', '?', '!', '~', 'is']
MISPLACED = ['return', 'return 0', 'break', 'continue', 'defer println("d")', 'free x',
             'leak x', 'await 1', 'else {', '}', 'match 1 {', 'spawn("s") {', 'val', '} else {',
             'parallel', 'comptime 1', 'drop {', 'init() {', 'func f() {', 'object O {']
SIGNALS = (signal.SIGSEGV, signal.SIGBUS, signal.SIGILL, signal.SIGFPE)
# cg_reduce.py on a crash that aborts (a double free the allocator caught)
if os.environ.get('CG_REDUCE_ABORT'):
    SIGNALS += (signal.SIGABRT,)


def timeout():
    return int(os.environ.get('PF_MUTATE_TIMEOUT', '60'))


def code_spans(src):
    """(start, end) of the source outside string literals' text."""
    import pf_meta
    spans, pos = [], 0
    for is_code, t in pf_meta.split_code(src):
        if is_code:
            spans.append((pos, pos + len(t)))
        pos += len(t)
    return spans


def tokens(src):
    """(start, end, text) of each token outside string literals' text
    (string literals themselves are tokens)."""
    out = []
    for m in TOKEN.finditer(src):
        out.append((m.start(), m.end(), m.group(0)))
    return out


def replace(src, a, b, text):
    return src[:a] + text + src[b:]


def stmt_lines(lines):
    """Indices of lines that are a whole statement: braces balanced, inside
    a function (indented), not a closing brace."""
    out = []
    for i, l in enumerate(lines):
        s = l.strip()
        if not s or not l.startswith('    ') or s.startswith('}'):
            continue
        if l.count('{') == l.count('}') and l.count('(') == l.count(')'):
            out.append(i)
    return out


def block_end(lines, i):
    """i opens a block (ends with `{`): the index of its closing line."""
    depth = 0
    for j in range(i, len(lines)):
        depth += lines[j].count('{') - lines[j].count('}')
        if depth <= 0:
            return j
    return len(lines) - 1


def m_token(r, src):
    toks = tokens(src)
    if not toks:
        return src
    a, b, t = r.choice(toks)
    k = r.randrange(9)
    idents = [x for _, _, x in toks if re.match(r'[a-z_]\w*$', x)]
    if k == 0:
        return replace(src, a, b, '')
    if k == 1:
        return replace(src, a, b, t + ' ' + t)
    if k == 2:
        i = toks.index((a, b, t))
        if i + 1 < len(toks):
            a2, b2, t2 = toks[i + 1]
            return src[:a] + t2 + src[b:a2] + t + src[b2:]
        return src
    if k == 3 and idents:
        return replace(src, a, b, r.choice(idents)) if re.match(r'\w', t) else src
    if k == 4:
        tt = [x for x in toks if x[2] in ('Int', 'String', 'Bool', 'Float', 'Int8', 'UInt8',
                                           'Int32', 'UInt64', 'Float32')]
        if tt:
            a, b, _ = r.choice(tt)
            return replace(src, a, b, r.choice(TYPES))
        return src
    if k == 5:
        kw = [x for x in toks if x[2] in KEYWORDS]
        if kw:
            a, b, _ = r.choice(kw)
            return replace(src, a, b, r.choice(KEYWORDS))
        return src
    if k == 6:
        lits = [x for x in toks if re.match(r'\d|"|true$|false$|null$', x[2])]
        if lits:
            a, b, _ = r.choice(lits)
            return replace(src, a, b, r.choice(LITERALS))
        return src
    if k == 7:
        ops = [x for x in toks if x[2] in OPERATORS]
        if ops:
            a, b, _ = r.choice(ops)
            return replace(src, a, b, r.choice(OPERATORS))
        return src
    return replace(src, a, a, r.choice(KEYWORDS + TYPES + LITERALS) + ' ')


def m_stmt(r, src):
    lines = src.split('\n')
    st = stmt_lines(lines)
    k = r.randrange(6)
    if k == 5:   # a whole block statement
        opens = [i for i, l in enumerate(lines) if l.startswith('    ') and l.rstrip().endswith('{')]
        if not opens:
            return src
        i = r.choice(opens)
        j = block_end(lines, i)
        blk = lines[i:j + 1]
        if r.random() < 0.5:
            del lines[i:j + 1]
        else:
            lines[j + 1:j + 1] = blk
        return '\n'.join(lines)
    if not st:
        return src
    i = r.choice(st)
    if k == 0:
        del lines[i]
    elif k == 1:
        lines.insert(i, lines[i])
    elif k == 2:
        j = r.choice(st)
        lines[i], lines[j] = lines[j], lines[i]
    elif k == 3:
        l = lines.pop(i)
        st2 = stmt_lines(lines) or [i]
        lines.insert(r.choice(st2), l)
    else:
        # a top-level declaration twice
        tops = [n for n, l in enumerate(lines) if re.match(r'(func|object|enum|interface|val|var)\b', l)]
        if not tops:
            return src
        t = r.choice(tops)
        e = block_end(lines, t) if lines[t].rstrip().endswith('{') else t
        lines[e + 1:e + 1] = [''] + lines[t:e + 1]
    return '\n'.join(lines)


def m_call(r, src):
    calls = [m for m in re.finditer(r'\b([a-z_]\w*)\(', src)
             if m.group(1) not in ('if', 'while', 'for', 'match', 'func', 'println', 'print')]
    if not calls:
        return src
    m = r.choice(calls)
    i, depth = m.end(), 1
    while i < len(src) and depth:
        depth += {'(': 1, ')': -1}.get(src[i], 0)
        i += 1
    close = i - 1
    inner = src[m.end():close]
    if r.random() < 0.5 and inner.strip():
        # drop the last top-level argument
        depth, cut = 0, -1
        for j, ch in enumerate(inner):
            depth += {'(': 1, '[': 1, '{': 1, ')': -1, ']': -1, '}': -1}.get(ch, 0)
            if ch == ',' and depth == 0:
                cut = j
        new = inner[:cut] if cut >= 0 else ''
    else:
        new = inner + (', ' if inner.strip() else '') + r.choice(LITERALS)
    return src[:m.end()] + new + src[close:]


def m_names(r, src):
    lines = src.split('\n')
    st = stmt_lines(lines)
    if not st:
        return src
    i = r.choice(st)
    ind = re.match(r'\s*', lines[i]).group(0)
    if r.random() < 0.5:
        names = re.findall(r'\b(?:val|var)\s+([a-z_]\w*)', src)
        if not names:
            return src
        lines.insert(i, f'{ind}{r.choice(["val", "var"])} {r.choice(names)} = {r.choice(LITERALS)}')
    else:
        lines.insert(i, ind + r.choice(MISPLACED))
    return '\n'.join(lines)


MUTATIONS = [m_token, m_token, m_token, m_stmt, m_stmt, m_call, m_names]


def mutant(src, r):
    out = src
    for _ in range(r.choice((1, 1, 2, 3))):
        out = r.choice(MUTATIONS)(r, out)
    return out


def classify(b, timed_out, log):
    """None when the ironc run is acceptable, else (kind, message)."""
    if timed_out:
        return 'hang', f'ironc did not finish in {timeout()} s'
    if b.returncode < 0:
        return 'crash', f'ironc killed by signal {-b.returncode}\n{log[-3000:]}'
    for pat in ('internal compiler error', 'internal error', 'Assertion', 'AddressSanitizer',
                'LeakSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer', 'Segmentation',
                'stack-overflow', 'iron_ice'):
        if pat in log:
            return 'crash', log[-4000:]
    if cg_fuzz.CWARN.search(log):
        return 'cc', '\n'.join(l for l in log.splitlines() if cg_fuzz.CWARN.match(l))[:3000]
    if b.returncode != 0 and not cg_fuzz.DIAG.search(log):
        return 'nocode', log[-3000:]
    return None


def run_ironc(ironc, args, cwd):
    env = dict(os.environ)
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=0:exitcode=86')
    env.setdefault('UBSAN_OPTIONS', 'print_stacktrace=1:halt_on_error=1')
    env.pop('IRON_CC_FLAGS', None)
    env.pop('IRON_LIR_SKIP', None)
    try:
        b = subprocess.run([ironc] + args, capture_output=True, text=True, encoding='utf-8',
                           errors='replace', timeout=timeout(), cwd=cwd, env=env)
        return b, False, b.stdout + b.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'') + (e.stderr or b'')
        if isinstance(out, bytes):
            out = out.decode('utf-8', 'replace')
        return None, True, out


def check_mutant(ironc, d, name, text):
    src = os.path.join(d, name + '.iron')
    with open(src, 'w', encoding='utf-8') as f:
        f.write(text)
    # the front end first (fast); a mutant it accepts is built in full
    b, to, log = run_ironc(ironc, ['check', src], d)
    bad = classify(b, to, log)
    if bad or b.returncode != 0:
        return bad
    exe = os.path.join(d, name + '.bin')
    b, to, log = run_ironc(ironc, ['build', src, '-o', exe], d)
    bad = classify(b, to, log)
    if bad or b.returncode != 0 or not os.path.exists(exe):
        return bad
    try:
        r = subprocess.run([exe], capture_output=True, timeout=10, cwd=d)
        err = r.stderr.decode('utf-8', 'replace')
        # (an Iron panic aborts too: only a crash the allocator caught counts)
        if -r.returncode in SIGNALS and not (r.returncode == -signal.SIGABRT and 'iron:' in err):
            return 'signal', (f'the program died of signal {-r.returncode}\n'
                              + r.stderr.decode('utf-8', 'replace')[-2000:])
    except subprocess.TimeoutExpired:
        pass
    finally:
        if os.path.exists(exe):
            os.remove(exe)
    return None


def check(ironc, outdir, area, seed):
    g, src_text, expected = cg_fuzz.program(area, seed)
    name = f'{area}_{seed}'
    d = os.path.join(outdir, name)
    os.makedirs(d, exist_ok=True)
    for fname, content in (getattr(expected, 'files', None) or {}).items():
        with open(os.path.join(d, fname), 'w', encoding='utf-8', newline='') as f:
            f.write(content)
    fails = []
    for k in range(MUTANTS):
        r = random.Random(f'mutate-{area}-{seed}-{k}')
        text = mutant(src_text, r)
        if text == src_text:
            continue
        mname = f'{name}_m{k}'
        bad = check_mutant(ironc, d, mname, text)
        if bad:
            fails.append((mname, bad))
        else:
            os.remove(os.path.join(d, mname + '.iron'))
    if not fails:
        shutil.rmtree(d, ignore_errors=True)
        return 'ok', ''
    with open(os.path.join(d, 'failure.txt'), 'w', encoding='utf-8') as f:
        for mname, (kind, msg) in fails:
            f.write(f'[{mname}] {kind}\n{msg}\n\n')
    return 'fail', '\n'.join(f'[{m}] {k.upper()}: {msg[:600]}' for m, (k, msg) in fails)
