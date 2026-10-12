"""Shared pieces of the standard library program fuzzers (pf_url, pf_iofs,
pf_mathlib, pf_netaddr, pf_strmore, ...): Iron literals for arbitrary bytes,
the escaped text form the probes print, and a ProbeGen that emits tagged
lines.

Every probe prints its results through `q(s)`, which writes printable ASCII
as is and every other byte as \\xHH, so a result holding a zero byte, a
newline or bytes that are not UTF-8 still prints as one comparable line.
"""
from pf_probe import Probe, ProbeGen
from pf_lang import Bail  # noqa: F401

Q_HELPER = r'''func q(s: String) -> String {
    val hex = "0123456789abcdef"
    var out = ""
    var i = 0
    while i < s.byte_len() {
        val b = s.byte_at(i)
        if b < 32 or b > 126 or b == 92 {
            out += "\\x" + hex.char_at(b / 16) + hex.char_at(b % 16)
        } else {
            out += String.from_byte(b)
        }
        i += 1
    }
    return out
}

func pk(n: Int) -> Int {
    return n
}'''


def qtext(b):
    """The text q() prints for the bytes (or str, taken as UTF-8) b."""
    if isinstance(b, str):
        b = b.encode('utf-8', 'surrogateescape')
    out = []
    for x in b:
        if x < 32 or x > 126 or x == 92:
            out.append('\\x%02x' % x)
        else:
            out.append(chr(x))
    return ''.join(out)


def lit(b):
    """An Iron expression whose value is the bytes (or str) b. Printable
    ASCII and valid multi-byte UTF-8 go in a literal; control bytes, the
    zero byte and bytes that are not UTF-8 are spliced in with
    String.from_byte."""
    if isinstance(b, str):
        b = b.encode('utf-8', 'surrogateescape')
    parts = []
    cur = []

    def flush():
        if cur:
            parts.append('"' + ''.join(cur) + '"')
            cur.clear()

    i = 0
    while i < len(b):
        x = b[i]
        if len(cur) >= 1000:
            flush()
        if 32 <= x <= 126:
            ch = chr(x)
            cur.append({'\\': '\\\\', '"': '\\"', '{': '\\{', '}': '\\}'}.get(ch, ch))
            i += 1
            continue
        if x == 10:
            cur.append('\\n')
            i += 1
            continue
        if x == 9:
            cur.append('\\t')
            i += 1
            continue
        n = 2 if 0xC2 <= x <= 0xDF else 3 if 0xE0 <= x <= 0xEF else 4 if 0xF0 <= x <= 0xF4 else 0
        if n and i + n <= len(b):
            try:
                cur.append(b[i:i + n].decode('utf-8'))
                i += n
                continue
            except UnicodeDecodeError:
                pass
        flush()
        parts.append(f'String.from_byte({x})')
        i += 1
    flush()
    if not parts:
        return '""'
    return parts[0] if len(parts) == 1 else '(' + ' + '.join(parts) + ')'


class StdGen(ProbeGen):
    """A ProbeGen with the q() helper and tagged output lines."""

    def __init__(self, rng):
        super().__init__(rng)
        self.need('q', Q_HELPER)

    def line(self, code, out, exprs, texts):
        """Append a println of the expressions (Iron source of String
        values, each printed through q) and the expected line."""
        tag = self.fresh('t')
        code.append(f'println("{tag}' + ''.join(' {q(' + e + ')}' for e in exprs) + '")')
        out.append(tag + ''.join(' ' + qtext(t) for t in texts))

    def raw(self, code, out, exprs, texts):
        """Like line, for non-String expressions printed as they are."""
        tag = self.fresh('t')
        code.append(f'println("{tag}' + ''.join(' {' + e + '}' for e in exprs) + '")')
        out.append(tag + ''.join(' ' + str(t) for t in texts))


def b2s(v):
    return 'true' if v else 'false'
