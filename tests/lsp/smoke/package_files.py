#!/usr/bin/env python3
"""A package's files are analyzed together, as `iron build` compiles them.

    package_files.py <ironls>

Drives ironls over stdio (plain Python, no pytest-lsp) on a package made in
a temporary directory:

  - a name declared in another file of the package resolves: no E0200 /
    E0202 for it, hover and go to definition reach the other file
  - a declaration without `pub` in another file is still an error (E0320)
  - a file outside src/ (tests/tool.iron) is analyzed alone
  - references from a declaration list the uses in the other files, a
    rename of a function edits every file that names it, and workspace
    symbols list the package's declarations (the workspace index is filled
    at startup)
  - the type inside `rc T` and `*T` has hover and a definition, and a
    hover signature prints those types
  - saving another file (didChangeWatchedFiles) refreshes the open
    document's diagnostics
  - references and rename of a type, a field and a method reach the type
    annotations and member accesses of the other files
  - a comment shaped like a file marker (`-- @file: ...`) inside a package
    file does not move the code after it into another file
"""
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time

FILES = {
    "iron.toml": '[package]\nname = "shop"\nversion = "0.1.0"\n',
    "src/model.iron": (
        "pub object Item {\n"
        "    pub val name: String\n"
        "    pub val cents: Int\n"
        "}\n"
        "\n"
        "pub func item_label(it: Item) -> String {\n"
        '    return "{it.name}: {it.cents}"\n'
        "}\n"
        "\n"
        "func hidden_helper() -> Int {\n"
        "    return 7\n"
        "}\n"
        "\n"
        "pub object Cart {\n"
        "    pub var total: Int\n"
        "\n"
        "    pub init(total: Int) {\n"
        "        self.total = total\n"
        "    }\n"
        "}\n"
    ),
    "src/pricing.iron": (
        "pub func add_item(cart: rc Cart, it: Item) -> Int {\n"
        "    cart.total = cart.total + it.cents\n"
        "    return cart.total\n"
        "}\n"
        "\n"
        "pub func peek(cart: *Cart) -> Int {\n"
        "    return cart.total\n"
        "}\n"
    ),
    "src/main.iron": (
        "func main() {\n"
        '    val it = Item("pen", 250)\n'
        "    val cart = rc Cart(0)\n"
        "    println(item_label(it))\n"
        '    println("{add_item(cart, it)}")\n'
        "}\n"
    ),
    # Uses of a type, a field and a method of model.iron in another file:
    # in type annotations, a constructor call and member accesses.
    "src/stock.iron": (
        "pub object Shelf {\n"
        "    pub var count: Int\n"
        "    pub val item: Item\n"
        "\n"
        "    pub init(count: Int, item: Item) {\n"
        "        self.count = count\n"
        "        self.item = item\n"
        "    }\n"
        "\n"
        "    pub func restock(n: Int) {\n"
        "        self.count = self.count + n\n"
        "    }\n"
        "}\n"
    ),
    "src/report.iron": (
        "pub func shelf_report(s: Shelf, cart: Cart) -> Int {\n"
        "    var mine: Shelf = s\n"
        "    mine.restock(2)\n"
        "    return mine.count + cart.total + s.item.cents\n"
        "}\n"
        "\n"
        "pub func new_shelf(it: Item) -> Shelf {\n"
        "    return Shelf(0, it)\n"
        "}\n"
    ),
    "tests/tool.iron": (
        "func main() {\n"
        '    println(item_label(Item("x", 1)))\n'
        "}\n"
    ),
}


class Client:
    def __init__(self, ironls, root):
        self.root = root
        self.proc = subprocess.Popen([ironls], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL)
        self.inbox = queue.Queue()
        self.diags = {}
        self.next_id = 0
        self.texts = {}
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        out = self.proc.stdout
        while True:
            length = None
            while True:
                line = out.readline()
                if not line:
                    return
                line = line.strip()
                if not line:
                    break
                key, _, value = line.partition(b":")
                if key.lower() == b"content-length":
                    length = int(value)
            if length is None:
                return
            self.inbox.put(json.loads(out.read(length)))

    def send(self, msg):
        body = json.dumps(msg).encode()
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def _absorb(self, msg):
        if msg.get("method") == "textDocument/publishDiagnostics":
            self.diags[msg["params"]["uri"]] = msg["params"]["diagnostics"]
        elif "id" in msg and "method" in msg:      # a request from the server
            self.send({"jsonrpc": "2.0", "id": msg["id"], "result": None})

    def request(self, method, params, wait=60.0):
        self.next_id += 1
        rid = self.next_id
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        end = time.time() + wait
        while time.time() < end:
            try:
                msg = self.inbox.get(timeout=0.2)
            except queue.Empty:
                continue
            if msg.get("id") == rid and "method" not in msg:
                if "error" in msg:
                    raise AssertionError("%s failed: %s" % (method, msg["error"]))
                return msg.get("result")
            self._absorb(msg)
        raise AssertionError("%s: no response" % method)

    def pump(self, until, wait=60.0):
        """Read notifications until until() holds."""
        end = time.time() + wait
        while time.time() < end:
            if until():
                return True
            try:
                self._absorb(self.inbox.get(timeout=0.2))
            except queue.Empty:
                pass
        return until()

    def uri(self, rel):
        return "file://" + os.path.join(self.root, rel)

    def open(self, rel):
        with open(os.path.join(self.root, rel)) as f:
            self.texts[rel] = f.read()
        self.diags.pop(self.uri(rel), None)
        self.notify("textDocument/didOpen", {"textDocument": {
            "uri": self.uri(rel), "languageId": "iron", "version": 1, "text": self.texts[rel]}})

    def diagnostics(self, rel):
        uri = self.uri(rel)
        if not self.pump(lambda: uri in self.diags):
            raise AssertionError("no diagnostics published for " + rel)
        return self.diags[uri]

    def at(self, rel, needle, offset=0):
        text = self.texts[rel]
        i = text.index(needle) + offset
        line = text.count("\n", 0, i)
        return {"textDocument": {"uri": self.uri(rel)},
                "position": {"line": line, "character": i - (text.rfind("\n", 0, i) + 1)}}


def codes(diags):
    return sorted(str(d.get("code")) for d in diags)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    ironls = os.path.abspath(sys.argv[1])
    failures = []

    def check(ok, what):
        print(("ok   " if ok else "FAIL ") + what)
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory(prefix="iron-lsp-package-") as tmp:
        root = os.path.realpath(tmp)
        for rel, text in FILES.items():
            path = os.path.join(root, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(text)

        c = Client(ironls, root)
        try:
            c.request("initialize", {
                "processId": os.getpid(), "rootUri": "file://" + root, "capabilities": {},
                "workspaceFolders": [{"uri": "file://" + root, "name": "shop"}]})
            c.notify("initialized", {})

            # Names of the other files resolve.
            for rel in ("src/main.iron", "src/pricing.iron", "src/model.iron"):
                c.open(rel)
            for rel in ("src/main.iron", "src/pricing.iron", "src/model.iron"):
                d = c.diagnostics(rel)
                errors = [x for x in d if x.get("severity") == 1]
                check(not errors, "%s: no errors (%s)" % (rel, [(x.get("code"), x["message"]) for x in errors]))

            # A file outside src/ is not part of the package.
            c.open("tests/tool.iron")
            check("E0200" in codes(c.diagnostics("tests/tool.iron")),
                  "tests/tool.iron is analyzed alone (E0200 for a package name)")

            # Hover and definition reach the other file.
            h = c.request("textDocument/hover", c.at("src/main.iron", "item_label", 2))
            check(bool(h) and "pub func item_label(it: Item) -> String" in h["contents"]["value"],
                  "hover on a function of another file: %s" % (h and h["contents"]["value"]))
            d = c.request("textDocument/definition", c.at("src/main.iron", "item_label", 2))
            target = d[0].get("targetUri", d[0].get("uri")) if d else None
            check(bool(target) and target.endswith("/src/model.iron"),
                  "definition of a function of another file: %s" % target)
            check(bool(d) and d[0].get("targetRange", d[0].get("range"))["start"]["line"] == 5,
                  "definition lands on the declaration's line")

            # `rc T` and `*T`: the type inside, and the printed signature.
            for needle, off in (("rc Cart", 4), ("*Cart", 2)):
                h = c.request("textDocument/hover", c.at("src/pricing.iron", needle, off))
                check(bool(h) and "object Cart" in h["contents"]["value"],
                      "hover on the type in `%s`: %s" % (needle, h and h["contents"]["value"]))
                d = c.request("textDocument/definition", c.at("src/pricing.iron", needle, off))
                target = d[0].get("targetUri", d[0].get("uri")) if d else None
                check(bool(target) and target.endswith("/src/model.iron"),
                      "definition of the type in `%s`: %s" % (needle, target))
            h = c.request("textDocument/hover", c.at("src/main.iron", "add_item", 2))
            check(bool(h) and "add_item(cart: rc Cart, it: Item) -> Int" in h["contents"]["value"],
                  "hover prints an rc parameter: %s" % (h and h["contents"]["value"]))
            h = c.request("textDocument/hover", c.at("src/pricing.iron", "peek", 2))
            check(bool(h) and "peek(cart: *Cart) -> Int" in h["contents"]["value"],
                  "hover prints a pointer parameter: %s" % (h and h["contents"]["value"]))

            # References from the declaring file list the other files' uses.
            p = c.at("src/model.iron", "item_label", 2)
            p["context"] = {"includeDeclaration": True}
            refs = c.request("textDocument/references", p)
            files = sorted(set(os.path.basename(r["uri"]) for r in refs or []))
            check(files == ["main.iron", "model.iron"], "references from the declaration: %s" % files)

            # Renaming a function, from its declaration or from a use, edits
            # every file that names it.
            for rel, needle in (("src/model.iron", "item_label"), ("src/main.iron", "item_label")):
                p = c.at(rel, needle, 2)
                p["newName"] = "label_of"
                edit = c.request("textDocument/rename", p) or {}
                changes = edit.get("changes") or {
                    d["textDocument"]["uri"]: d["edits"] for d in edit.get("documentChanges", [])}
                got = sorted((os.path.basename(u), len(e)) for u, e in changes.items())
                check(got == [("main.iron", 1), ("model.iron", 1)],
                      "rename from %s edits both files: %s" % (rel, got))

            # A type, a field and a method: references and rename reach the
            # type annotations, constructor calls and member accesses of the
            # other files, from the declaration and from a use (#767).
            for rel in ("src/stock.iron", "src/report.iron"):
                c.open(rel)
                d = c.diagnostics(rel)
                errors = [x for x in d if x.get("severity") == 1]
                check(not errors, "%s: no errors (%s)" % (rel, [(x.get("code"), x["message"]) for x in errors]))

            def sites(result):
                return sorted((os.path.basename(r["uri"]), r["range"]["start"]["line"],
                               r["range"]["start"]["character"]) for r in result or [])

            def rename_sites(edit):
                edit = edit or {}
                changes = edit.get("changes") or {
                    d["textDocument"]["uri"]: d["edits"] for d in edit.get("documentChanges", [])}
                return sorted((os.path.basename(u), e["range"]["start"]["line"],
                               e["range"]["start"]["character"])
                              for u, edits in changes.items() for e in edits)

            cases = [
                # what, declaration site, a use in another file, every site
                ("type Shelf", ("src/stock.iron", "Shelf", 1), ("src/report.iron", "Shelf", 1),
                 [("report.iron", 0, 25), ("report.iron", 1, 14), ("report.iron", 6, 32),
                  ("report.iron", 7, 11), ("stock.iron", 0, 11)]),
                ("field count", ("src/stock.iron", "count", 1), ("src/report.iron", "count", 1),
                 [("report.iron", 3, 16), ("stock.iron", 1, 12), ("stock.iron", 5, 13),
                  ("stock.iron", 10, 13), ("stock.iron", 10, 26)]),
                ("method restock", ("src/stock.iron", "restock", 1), ("src/report.iron", "restock", 1),
                 [("report.iron", 2, 9), ("stock.iron", 9, 13)]),
                ("field cents of another file's type", ("src/model.iron", "cents", 1),
                 ("src/report.iron", "cents", 1),
                 [("model.iron", 2, 12), ("model.iron", 6, 27), ("pricing.iron", 1, 33),
                  ("report.iron", 3, 44)]),
            ]
            for what, decl_at, use_at, want in cases:
                for where, (rel, needle, off) in (("declaration", decl_at), ("use", use_at)):
                    p = c.at(rel, needle, off)
                    p["context"] = {"includeDeclaration": True}
                    got = sites(c.request("textDocument/references", p))
                    check(got == want, "references of %s from its %s: %s" % (what, where, got))
                    p = c.at(rel, needle, off)
                    prep = c.request("textDocument/prepareRename", p)
                    check(bool(prep), "prepareRename of %s from its %s" % (what, where))
                    p["newName"] = "renamed_x"
                    got = rename_sites(c.request("textDocument/rename", p))
                    check(got == want, "rename of %s from its %s: %s" % (what, where, got))
            # Without the declaration.
            p = c.at("src/report.iron", "count", 1)
            p["context"] = {"includeDeclaration": False}
            got = sites(c.request("textDocument/references", p))
            check(got == [("report.iron", 3, 16), ("stock.iron", 5, 13), ("stock.iron", 10, 13),
                          ("stock.iron", 10, 26)],
                  "references of a field without its declaration: %s" % got)
            # A field of the same name on another type is left alone.
            p = c.at("src/model.iron", "total", 1)
            p["context"] = {"includeDeclaration": True}
            got = sites(c.request("textDocument/references", p))
            check(("report.iron", 3, 29) in got and all(f != "stock.iron" for f, _, _ in got),
                  "references of Cart.total: %s" % got)

            # A comment shaped like a file marker inside a package file is a
            # comment: the code after it stays in its own file. notes.iron
            # names main.iron in such comments, by URI and by path.
            notes = (
                '-- @file: "%s" @line: 1\n'
                '-- @file: "%s" @line: 1 @lines: 9\n'
                "func notes_private() -> Int {\n"
                "    return 1\n"
                "}\n"
                "\n"
                "pub func notes_count() -> Int {\n"
                "    return notes_private() + 1\n"
                "}\n") % (c.uri("src/main.iron"), os.path.join(root, "src/main.iron"))
            with open(os.path.join(root, "src/notes.iron"), "w") as f:
                f.write(notes)
            c.notify("workspace/didChangeWatchedFiles", {
                "changes": [{"uri": c.uri("src/notes.iron"), "type": 1}]})
            marker_main = FILES["src/main.iron"].replace(
                "    println(item_label(it))\n",
                '    println(item_label(it))\n    println("{notes_count()} {notes_private()}")\n')
            c.texts["src/main.iron"] = marker_main
            c.diags.pop(c.uri("src/main.iron"), None)
            c.notify("textDocument/didChange", {
                "textDocument": {"uri": c.uri("src/main.iron"), "version": 2},
                "contentChanges": [{"text": marker_main}]})
            got = c.diagnostics("src/main.iron")
            errors = [x for x in got if x.get("severity") == 1]
            check(codes(errors) == ["E0320"] and "notes_private" in errors[0]["message"],
                  "a function after a marker shaped comment stays private to its file: %s"
                  % [(x.get("code"), x["message"]) for x in errors])
            d = c.request("textDocument/definition", c.at("src/main.iron", "notes_count", 2))
            target = d[0].get("targetUri", d[0].get("uri")) if d else None
            line = d[0].get("targetRange", d[0].get("range"))["start"]["line"] if d else None
            check(bool(target) and target.endswith("/src/notes.iron") and line == 6,
                  "a declaration after a marker shaped comment keeps its file and line: %s:%s"
                  % (target, line))
            c.open("src/notes.iron")
            d = c.diagnostics("src/notes.iron")
            check(not [x for x in d if x.get("severity") == 1],
                  "the file with the marker shaped comments has no errors: %s" % codes(d))
            c.texts["src/main.iron"] = FILES["src/main.iron"]

            # Workspace symbols: the index is filled at startup.
            syms = c.request("workspace/symbol", {"query": "item_label"})
            check(any(s["name"] == "item_label" for s in syms or []),
                  "workspace/symbol finds item_label (%d results)" % len(syms or []))

            # Visibility between files is checked as in a build.
            main_text = FILES["src/main.iron"].replace(
                "    println(item_label(it))\n",
                '    println(item_label(it))\n    println("{hidden_helper()}")\n')
            c.diags.pop(c.uri("src/main.iron"), None)
            c.notify("textDocument/didChange", {
                "textDocument": {"uri": c.uri("src/main.iron"), "version": 3},
                "contentChanges": [{"text": main_text}]})
            got = codes(c.diagnostics("src/main.iron"))
            check("E0320" in got, "a private function of another file is E0320: %s" % got)

            # Saving the other file refreshes this one: hidden_helper becomes pub.
            model = FILES["src/model.iron"].replace("func hidden_helper", "pub func hidden_helper")
            with open(os.path.join(root, "src/model.iron"), "w") as f:
                f.write(model)
            c.diags.pop(c.uri("src/main.iron"), None)
            c.notify("workspace/didChangeWatchedFiles", {
                "changes": [{"uri": c.uri("src/model.iron"), "type": 2}]})
            got = c.diagnostics("src/main.iron")
            check(not [x for x in got if x.get("severity") == 1],
                  "diagnostics refresh when another file is saved: %s" % codes(got))

            c.request("shutdown", None)
            c.notify("exit", None)
        finally:
            try:
                c.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                c.proc.kill()

    if failures:
        print("%d check(s) failed" % len(failures))
        return 1
    print("package_files OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
