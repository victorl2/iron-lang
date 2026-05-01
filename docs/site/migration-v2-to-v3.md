# Migrating from Iron v2 to Iron v3

This document is the entry point for migrating an existing Iron v2 codebase to Iron v3 syntax. Comprehensive syntax migration reference (the v2→v3 grammar diff, semantic changes, breaking changes) is documented in the [Iron language docs](https://github.com/iron-lang/iron-lang) and is owned by the language team.

The notes below cover **only the LSP / editor-extension side** of the upgrade.

## LSP notes

### Editor extension version range

Starting with v3.0.0-alpha.1, the iron-lsp editor extensions for VSCode, Neovim, and Zed enforce a hard version-range gate:

```
ironls compatible range: ">= 3.0.0, < 4.0.0"
```

Older `ironls` binaries (< 3.0.0) are **structurally rejected** by the extension's hard-refuse machinery — the extension surfaces a clear diagnostic and refuses to attach. This prevents a v1 server from silently producing v3-incompatible diagnostics in your editor.

If you previously pinned the extension's compatible version range explicitly in your editor configuration (most users do not), update the pin to `">= 3.0.0, < 4.0.0"` to match the new release.

### `iron.migrate` LSP command

v3.0.0-alpha.1 ships an in-editor codemod command that runs `ironc migrate --from v2 --to v3` against your workspace and previews the diff before applying:

- **VSCode**: command palette → "Iron: Migrate v2 → v3"
- **Neovim**: `:IronLspMigrateV2ToV3`
- **Zed**: invoke `iron.migrate` via `workspace/executeCommand`

The command is gated on `ironc --version >= 3.0.0`. If you have an older `ironc` on your `PATH`, the LSP surfaces an error and does not run the codemod — update your toolchain first.

### Parity gate

`test_parity_ironc_lsp` and `test_parity_ironc_lsp_fmt` (the LSP-vs-CLI byte-for-byte parity gates from Phase 1 / Phase 7) continue to enforce zero divergence between `ironc check` and the LSP analyzer on the v3-migrated `tests/integration/` corpus. If you observe LSP diagnostics that disagree with `ironc check` output on a v3 file, please open an issue — that is a regression of the LSP's Core Value, not user error.
