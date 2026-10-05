#!/usr/bin/env python3
"""macos_stubs.py - write .tbd link stubs for the symbols a bundle imports.

usage: macos_stubs.py <sdk> <arch> <needed-symbols-file> <out-dir>

Reads the text-based stubs (TBD v4) of the macOS SDK: every framework under
System/Library/Frameworks plus /usr/lib/libobjc and /usr/lib/libSystem. Each
needed linker symbol (`_CFRelease`, `_OBJC_CLASS_$_NSWindow`, ...) is
attributed to the library a program links to get it, the way `-framework X`
or `-lSystem` would: symbols of a sub-framework or sub-library that the
umbrella carries inline (HIServices inside ApplicationServices) or
re-exports (libsystem_c through libSystem) count as the umbrella's. A
library that exports a symbol itself wins over an umbrella that only
re-exports it.

For each library used, writes <out-dir>/<Name>.tbd listing only those
symbols, and prints one line per stub: `<file> <install-name> <count>`.
Exits 1, naming them, if some symbols are exported by no SDK library.
"""
import os
import re
import sys

SYMBOL_KEYS = ("symbols", "weak-symbols", "thread-local-symbols")
OBJC_KEYS = {
    "objc-classes": ("_OBJC_CLASS_$_", "_OBJC_METACLASS_$_"),
    "objc-eh-types": ("_OBJC_EHTYPE_$_",),
    "objc-ivars": ("_OBJC_IVAR_$_",),
}


def documents(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    for doc in re.split(r"^--- !tapi-tbd.*$", text, flags=re.M)[1:]:
        yield doc


def logical_lines(doc):
    """The document's lines with every bracketed list joined onto one."""
    out, buf, depth = [], "", 0
    for line in doc.splitlines():
        if depth > 0:
            buf += " " + line.strip()
        else:
            buf = line
        depth += line.count("[") - line.count("]")
        if depth <= 0:
            out.append(buf)
            buf, depth = "", 0
    if buf:
        out.append(buf)
    return out


def items(body):
    return [x.strip().strip("'\"") for x in body.split(",") if x.strip()]


def exported(doc, arch_targets):
    """Linker symbol names a TBD document exports for the target."""
    names = set()
    in_exports = False
    targets_ok = False
    for line in logical_lines(doc):
        if re.match(r"^(exports|reexports):", line):
            in_exports = True
            continue
        if re.match(r"^\S", line):
            in_exports = False
            continue
        if not in_exports:
            continue
        m = re.match(r"^\s*-\s*targets:\s*\[(.*)\]", line)
        if m:
            targets_ok = any(t in arch_targets for t in items(m.group(1)))
            continue
        m = re.match(r"^\s+([a-z-]+):\s*\[(.*)\]", line)
        if not m or not targets_ok:
            continue
        key, body = m.group(1), m.group(2)
        if key in SYMBOL_KEYS:
            names.update(items(body))
        elif key in OBJC_KEYS:
            for name in items(body):
                for prefix in OBJC_KEYS[key]:
                    names.add(prefix + name.lstrip("_"))
    return names


def install_name(doc):
    m = re.search(r"^install-name:\s*'?([^'\n]+)'?", doc, flags=re.M)
    return m.group(1).strip() if m else None


def reexported(doc, arch_targets):
    """Install names a TBD document re-exports for the target."""
    libs = set()
    active = False
    targets_ok = False
    for line in logical_lines(doc):
        if re.match(r"^reexported-libraries:", line):
            active = True
            continue
        if re.match(r"^\S", line):
            active = False
            continue
        if not active:
            continue
        m = re.match(r"^\s*-\s*targets:\s*\[(.*)\]", line)
        if m:
            targets_ok = any(t in arch_targets for t in items(m.group(1)))
            continue
        m = re.match(r"^\s+libraries:\s*\[(.*)\]", line)
        if m and targets_ok:
            libs.update(items(m.group(1)))
    return libs


def main():
    sdk, arch, needed_file, out = sys.argv[1:5]
    arch_targets = {"arm64": {"arm64-macos", "arm64e-macos"},
                    "x86_64": {"x86_64-macos"}}[arch]
    stub_files = [os.path.join(sdk, "usr/lib/libSystem.B.tbd"),
                  os.path.join(sdk, "usr/lib/libobjc.A.tbd")]
    fw_root = os.path.join(sdk, "System/Library/Frameworks")
    # Not for applications: DriverKit is for driver extensions, Kernel for
    # kernel extensions, and System duplicates libSystem.
    skip = {"DriverKit", "Kernel", "System"}
    for fw in sorted(os.listdir(fw_root)):
        name = fw[:-len(".framework")] if fw.endswith(".framework") else None
        if name in skip:
            continue
        path = os.path.join(fw_root, fw, name + ".tbd") if name else None
        if path and os.path.isfile(path):
            stub_files.append(path)

    # Every library in the SDK by install name, for following re-exports.
    by_name = {}
    for root in (os.path.join(sdk, "usr/lib"), fw_root):
        for dirpath, _, files in os.walk(root):
            for fn in files:
                if fn.endswith(".tbd"):
                    for doc in documents(os.path.join(dirpath, fn)):
                        name = install_name(doc)
                        if name:
                            by_name.setdefault(name, doc)

    def closure(name, seen):
        """Symbols a library exports, its re-exports' included."""
        doc = by_name.get(name)
        if doc is None or name in seen:
            return set()
        seen.add(name)
        syms = set(exported(doc, arch_targets))
        for lib in reexported(doc, arch_targets):
            syms |= closure(lib, seen)
        return syms

    # libSystem and libobjc come first, whether they export a symbol
    # themselves or through a sub-library: a framework that happens to
    # export a libc function too is not where a program should get it.
    preferred = {}
    direct = {}    # symbol -> umbrella install name (exported by the umbrella itself)
    indirect = {}  # symbol -> umbrella install name (inline sub-library or re-export)
    for path in stub_files[:2]:
        docs = list(documents(path))
        if docs:
            umbrella = install_name(docs[0])
            for sym in closure(umbrella, set()):
                preferred.setdefault(sym, umbrella)
    for path in stub_files[2:]:
        docs = list(documents(path))
        if not docs:
            continue
        umbrella = install_name(docs[0])
        for sym in exported(docs[0], arch_targets):
            direct.setdefault(sym, umbrella)
        rest = set()
        for doc in docs[1:]:
            rest |= exported(doc, arch_targets)
        for lib in reexported(docs[0], arch_targets):
            rest |= closure(lib, {umbrella})
        for sym in rest:
            indirect.setdefault(sym, umbrella)

    # ld64 binds lazy symbols through dyld_stub_binder; libSystem has it
    # even where a stub file no longer lists it.
    preferred.setdefault("dyld_stub_binder", "/usr/lib/libSystem.B.dylib")

    with open(needed_file) as f:
        needed = sorted({line.strip() for line in f if line.strip()})
    by_lib, missing = {}, []
    for sym in needed:
        lib = preferred.get(sym) or direct.get(sym) or indirect.get(sym)
        if lib:
            by_lib.setdefault(lib, []).append(sym)
        else:
            missing.append(sym)
    if missing:
        print("macos_stubs.py: no SDK library exports these symbols:", file=sys.stderr)
        for sym in missing:
            print("  " + sym, file=sys.stderr)
        return 1

    tbd_target = {"arm64": "arm64-macos", "x86_64": "x86_64-macos"}[arch]
    os.makedirs(out, exist_ok=True)
    for lib, syms in sorted(by_lib.items()):
        base = os.path.basename(lib)
        base = re.sub(r"\.[A-Z]$", "", re.sub(r"\.dylib$", "", base))
        plain = [s for s in syms if not s.startswith(("_OBJC_CLASS_$_", "_OBJC_METACLASS_$_"))]
        classes = sorted({s.split("$_", 1)[1] for s in syms
                          if s.startswith(("_OBJC_CLASS_$_", "_OBJC_METACLASS_$_"))})
        lines = ["--- !tapi-tbd", "tbd-version: 4", f"targets: [ {tbd_target} ]",
                 f"install-name: '{lib}'", "exports:", f"  - targets: [ {tbd_target} ]"]
        if plain:
            lines.append("    symbols: [ " + ", ".join(f"'{s}'" for s in plain) + " ]")
        if classes:
            lines.append("    objc-classes: [ " + ", ".join(classes) + " ]")
        lines.append("...")
        path = os.path.join(out, base + ".tbd")
        with open(path, "w") as f:
            f.write("\n".join(lines) + "\n")
        print(f"{base}.tbd {lib} {len(syms)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
