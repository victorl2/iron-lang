# Iron LSP — VSCode extension

Iron language support for [Visual Studio Code](https://code.visualstudio.com/):
TextMate syntax highlighting and a full LSP 3.17 client that delegates to
[`ironls`](https://github.com/iron-lang/iron-lang), the Iron language server.

**Tracks:** Iron v3.1.1-alpha (current main-branch alpha). See the
**Version mismatch** section below for the exact `ironls` binary the
extension accepts.

## Iron syntax overview

Iron v3 introduces the following first-class surface features (all
highlighted by the TextMate grammar and understood by `ironls`):

- **`init` blocks** — anonymous (`init(x: Int, y: Int) { ... }`) and named
  (`init Named(x: Int) { ... }`) constructors declared as first-class
  object members; replaces the v2 receiver-method initializer pattern.
- **`patch` extensions** — reopen an existing object or primitive type
  (`patch Int { ... }`, `patch Player { ... }`) to add methods.
- **`pub` visibility** — symbol-level export modifier distinguishing
  module-public from module-private decls.
- **`pure` methods** — side-effect-restricted method annotation used by
  the compiler for memoization + reordering safety.
- **`readonly` + `mut` mutation tiers** — transitive-readonly bindings
  and explicit mutable bindings; the type system enforces compatibility
  at call boundaries.

Legacy pre-v3 keywords (`val`, `var`, `object`, `interface`, `impl`,
`func`, `for`, `while`, `if/else/elif/match`, `return`, `import`,
`comptime`, etc.) continue to work unchanged. The complete roster is 44
keywords, drift-guarded at build time by `test_grammar_keyword_drift_*`.

## Features

- Syntax highlighting for `.iron` files via the drift-guarded TextMate
  grammar (keywords are extracted directly from the compiler's lexer).
- Diagnostics, hover, go-to-definition, rename, format, code actions — all
  delivered by `ironls` via the Language Server Protocol over stdio.
- Configurable log level + trace channel for debugging.
- `Iron LSP: Diagnose` command (Command Palette) producing a self-contained
  report for bug filing.
- `Iron: Migrate v2 -> v3` command (Command Palette) — invokes
  `workspace/executeCommand iron.migrate` on ironls, which runs
  `ironc migrate` and applies the resulting WorkspaceEdit in-editor.

## Iron v3 schematic example

```iron
pub object Player {
    val name: String
    val hp: Int

    init(name: String, hp: Int) {
        self.name = name
        self.hp = hp
    }

    pure func is_alive(self) -> Bool {
        return self.hp > 0
    }
}

patch Player {
    func take_damage(mut self, amount: Int) {
        self.hp = self.hp - amount
    }
}
```

## Requirements

- VSCode 1.92.0 or newer.
- `ironls` available on `$PATH` **or** configured via the
  `iron.languageServer.path` setting. Releases of the Iron toolchain ship
  `ironls` alongside `ironc`; follow the install instructions at
  <https://iron-lang.dev/install>.
- Compatible `ironls` versions: see `ironLspCompatibleIronlsRange` in
  `package.json` (currently `>= 3.0.0, < 4.0.0`). An incompatible server
  triggers a **hard refuse** — see the "Version mismatch" section below.

## Install

Once published to the Marketplace:

```text
ext install iron-lang.iron-lsp
```

Until then, package locally:

```bash
cd editors/vscode
npm ci
npm run package      # produces iron-lsp-<version>.vsix
code --install-extension iron-lsp-*.vsix
```

## Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `iron.languageServer.path` | `""` | Absolute path to `ironls`. Empty → search `$PATH` via `which`/`where`. |
| `iron.languageServer.trace.server` | `"off"` | LSP trace output level (`off` \| `messages` \| `verbose`). |
| `iron.languageServer.logLevel` | `"info"` | Editor-side log severity (`error` \| `warn` \| `info` \| `debug`). |

Changes to `iron.languageServer.*` restart the server after a 500 ms
debounce — no manual reload required.

## Troubleshooting

### "ironls not found"

If the extension cannot locate the server, you will see one of:

- `Iron LSP: could not find "ironls" on PATH. Install from https://iron-lang.dev/install or set "iron.languageServer.path" in settings.`
- `Iron LSP: "iron.languageServer.path" points to "<path>" which is not executable. Check the path in settings.`

Each error has an **Open Settings** button that jumps to the
`iron.languageServer` settings scope.

### Version mismatch

Phase 7 HARD-22 / UI-SPEC S9 — when the extension detects an `ironls`
version outside `ironLspCompatibleIronlsRange` (`>= 3.0.0, < 4.0.0`),
it refuses to activate the language client:

> **Iron LSP: detected ironls X.Y.Z, but this extension requires
> &gt;= 3.0.0 .. &lt; 4.0.0. The language server will NOT activate.
> Install the latest ironls to continue.**

Click **Update Iron LSP** in the toast to open
<https://github.com/iron-lang/iron-lang/releases/latest>. Install the
updated binary, then reload the window (**Developer: Reload Window**).

This also triggers if the installed `ironls` is old enough that it does
not report a version via `--version`. Upgrade to the latest release and
the language client will activate normally.

### Diagnose report

Run **Iron LSP: Diagnose** from the Command Palette. The extension opens
an untitled document with the resolved `ironls` path, reported version,
detected workspace roots, LSP session status, and a log tail — attach the
report to any bug filed against `iron-lang`.

### Output channels

- **Iron Language Server** — editor-side structured log (always on).
- **Iron Language Server (trace)** — raw LSP traffic (lazily created when
  `iron.languageServer.trace.server` is not `"off"`).

## Contributing

This extension lives inside the main Iron compiler repository at
[iron-lang/iron-lang](https://github.com/iron-lang/iron-lang) under
`editors/vscode/`. File issues and PRs against the main repo. The grammar
is generated from `src/lexer/lexer.c`; see `grammars/textmate/README.md`
for the regeneration workflow.

## License

Apache-2.0 — see `LICENSE` in this directory.
