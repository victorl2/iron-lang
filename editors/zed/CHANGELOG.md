# Iron LSP — Zed extension changelog

All notable changes to the `iron-lsp` Zed extension are documented in
this file.

## 0.1.0 — initial release

- Native Zed extension (Rust → `wasm32-wasip2`) via
  `zed_extension_api@0.7.0`.
- Downloads a signed `ironls` binary from the matching
  `iron-lang/iron-lang` GitHub Release
  (`ironls-v<version>-<os>-<arch>.tar.gz` + `.sha256` sidecar).
- **Hand-rolled SHA-256 verification** via the `sha2` crate on raw
  bytes **before** extraction (T-06-05-01 mitigation).
  `zed_extension_api::download_file` has no built-in verify — see
  RESEARCH §"Code Pattern 4" and Zed issue
  `zed-industries/zed#16732` (closed not-planned).
- Cached binary in Zed `work_dir`; reused across sessions until the
  version range declared by the extension changes.
- User override via the `iron_lsp_path` extension setting (UI-SPEC
  S8 — points to a locally built `ironls` for dev workflows).
- 24h cache on `zed::latest_github_release(...)` result
  (T-06-05-03 rate-limit mitigation).
- TOCTOU handling (T-06-05-02): verify-before-extract; cached path
  stable across launches unless extension version changes.
- Path-traversal defense-in-depth (T-06-05-04): extracted files are
  checked to remain within `work_dir`; Zed's `DownloadedFileType::GzipTar`
  extractor is the primary mitigation.
- Structured JSON logging (UI-SPEC S5) with `src: 'zed-ext'` and
  download-flow events: `download.start`, `download.verified`,
  `download.mismatch`.
- Version-range compatibility warning (UI-SPEC S9) if the fetched
  `ironls` is outside the extension's declared range
  (`1.2.0..<1.3.0`). Phase 7 HARD-22 promotes this to a hard refuse.
- `sha2` / `hex` / `zed_extension_api` pinned in `Cargo.toml`;
  `Cargo.lock` committed (T-06-05-07 supply-chain mitigation).
- Native-side (non-sandbox) helper tests under
  `editors/zed/test/dev-load/` assert:
  - `sha256_verify_matches_mock_release` — digest round-trip on
    deterministic `mock_release` fixture
  - `sha256_verify_rejects_tampered_bytes` — T-06-05-01 invariant
    on single-byte flip
  - `sha256_output_is_lowercase_hex_64_chars` — UI-SPEC S8 display
    invariant
- CI `zed-package-validate` job: `cargo build --target wasm32-wasip2
  --release`, clippy (best-effort in v1), Zed CLI
  `zed extension validate` dry-run, uploads `.wasm` artifact.
- CI `zed-e2e` job (ubuntu + macos matrix; Linux
  `continue-on-error: true` per CONTEXT D-07): runs the native
  helper tests (`cargo test --features dev-extension-test`),
  verifies `wasm32-wasip2` build, installs Zed nightly, smoke-tests
  CLI presence. `GH_TOKEN` set (PITFALLS §8).
- Extended `.github/workflows/release.yml`: per-platform `ironls`
  tarball + `.sha256` asset emission on Linux-x86_64,
  macOS-x86_64, macOS-aarch64. New `release-wasm` job uploads
  `iron.wasm` (tree-sitter grammar, ABI 15).
- Minimum Zed: **0.200**.
- Linux support best-effort in v1 (CONTEXT D-07 documents Zed's
  maturing Linux support; macOS is the primary target).
- Publisher: `iron-lang/iron-lsp` (EXT-09; manual publish per
  CONTEXT D-11 — see
  `docs/dev/publisher-namespace-checklist.md`).
- Version-range compatibility: `ironls 1.2.x`.
