# Iron LSP — Zed extension

Language support for the [Iron programming language](https://github.com/iron-lang/iron-lang)
in the [Zed editor](https://zed.dev/). Ships with:

- syntax highlighting (via the in-tree tree-sitter-iron parser, distributed
  as `iron.wasm` alongside each ironls release),
- LSP integration (diagnostics, hover, go-to-definition, completion,
  rename, formatting — every capability ironls itself implements),
- automatic `ironls` binary download from GitHub Releases with
  SHA-256 verification.

**Tracks:** Iron v3.0.0-alpha.1 (current main-branch alpha). See the
**Version compatibility** section below for the exact `ironls` range the
extension accepts (the hard-refuse constant bumps atomically in a later
phase).

## Iron syntax overview

Iron v3 introduces first-class surface features surfaced by `ironls`
completion, hover, and diagnostics — all rendered by Zed's standard LSP
client UI:

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

Legacy pre-v3 keywords (`val`, `var`, `object`, `interface`, `impl`, etc.)
continue to work unchanged. The complete roster is 44 keywords,
drift-guarded at build time by `test_grammar_keyword_drift_tree_sitter`.

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

- **Zed 0.200+** (the `zed_extension_api` 0.7 surface + `wasm32-wasip2`
  target). Older Zed versions use a different extension API and will not
  load this extension.
- **macOS** (Apple silicon or Intel) or **Linux** (x86_64). Windows is
  not supported in v1.
- Internet access on first activation (to download the `ironls` binary).
  Users who have already built `ironls` locally can set `iron_lsp_path`
  to bypass the download entirely.

## Install

Once published to the [Zed extensions registry](https://zed.dev/extensions)
search for "Iron LSP" under **Zed → Extensions**. Until then, install in
dev-extension mode:

```sh
git clone https://github.com/iron-lang/iron-lang.git
cd iron-lang/editors/zed
cargo build --target wasm32-wasip2 --release
# Then in Zed: Extensions → Install Dev Extension → select this directory.
```

## Configure

Both settings live under the Zed `lsp.iron-lsp` key:

```json
{
  "lsp": {
    "iron-lsp": {
      "settings": {
        "iron_lsp_path": "/absolute/path/to/ironls",
        "iron_lsp_log_level": "info"
      }
    }
  }
}
```

### `iron_lsp_path` (default: empty)

Absolute path to a local `ironls` binary. When set and the file is
executable, the extension uses it directly and skips the GitHub download
+ SHA-256 verification flow. Useful when you are developing iron-lang
itself (local `build/ironls` is always newer than the last release) or
when corporate network policy blocks `github.com`.

### `iron_lsp_log_level` (default: `"info"`)

Controls the verbosity of the `src: "zed-ext"` log lines the extension
emits to the Zed developer console (`View → Debug` in Zed). Valid
values: `error`, `warn`, `info`, `debug`.

## How download + verification works

On first activation (and after an `ironls` version bump), the extension
runs the following flow — verbatim per CONTEXT D-06 + RESEARCH §Pattern 4:

1. **User override.** If `iron_lsp_path` is set and points to an
   existing file, use it and stop.
2. **Cached binary.** If a previous activation already downloaded and
   verified an `ironls` binary and the cached copy still exists, use it.
3. **Fresh download.** Otherwise:
   - `zed::current_platform()` → `(os, arch)` tuple.
   - `zed::latest_github_release("iron-lang/iron-lang", { require_assets: true, pre_release: false })`
     fetches the newest non-prerelease tag with assets attached.
   - Locate two assets: `ironls-{version}-{os}-{arch}.tar.gz` and its
     `.sha256` sidecar.
   - Download the `.sha256` sidecar first (fail fast if the release is
     missing it).
   - Download the tarball as `DownloadedFileType::Uncompressed` so we
     can hash the raw bytes on disk.
   - **SHA-256 verify** — `sha2::Sha256::digest` over the raw bytes,
     hex-encoded, compared against the sidecar's contents. On mismatch,
     the tarball is deleted and the extension aborts with a toast
     directing you to `iron_lsp_path`.
   - On match, re-invoke `zed::download_file` with
     `DownloadedFileType::GzipTar` to extract the archive, then
     `zed::make_file_executable` on the extracted `ironls` binary.

The hand-rolled SHA-256 step is required because
`zed_extension_api::download_file` does **not** provide built-in hash
verification (see issue [zed-industries/zed#16732](https://github.com/zed-industries/zed/issues/16732),
closed "not planned" — Zed's maintainers have said extensions should
do this themselves). Shipping an unverified download would allow a
MITM or a compromised GitHub Release asset to run arbitrary code as
your user account; the extension treats this as non-negotiable.

## Troubleshooting

### "Iron LSP: ironls download verification failed…"

The SHA-256 of the downloaded tarball did not match the published
`.sha256` sidecar. Next activation will re-download. Causes, in order
of likelihood:

- The download was truncated or corrupted in transit (try again).
- GitHub's asset storage is in a partial state (wait a few minutes).
- The release was re-cut and the tarball + sidecar are out of sync
  — report it as a repo issue.
- Extremely rare but serious: the binary was tampered with. Do **not**
  bypass by setting `iron_lsp_path` until you have verified locally.

If it keeps failing, `iron_lsp_path` lets you build and use `ironls`
yourself.

### "Iron LSP: could not download ironls from GitHub…"

Usually network-related:

- Corporate proxy blocking `github.com`.
- GitHub API rate-limit exhaustion (set `GH_TOKEN` in your
  shell environment, or use `iron_lsp_path` to bypass).
- No ironls release exists for your platform (check the
  [releases page](https://github.com/iron-lang/iron-lang/releases)).

### macOS Gatekeeper

`ironls` binaries attached to GitHub Releases are Developer-ID signed,
Apple-notarized, and stapled per Phase 7 HARD-21 (see
`scripts/ci/sign_and_notarize_macos.sh` +
`docs/dev/apple-notarization-setup.md`). Gatekeeper accepts them
silently on first launch — no `xattr -dr com.apple.quarantine` dance
required. If you built `ironls` locally and pointed `iron_lsp_path` at
it, that local binary is *not* signed and Gatekeeper may quarantine
it; in that case either use the release binary via the download flow
or sign your local copy yourself.

### Zed on Linux

Zed's Linux support is still maturing (CONTEXT D-07). The extension
itself works; some UI surfaces (toast rendering, status bar) may
behave slightly differently across Zed nightly builds on Linux. The
CI harness allows Linux to fail on this extension for v1.

## Version compatibility

This extension targets `ironls` in the range `>= 3.0.0, < 4.0.0` per
the `[version_constraints] ironls` entry in `extension.toml`. Phase 7
HARD-22 / D-10 enforces this with a **hard refuse**: on every
activation the extension runs `ironls --version`, parses the semver
token, and aborts `language_server_command` if it falls outside the
range.

### Troubleshooting: "Iron LSP: detected ironls X.Y.Z, but this extension requires …"

The hard-refuse error surfaces in Zed's notifications and developer
console. To resolve:

1. Install the latest release from
   <https://github.com/iron-lang/iron-lang/releases/latest>.
2. Set `iron_lsp_path` in your Zed settings to the upgraded binary
   (or clear it so the extension re-downloads on next activation).
3. Reload the workspace (`Developer: Reload Extensions`).

If you need to pin to a specific release, build the matching
extension version from `editors/zed/` and install via
`Extensions → Install Dev Extension`.

## Development

```sh
cd editors/zed
cargo build --target wasm32-wasip2 --release
cargo test --features dev-extension-test       # native SHA-256 helper tests
zed --dev-extension editors/zed                 # load into a running Zed
```

The build step requires the `wasm32-wasip2` target:

```sh
rustup target add wasm32-wasip2
```

## License

Apache-2.0, matching the iron-lang repo root.
