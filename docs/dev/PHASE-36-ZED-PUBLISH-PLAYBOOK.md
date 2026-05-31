# Phase 36 — Zed Extensions Registry Publish Playbook

> Phase 36 Plan 04 (REL-11). Human-gated playbook for publishing
> `iron-lsp` v0.5.0 (v4-compatible) to the Zed extensions registry. The
> Zed registry is a PR-based system against
> `github.com/zed-industries/extensions`; the agent prepared
> `editors/zed/extension.toml` (version `0.5.0`) and this playbook
> produces the PR body. The maintainer opens the PR.

## 0. Prerequisites

- `gh` CLI authenticated with a personal account that can fork
  `zed-industries/extensions`:
  ```bash
  gh auth status
  ```
- The Iron LSP `v4.0.0-alpha.1` release tag is already cut and pushed
  per `docs/dev/PHASE-36-TAG-PUSH-PLAYBOOK.md` — the Zed registry entry
  pins the extension to a specific commit SHA on `main`, so the tagged
  commit must already exist on the remote.
- Rust toolchain installed (only needed for the optional local schema
  validation in §5):
  ```bash
  rustup --version
  cargo --version
  ```

## 1. Capture the iron-lang commit SHA

From a clean checkout of `iron-lang/iron-lang` at the tagged commit:

```bash
cd /Users/victor/code/iron-lsp
git fetch --tags origin
git checkout v4.0.0-alpha.1
SHA=$(git rev-parse HEAD)
echo "Iron LSP v4.0.0-alpha.1 SHA: ${SHA}"
# Save this SHA — it goes into the registry entry below.
```

## 2. Fork zed-industries/extensions

```bash
gh repo fork zed-industries/extensions --clone --remote
cd extensions
```

## 3. Add the iron-lsp entry

> **Re-check the current convention before executing.** The Zed
> registry submission flow has changed at least twice since the
> registry opened. The canonical reference is:
> `https://github.com/zed-industries/extensions/blob/main/CONTRIBUTING.md`
> Reading that file first is mandatory; the steps below describe the
> 2025–2026 submodule-per-extension convention but the PR reviewer will
> catch any drift if it has changed.

Edit `extensions.toml` and add the entry in alphabetical order
(`iron-lsp` belongs in the `i*` block). Append:

```toml
[iron-lsp]
submodule = "extensions/iron-lsp"
version = "0.5.0"
```

Add the iron-lang repo as a submodule pinned to the tagged commit:

```bash
git submodule add -b main https://github.com/iron-lang/iron-lang.git extensions/iron-lsp-tmp
cd extensions/iron-lsp-tmp
git checkout "${SHA}"
cd ../..

# Zed extensions live as single-dir submodules. Move only the editors/zed subdir.
mv extensions/iron-lsp-tmp/editors/zed extensions/iron-lsp
rm -rf extensions/iron-lsp-tmp
```

Verify the resulting structure:

```bash
ls extensions/iron-lsp/
# Expected: extension.toml, src/lib.rs, languages/, Cargo.toml, ...
cat extensions/iron-lsp/extension.toml | head -5
# Expected: id = "iron-lsp" ... version = "0.5.0"
```

## 4. Open the PR

```bash
git checkout -b add-iron-lsp-v0.5.0
git add extensions.toml extensions/iron-lsp .gitmodules
git commit -m "Add iron-lsp v0.5.0 (Iron v4 support)"
git push -u origin add-iron-lsp-v0.5.0
gh pr create \
  --repo zed-industries/extensions \
  --base main \
  --head "$(gh api user -q .login):add-iron-lsp-v0.5.0" \
  --title "Add iron-lsp v0.5.0 (Iron v4 support)" \
  --body "$(cat <<'EOF'
Adds the `iron-lsp` extension at v0.5.0, which provides language support
for the Iron programming language v4.

- **Iron LSP server compatibility:** `>= 4.0.0, < 5.0.0` (enforced by
  the extension; older `ironls` is structurally refused at extension
  start per Phase 7 HARD-22 / D-10).
- **Iron language release:** v4.0.0-alpha.1 ("Memory Model Overhaul") —
  see https://github.com/iron-lang/iron-lang/releases/tag/v4.0.0-alpha.1.

## What this extension does

- Downloads the platform-appropriate `ironls` binary from the iron-lang
  GitHub Release, verifies SHA-256, and spawns it as the language
  server (`src/lib.rs` `language_server_command`).
- Locks the download host to `github.com` (capabilities table in
  `extension.toml`).
- Settings exposed: `iron_lsp_path` (local override that bypasses the
  download path), `iron_lsp_log_level` (`error`/`warn`/`info`/`debug`).

## Why a new version

Iron v4 is a memory-model overhaul (`val`/`var`, `heap`/`rc`/`weak rc`,
`*T`/`*var T`, `readonly`, `drop`/`copy`/`nocopy`, `defer`, arena). The
extension's `compatible_ironls` range was bumped to
`>= 4.0.0, < 5.0.0` to match, and the extension version was bumped to
`0.5.0` (minor — still pre-1.0 for the extension itself, even though
the underlying LSP server is in v4 alpha).

## Verification

- Iron LSP v4.0.0-alpha.1 tag:
  https://github.com/iron-lang/iron-lang/releases/tag/v4.0.0-alpha.1
- Extension manifest:
  https://github.com/iron-lang/iron-lang/blob/v4.0.0-alpha.1/editors/zed/extension.toml
- Local schema validation (run from the PR branch):
  `cargo run --bin extension-cli -- validate extensions/iron-lsp`
EOF
)"
```

## 5. Optional local schema validation

If the registry's `extension-cli` is checked into the repo, validate
locally before pushing:

```bash
cargo run --bin extension-cli -- validate extensions/iron-lsp
# Expected: "Extension 'iron-lsp' validated successfully."
```

If the binary is not present, skip — the PR CI runs it server-side.

## 6. Monitor the PR

Zed maintainers review and merge. Typical turnaround: 1–7 days. Address
review comments by force-pushing to the `add-iron-lsp-v0.5.0` branch.
Once merged, the v0.5.0 listing becomes visible in the Zed extensions
list within ~1 hour.

## 7. Failure modes

### 7.1 CI fails on schema validation

`extension.toml` has a malformed field. Inspect the Zed schema in
`zed-industries/extensions` (typically under `crates/extension-cli` or
similar) and align field names + types.

### 7.2 Reviewer asks for a Linux-targeted smoke test

Provide the output of running `zed --foreground` with the extension
installed against a sample `.iron` file, captured to a gist or text
file linked in the PR comment.

### 7.3 Submodule convention has changed

See §3 NOTE — re-read
`zed-industries/extensions/blob/main/CONTRIBUTING.md` and follow the
current convention. If the registry has moved away from
git-submodules to e.g. published Rust crates, adapt accordingly.

### 7.4 PR open but the registry shows the OLD version

Either (a) the PR has not yet been merged, or (b) the registry's
extension-index rebuild is queued. Wait 1 hour after merge before
escalating in the Zed community channels.

## 8. Cross-references

- `docs/dev/PHASE-36-VSCE-PUBLISH-PLAYBOOK.md` — VSCode Marketplace
  side of the same release.
- `docs/dev/PHASE-36-TAG-PUSH-PLAYBOOK.md` — tag-cut + push that this
  playbook depends on (must run first).
- `docs/dev/publisher-namespace-checklist.md` — registry-credential
  prerequisites (one-time).
