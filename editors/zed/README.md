# Iron LSP: Zed extension

Language support for the [Iron programming language](https://github.com/victorl2/iron-lang)
in the [Zed editor](https://zed.dev/):

- syntax highlighting, bracket matching, indentation, the outline panel and
  vim-mode text objects, from the in-tree tree-sitter grammar
  (`grammars/tree-sitter/iron`, pinned in `extension.toml`);
- the `ironls` language server: diagnostics while typing, hover,
  completion, signature help, go to definition, references, rename,
  semantic tokens, inlay hints, formatting, code actions, folding and
  document symbols;
- `ironls` from your settings, your PATH, or downloaded from the latest
  GitHub release and checked against its SHA-256 sidecar.

## Requirements

- Zed with extension API 0.7 support (`zed_extension_api = "=0.7.0"`).
- macOS (Apple silicon or Intel), Linux x86_64 or Windows x86_64 for the
  downloaded `ironls`; any platform when `ironls` is on PATH or configured.

## Install

Until the extension is in the Zed registry, install it as a dev extension
(Zed needs `rustup` with the `wasm32-wasip2` target for this):

```sh
rustup target add wasm32-wasip2
```

Then in Zed: **Extensions** > **Install Dev Extension** and pick
`editors/zed`. Zed builds the extension and the grammar itself.

## Configure

All settings live under `lsp.iron-lsp` in Zed's `settings.json`:

```json
{
  "lsp": {
    "iron-lsp": {
      "binary": { "path": "/absolute/path/to/ironls" },
      "settings": {
        "inlayHints": { "parameterNames": true, "bindingTypes": true }
      }
    }
  },
  "languages": {
    "Iron": { "inlay_hints": { "enabled": true } }
  }
}
```

- `binary.path` (also `binary.arguments`, `binary.env`): the `ironls` to
  run. Without it the extension uses `ironls` from your PATH, and only
  downloads one when neither is set.
- `settings.inlayHints.parameterNames` (`area(width: 2.0, height: 3.0)`)
  and `settings.inlayHints.bindingTypes` (`val total: Float`): the same
  keys as the VS Code extension's `iron.inlayHints.*`. You may also nest
  them under `"iron"`. Changes apply without a restart.
- Zed shows inlay hints only when `inlay_hints.enabled` is on (globally
  or for `Iron`, as above).

## How the download works

When no `ironls` is configured or on PATH, the extension:

1. asks GitHub for the latest release of `victorl2/iron-lang`;
2. downloads the `.sha256` sidecar of
   `ironls-<tag>-<os>-<arch>.tar.gz` (`.zip` on Windows), then the
   archive itself, uncompressed;
3. hashes the archive with SHA-256 and compares it with the sidecar,
   deleting it on a mismatch (`zed_extension_api::download_file` has no
   built-in verification, see zed-industries/zed#16732);
4. only then extracts it and makes `ironls` executable.

## Version compatibility

The extension works with `ironls` `>= 4.0.0, < 5.0.0`
(`[version_constraints]` in `extension.toml`). It runs `ironls --version`
before starting the server and refuses a version outside that range with a
message naming the binary. If Zed does not allow the extension to run
processes, the check is skipped.

## Development

```sh
cd editors/zed
cargo build --target wasm32-wasip2 --release
cargo test --features dev-extension-test
```

The native tests check the manifest, the language config, the grammar pin
(the pinned commit must contain the generated parser), the query files and
the settings sent to `ironls`. `ctest -R test_tree_sitter_queries` compiles
every query file against the grammar.

`languages/iron/highlights.scm` is generated: edit
`grammars/tree-sitter/iron/queries/highlights.scm` and run
`scripts/sync-editor-queries.sh`. When `grammar.js` changes, regenerate
`grammars/tree-sitter/iron/src/` and bump `[grammars.iron] commit` to a
commit that contains it.

## License

Apache-2.0, matching the iron-lang repo root.
