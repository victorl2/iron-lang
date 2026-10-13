"""The io, os and FileHandle modules (manual 9.5, 9.6.1, 9.9, networking.md
"Text and binary file operations"): random sequences of file system calls
in a sandbox directory, against a model of the tree.

Each program makes a fresh directory under /tmp and works inside it:
write / append (text and bytes) of random binary data with zero bytes and
of large generated blobs, bounded reads with limits around the size,
file_info, copy and move with and without overwrite, the simple functions
(read_file, write_file, append_file, read_lines, file_exists, is_dir,
create_dir, delete_file, list_files), unicode, quoted and 255-byte names,
paths through a file (ENOTDIR), missing parents, directories used as files,
the empty path and paths holding a zero byte, the path helpers (basename,
dirname, extension, join_path) on arbitrary bytes, FileHandle open / close /
drop (many handles, close twice, an existing file keeps its contents), and
OS.env / env_or / has_env on variables the harness sets (empty, unicode,
'=' in the value, 5000 bytes) and on names that cannot exist. When the
fuzzer does not run as root it also prepares files and directories without
permissions and expects 8001 for them. The program removes everything it
made, and the last line checks the sandbox is gone.

The error codes are the ones iron_errors.h gives (8000 not found, 8001
permission, 8002 invalid argument, 8003 too large, 8007 already exists,
8008 not a directory, 8009 is a directory); the manual only says `error`
is 0 on success (see the report).
"""
import os
import random

from stdfz_common import StdGen, Probe, lit, qtext, b2s
from pf_lang import Expected

# Variables every program may read; set here, in the process that runs the
# programs (prog_fuzz imports the generator in the worker that runs them).
ENV = {
    'IRONFZ_EMPTY': '',
    'IRONFZ_UNI': '日本 é',
    'IRONFZ_EQ': 'a=b=c',
    'IRONFZ_SP': ' spaced  out ',
    'IRONFZ_LONG': 'x' * 5000,
    'IRONFZ_NL': 'line1\nline2',
}
os.environ.update(ENV)

ROOT_USER = hasattr(os, 'geteuid') and os.geteuid() == 0
PERM_DIR = '/tmp/ironfz_perm_%d' % (os.getuid() if hasattr(os, 'getuid') else 0)


def _prepare_perm():
    """Files the programs cannot read or write (only meaningful when not
    root: root bypasses the permission bits)."""
    if ROOT_USER:
        return
    try:
        os.makedirs(PERM_DIR, exist_ok=True)
        for name, mode, body in (('noread', 0o000, b'secret'), ('nowrite', 0o444, b'ro')):
            p = os.path.join(PERM_DIR, name)
            if not os.path.exists(p):
                with open(p, 'wb') as f:
                    f.write(body)
            os.chmod(p, mode)
        d = os.path.join(PERM_DIR, 'rodir')
        os.makedirs(d, exist_ok=True)
        if not os.path.exists(os.path.join(d, 'inside')):
            with open(os.path.join(d, 'inside'), 'wb') as f:
                f.write(b'in')
        os.chmod(d, 0o555)
    except OSError:
        pass


_prepare_perm()

MSG = {0: '', 8000: 'file not found', 8001: 'file permission denied', 8002: 'invalid file argument',
       8003: 'file exceeds byte limit', 8004: 'file read failed', 8005: 'file write failed',
       8007: 'destination already exists', 8008: 'path is not a directory',
       8009: 'path is a directory'}

NAMES = ['a', 'b', 'c.txt', 'é', '日本.dat', 'x y', 'q"t', "it's", '.hid', 'tab\tn',
         'L' * 200, '%41', 'a.b.c', '-dash', 'ü', 'back\\slash', 'n' * 255, '{br}', 'z']
LONGNAME = 'm' * 256

HELPERS = '''func info_s(i: FileInfo) -> String {
    return "{i.exists} {i.is_file} {i.is_dir} {i.size} {i.error} " + q(i.error_message)
}

func listing(p: String) -> String {
    var xs = IO.list_files(p).split("\\n")
    xs.sort()
    var o = ""
    for x in xs {
        if x != "" {
            o += q(x) + "|"
        }
    }
    return o
}

func blob(n: Int, seed: Int) -> String {
    -- a 997-byte pseudo-random chunk repeated (an append per byte would
    -- be quadratic, #597)
    var x = seed
    var chunk = ""
    for i in range(min(n, 997)) {
        x = (x * 1103515245 + 12345) % 2147483648
        chunk += String.from_byte(x / 65536 % 256)
    }
    if n <= 997 {
        return chunk
    }
    return chunk.repeat(n / 997) + chunk.byte_slice(0, n % 997)
}

func digest(s: String) -> Int {
    var h = 7
    var i = 0
    while i < s.byte_len() {
        h = (h * 31 + s.byte_at(i)) % 1000000007
        i += 1
    }
    return h
}

func sandbox() -> String {
    Math.seed(Time.now_ns())
    val root = "/tmp/ironfz_io_{Time.now_ns()}_{Math.random_int(0, 1000000000)}"
    IO.create_dir(root)
    return root
}'''


def blob(n, seed):
    """The bytes the Iron blob(n, seed) helper makes."""
    x = seed
    chunk = bytearray()
    for _ in range(min(n, 997)):
        x = (x * 1103515245 + 12345) % 2147483648
        chunk.append(x // 65536 % 256)
    chunk = bytes(chunk)
    if n <= 997:
        return chunk
    return chunk * (n // 997) + chunk[:n % 997]


def digest(b):
    h = 7
    for x in b:
        h = (h * 31 + x) % 1000000007
    return h


def is_long(rel):
    return any(len(c.encode()) > 255 for c in rel.split('/'))


class FS:
    """The sandbox tree: files (rel path -> bytes) and dirs (rel paths;
    '' is the sandbox itself)."""

    def __init__(self):
        self.files = {}
        self.dirs = {''}

    @staticmethod
    def join(a, b):
        return b if a == '' else a + '/' + b

    def lookup(self, rel):
        """'file', 'dir', 'missing' (the parent exists), 'ENOENT',
        'ENOTDIR' or 'ENAMETOOLONG'."""
        if rel == '':
            return 'dir'
        comps = rel.split('/')
        cur = ''
        for c in comps[:-1]:
            if len(c.encode()) > 255:
                return 'ENAMETOOLONG'
            nxt = self.join(cur, c)
            if nxt in self.files:
                return 'ENOTDIR'
            if nxt not in self.dirs:
                return 'ENOENT'
            cur = nxt
        if len(comps[-1].encode()) > 255:
            return 'ENAMETOOLONG'
        full = self.join(cur, comps[-1])
        if full in self.files:
            return 'file'
        if full in self.dirs:
            return 'dir'
        return 'missing'

    def children(self, rel):
        pre = '' if rel == '' else rel + '/'
        out = set()
        for p in list(self.files) + list(self.dirs):
            if p and p.startswith(pre) and '/' not in p[len(pre):]:
                out.add(p[len(pre):])
        return sorted(out, key=lambda s: s.encode())


ERRNO = {'ENOENT': 8000, 'missing': 8000, 'ENOTDIR': 8008, 'ENAMETOOLONG': 8002, 'dir': 8009}


class IoGen(StdGen):
    NPROBES = (8, 18)

    def __init__(self, rng):
        super().__init__(rng)
        for m in ('io', 'os', 'time', 'math'):
            self.need_import(m)
        self.need('io', HELPERS)
        self.fs = FS()
        self.perm = not ROOT_USER and os.path.isdir(PERM_DIR)

    # ------------------------------------------------------------ paths
    def rel_target(self, want='any'):
        """A path relative to the sandbox and its expression."""
        r = self.r
        fs = self.fs
        k = r.random()
        dirs = sorted(fs.dirs)
        files = sorted(fs.files)
        if want == 'existing_file' and files:
            return r.choice(files)
        if k < 0.35 and files and want != 'new':
            return r.choice(files)
        if k < 0.45 and len(dirs) > 1 and want != 'new':
            return r.choice(dirs[1:])
        if k < 0.52 and files:
            return r.choice(files) + '/' + r.choice(NAMES[:6])
        if k < 0.58:
            return 'nodir/' + r.choice(NAMES[:6])
        base = r.choice(dirs)
        name = r.choice(NAMES) if r.random() > 0.02 else LONGNAME
        if len(base.encode()) + len(name.encode()) > 700:
            base, name = '', r.choice(NAMES[:6])
        return FS.join(base, name)

    def pexpr(self, rel):
        return 'root' if rel == '' else f'root + {lit("/" + rel)}'

    def data(self):
        """(Iron expression, bytes)."""
        r = self.r
        k = r.random()
        if k < 0.45:
            b = bytes(r.choice([0, 10, 13, 65, 97, 200, 255, 0xc3, 0xa9, 32, 9])
                      for _ in range(r.randint(0, 12)))
            return lit(b), b
        if k < 0.7:
            s = r.choice(['', 'hello', 'one\ntwo\n', 'no newline', '\n\n', 'café 日本',
                          'a\r\nb\r\n', 'x\x00y', '\x00'])
            return lit(s), s.encode()
        n = r.choice([0, 1, 255, 4095, 4096, 4097, 20000, 65539])
        seed = r.randint(1, 10**6)
        return f'blob({n}, {seed})', blob(n, seed)

    # ------------------------------------------------------------ probes
    def p_write(self):
        r = self.r
        fs = self.fs
        rel = self.rel_target()
        de, data = self.data()
        fn = r.choice(['write_text', 'write_bytes', 'append_text', 'append_bytes'])
        st = fs.lookup(rel)
        if st in ('file', 'missing'):
            err, nbytes = 0, len(data)
            if fn.startswith('append') and st == 'file':
                fs.files[rel] = fs.files[rel] + data
            else:
                fs.files[rel] = data
        else:
            err, nbytes = ERRNO[st], 0
        n = self.fresh('w')
        code = [f'val {n} = IO.{fn}({self.pexpr(rel)}, {de})']
        out = []
        self.raw(code, out, [f'{n}.bytes', f'{n}.error', f'q({n}.error_message)'],
                 [nbytes, err, qtext(MSG[err])])
        return Probe(code, out)

    def read_expect(self, rel, mx):
        st = self.fs.lookup(rel)
        if mx < 0:
            return 8002, b''
        if st == 'file':
            d = self.fs.files[rel]
            if len(d) > mx:
                return 8003, b''
            return 0, d
        return ERRNO[st], b''

    def p_read(self):
        r = self.r
        rel = self.rel_target() if r.random() < 0.9 else ''
        size = len(self.fs.files.get(rel, b''))
        mx = r.choice([-1, 0, size - 1, size, size + 1, 1 << 40, 9223372036854775807, 4096])
        err, d = self.read_expect(rel, mx)
        n = self.fresh('r')
        fn = r.choice(['read_text', 'read_bytes'])
        code = [f'val {n} = IO.{fn}({self.pexpr(rel)}, {mx})']
        out = []
        a = r.randint(0, max(0, len(d)))
        b = r.randint(a, min(len(d), a + 40))
        self.raw(code, out, [f'{n}.error', f'q({n}.error_message)', f'{n}.data.byte_len()',
                             f'digest({n}.data)', f'q({n}.data.byte_slice({a}, {b}))'],
                 [err, qtext(MSG[err]), len(d), digest(d), qtext(d[a:b])])
        return Probe(code, out)

    def p_info(self):
        r = self.r
        rel = self.rel_target() if r.random() < 0.9 else ''
        st = self.fs.lookup(rel)
        if st == 'file':
            t = (True, True, False, len(self.fs.files[rel]), 0)
        elif st == 'dir':
            t = (True, False, True, 0, 0)
        elif st in ('missing', 'ENOENT'):
            t = (False, False, False, 0, 0)
        else:
            t = (False, False, False, 0, ERRNO[st])
        n = self.fresh('i')
        code = [f'val {n} = IO.file_info({self.pexpr(rel)})']
        out = []
        txt = ' '.join([b2s(t[0]), b2s(t[1]), b2s(t[2]), str(t[3]), str(t[4]), qtext(MSG[t[4]])])
        mod = 'true' if t[0] else 'false'
        self.raw(code, out, [f'info_s({n})', f'{n}.modified_unix > 1000000000'], [txt, mod])
        return Probe(code, out)

    def p_copy_move(self):
        r = self.r
        fs = self.fs
        mv = r.random() < 0.5
        ow = r.random() < 0.5
        src = self.rel_target('existing_file' if r.random() < 0.7 else 'any')
        dst = self.rel_target()
        alias = False
        if r.random() < 0.1 and fs.lookup(src) == 'file':
            dst, alias = src, True
        ss, ds = fs.lookup(src), fs.lookup(dst)
        nbytes = 0
        if ss != 'file':
            err = ERRNO[ss]
        elif ds in ('file', 'dir'):
            if dst == src:
                err = 8002
            elif not ow:
                err = 8007
            elif ds == 'dir':
                err = 8009
            else:
                err = 0
        elif ds == 'missing':
            err = 0
        else:
            err = ERRNO[ds]
        if err == 0:
            nbytes = len(fs.files[src])
            fs.files[dst] = fs.files[src]
            if mv:
                del fs.files[src]
        n = self.fresh('c')
        dexpr = self.pexpr(dst)
        if alias:
            dexpr = f'root + {lit("/./" + dst)}'
        fn = 'move_file' if mv else 'copy_file'
        code = [f'val {n} = IO.{fn}({self.pexpr(src)}, {dexpr}, {b2s(ow)})']
        out = []
        self.raw(code, out, [f'{n}.bytes', f'{n}.error', f'q({n}.error_message)'],
                 [nbytes, err, qtext(MSG[err])])
        return Probe(code, out)

    def p_legacy(self):
        r = self.r
        fs = self.fs
        k = r.choice(['read_file', 'write_file', 'append_file', 'file_exists', 'is_dir',
                      'create_dir', 'delete_file', 'list_files', 'read_lines', 'create_dir'])
        rel = self.rel_target()
        st = fs.lookup(rel)
        pe = self.pexpr(rel)
        code, out = [], []
        if k == 'read_file':
            d = fs.files.get(rel, b'') if st == 'file' else b''
            self.raw(code, out, [f'digest(IO.read_file({pe}))', f'IO.read_file({pe}).byte_len()'],
                     [digest(d), len(d)])
        elif k in ('write_file', 'append_file'):
            de, data = self.data()
            code.append(f'IO.{k}({pe}, {de})')
            if st in ('file', 'missing'):
                fs.files[rel] = (fs.files[rel] if st == 'file' and k == 'append_file' else b'') + data
            self.raw(code, out, [f'IO.file_exists({pe})'], [b2s(st in ('file', 'missing', 'dir'))])
        elif k == 'file_exists':
            self.raw(code, out, [f'IO.file_exists({pe})'], [b2s(st in ('file', 'dir'))])
        elif k == 'is_dir':
            self.raw(code, out, [f'IO.is_dir({pe})'], [b2s(st == 'dir')])
        elif k == 'create_dir':
            if st == 'missing':
                fs.dirs.add(rel)
            code.append(f'IO.create_dir({pe})')
            self.raw(code, out, [f'IO.is_dir({pe})'], [b2s(fs.lookup(rel) == 'dir')])
        elif k == 'delete_file':
            if st == 'file':
                del fs.files[rel]
            elif st == 'dir' and rel and not fs.children(rel):
                fs.dirs.discard(rel)
            code.append(f'IO.delete_file({pe})')
            self.raw(code, out, [f'IO.file_exists({pe})'], [b2s(fs.lookup(rel) in ('file', 'dir'))])
        elif k == 'list_files':
            if r.random() < 0.5:
                dirs = sorted(fs.dirs)
                rel = r.choice(dirs)
                st = 'dir'
                pe = self.pexpr(rel)
            names = fs.children(rel) if st == 'dir' else []
            self.raw(code, out, [f'listing({pe})'], [''.join(qtext(x) + '|' for x in names)])
        else:
            d = fs.files.get(rel, b'') if st == 'file' else b''
            lines = d.split(b'\n') if st == 'file' else []
            if lines and lines[-1] == b'':
                lines.pop()
            n = self.fresh('ls')
            code.append(f'val {n} = IO.read_lines({pe})')
            self.raw(code, out, [f'{n}.len()', f'q("|".join({n}))'],
                     [len(lines), qtext(b'|'.join(lines))])
        return Probe(code, out)

    def p_badpath(self):
        """The empty path and paths holding a zero byte name no file."""
        r = self.r
        fs = self.fs
        files = sorted(fs.files)
        if files and r.random() < 0.7:
            victim = r.choice(files)
            pe = f'root + {lit("/" + victim + chr(0) + "x")}'
        else:
            victim = None
            pe = r.choice(['""', 'String.from_byte(0)', f'root + {lit("/" + chr(0))}'])
        code, out = [], []
        k = r.choice(['read', 'write', 'info', 'copy', 'legacy_write', 'exists', 'handle', 'delete'])
        if k == 'read':
            self.raw(code, out, [f'IO.read_bytes({pe}, 100).error'], [8002])
        elif k == 'write':
            self.raw(code, out, [f'IO.write_bytes({pe}, "zz").error'], [8002])
        elif k == 'info':
            self.raw(code, out, [f'info_s(IO.file_info({pe}))'], ['false false false 0 8002 ' + qtext(MSG[8002])])
        elif k == 'copy':
            src = files[0] if files else 'nofile'
            self.raw(code, out, [f'IO.copy_file({self.pexpr(src)}, {pe}, true).error'], [8002])
        elif k == 'legacy_write':
            code.append(f'IO.write_file({pe}, "clobbered")')
        elif k == 'exists':
            self.raw(code, out, [f'IO.file_exists({pe})', f'IO.is_dir({pe})'], ['false', 'false'])
        elif k == 'handle':
            h = self.fresh('h')
            code.append(f'val {h} = FileHandle.open({pe})')
            self.raw(code, out, [f'{h}.fd'], [-1])
        else:
            code.append(f'IO.delete_file({pe})')
        if victim is not None:
            d = fs.files[victim]
            self.raw(code, out, [f'digest(IO.read_file({self.pexpr(victim)}))'], [digest(d)])
        return Probe(code, out)

    def p_pathfn(self):
        r = self.r
        pieces = ['a', 'b', '/', '//', '.', '..', '.x', 'f.txt', 'é', '\x00', 'a.b', '', 'tar.gz',
                  ' ', '\\']
        code, out = [], []
        for _ in range(r.randint(1, 4)):
            s = ''.join(r.choice(pieces) for _ in range(r.randint(0, 5)))
            b = s.encode()
            k = r.choice(['basename', 'dirname', 'extension', 'join_path'])
            if k == 'basename':
                i = b.rfind(b'/')
                v = b[i + 1:]
                self.line(code, out, [f'IO.basename({lit(b)})'], [v])
            elif k == 'dirname':
                i = b.rfind(b'/')
                v = b'.' if i < 0 else b'/' if i == 0 else b[:i]
                self.line(code, out, [f'IO.dirname({lit(b)})'], [v])
            elif k == 'extension':
                i = b.rfind(b'/')
                base = b[i + 1:]
                d = base.rfind(b'.')
                v = b'' if d <= 0 else base[d + 1:]
                self.line(code, out, [f'IO.extension({lit(b)})'], [v])
            else:
                t = ''.join(r.choice(pieces) for _ in range(r.randint(0, 3))).encode()
                v = t if b == b'' else b.rstrip(b'/') + b'/' + t
                self.line(code, out, [f'IO.join_path({lit(b)}, {lit(t)})'], [v])
        return Probe(code, out)

    def p_handle(self):
        r = self.r
        fs = self.fs
        code, out = [], []
        k = r.choice(['existing', 'new', 'many', 'twice', 'bad', 'reuse'])
        if k == 'existing' and fs.files:
            rel = r.choice(sorted(fs.files))
            h, tag = self.fresh('h'), self.fresh('t')
            code += ['{', f'    var {h} = FileHandle.open({self.pexpr(rel)})',
                     f'    println("{tag} {{{h}.fd >= 0}}")', '}']
            out.append(f'{tag} true')
            self.raw(code, out, [f'digest(IO.read_file({self.pexpr(rel)}))'], [digest(fs.files[rel])])
        elif k == 'new':
            rel = self.rel_target('new')
            st = fs.lookup(rel)
            ok = st in ('missing', 'file')
            if st == 'missing':
                fs.files[rel] = b''
            h = self.fresh('h')
            code.append(f'val {h} = FileHandle.open({self.pexpr(rel)})')
            self.raw(code, out, [f'{h}.fd >= 0', f'IO.file_exists({self.pexpr(rel)})'],
                     [b2s(ok), b2s(fs.lookup(rel) in ('file', 'dir'))])
        elif k == 'many':
            rel = self.rel_target('existing_file') if fs.files else ''
            if not fs.files or fs.lookup(rel) != 'file':
                return None
            cnt = r.choice([10, 100, 300])
            hs, ok = self.fresh('hs'), self.fresh('ok')
            code += [f'var {ok} = true', '{', f'    var {hs}: [FileHandle] = []',
                     f'    for i in range({cnt}) {{',
                     f'        {hs}.push(FileHandle.open({self.pexpr(rel)}))',
                     f'        if {hs}[{hs}.len() - 1].fd < 0 {{', f'            {ok} = false', '        }',
                     '    }', '}']
            self.raw(code, out, [ok], ['true'])
        elif k == 'twice':
            rel = self.rel_target('new')
            st = fs.lookup(rel)
            if st not in ('missing', 'file'):
                return None
            if st == 'missing':
                fs.files[rel] = b''
            h = self.fresh('h')
            code += [f'var {h} = FileHandle.open({self.pexpr(rel)})', f'{h}.close()', f'{h}.close()']
            self.raw(code, out, [f'{h}.fd'], [-1])
        elif k == 'bad':
            rel = r.choice(['nodir/x'] + [d for d in sorted(fs.dirs)])
            h = self.fresh('h')
            code.append(f'val {h} = FileHandle.open({self.pexpr(rel)})')
            self.raw(code, out, [f'{h}.fd'], [-1])
        else:
            rel = self.rel_target('new')
            st = fs.lookup(rel)
            if st not in ('missing', 'file'):
                return None
            if st == 'missing':
                fs.files[rel] = b''
            a, b2 = self.fresh('fa'), self.fresh('fb')
            code += [f'var {a} = -1', '{', f'    val h = FileHandle.open({self.pexpr(rel)})',
                     f'    {a} = h.fd', '}', f'var {b2} = -2', '{',
                     f'    val h = FileHandle.open({self.pexpr(rel)})', f'    {b2} = h.fd', '}']
            self.raw(code, out, [f'{a} == {b2}'], ['true'])
        return Probe(code, out)

    def p_env(self):
        r = self.r
        code, out = [], []
        names = list(ENV) + ['IRONFZ_UNSET', 'IRONFZ_empty', '', 'IRONFZ_EQ=a', 'IRONFZ_UNI\x00x',
                             '=', 'IRONFZ_SP\x00']
        for _ in range(r.randint(1, 4)):
            nm = r.choice(names)
            val = ENV.get(nm)
            k = r.choice(['env', 'env_or', 'has_env'])
            if k == 'has_env':
                self.raw(code, out, [f'OS.has_env({lit(nm)})'], [b2s(val is not None)])
            elif k == 'env_or':
                fb = r.choice(['', 'fallback', 'é'])
                self.line(code, out, [f'OS.env_or({lit(nm)}, {lit(fb)})'], [val if val is not None else fb])
            else:
                v = self.fresh('v')
                code.append(f'val {v} = OS.env({lit(nm)})')
                tag = self.fresh('t')
                code += [f'if {v} != null {{', f'    println("{tag} " + q({v}))', '} else {',
                         f'    println("{tag} null")', '}']
                out.append(f'{tag} ' + (qtext(val) if val is not None else 'null'))
        return Probe(code, out)

    def p_perm(self):
        if not self.perm:
            return None
        r = self.r
        code, out = [], []
        P = PERM_DIR
        k = r.choice(['read', 'write', 'append', 'into', 'copy_from', 'copy_into', 'move_into',
                      'info', 'legacy', 'list'])
        if k == 'read':
            self.raw(code, out, [f'IO.read_text({lit(P + "/noread")}, 100).error'], [8001])
        elif k == 'write':
            self.raw(code, out, [f'IO.write_text({lit(P + "/nowrite")}, "x").error'], [8001])
        elif k == 'append':
            self.raw(code, out, [f'IO.append_bytes({lit(P + "/nowrite")}, "x").error'], [8001])
        elif k == 'into':
            self.raw(code, out, [f'IO.write_bytes({lit(P + "/rodir/new")}, "x").error'], [8001])
        elif k == 'copy_from':
            dst = self.rel_target('new')
            if self.fs.lookup(dst) != 'missing':
                return None
            self.raw(code, out, [f'IO.copy_file({lit(P + "/noread")}, {self.pexpr(dst)}, false).error',
                                 f'IO.file_exists({self.pexpr(dst)})'], [8001, 'false'])
        elif k in ('copy_into', 'move_into'):
            if not self.fs.files:
                return None
            src = r.choice(sorted(self.fs.files))
            fn = 'copy_file' if k == 'copy_into' else 'move_file'
            self.raw(code, out, [f'IO.{fn}({self.pexpr(src)}, {lit(P + "/rodir/new")}, true).error',
                                 f'IO.file_exists({self.pexpr(src)})'], [8001, 'true'])
        elif k == 'info':
            self.raw(code, out, [f'info_s(IO.file_info({lit(P + "/noread")}))'],
                     ['true true false 6 0 '])
        elif k == 'legacy':
            self.raw(code, out, [f'IO.read_file({lit(P + "/noread")}).byte_len()'], [0])
        else:
            self.raw(code, out, [f'listing({lit(P + "/rodir")})'], ['inside|'])
        return Probe(code, out)

    def probe_kinds(self):
        return [(6, self.p_write), (5, self.p_read), (3, self.p_info), (4, self.p_copy_move),
                (6, self.p_legacy), (2, self.p_badpath), (2, self.p_pathfn), (2, self.p_handle),
                (1, self.p_env), (1, self.p_perm)]

    def program(self):
        self.add(Probe(['val root = sandbox()'], [], block=False))
        super().program()
        # remove everything: files, then directories deepest first
        fs = self.fs
        code = []
        for f in sorted(fs.files):
            code.append(f'IO.delete_file({self.pexpr(f)})')
        for d in sorted(fs.dirs, key=lambda p: -p.count('/') - (1 if p else 0)):
            if d:
                code.append(f'IO.delete_file({self.pexpr(d)})')
        code.append('IO.delete_file(root)')
        out = []
        self.raw(code, out, ['IO.file_exists(root)'], ['false'])
        self.add(Probe(code, out))


def generate(rng):
    g = IoGen(rng)
    g.program()
    return g
