# Publisher namespace + release-pipeline checklist (EXT-05, EXT-09)

Manual steps the project owner executes **before first publish** to the
VSCode Marketplace and the Zed extensions registry. Per
CONTEXT.md D-11 (locked 2026-04-19), every registry publish is
**human-gated** — CI runs dry-runs only. This document is the contract
for what those manual steps look like.

Scope:
- **EXT-05** — VSCode Marketplace publisher `iron-lang` claim + publish
  pipeline
- **EXT-09** — Zed extensions registry publisher `iron-lang` claim +
  publish pipeline
- GitHub Release cadence for `ironls` binary + `iron.wasm` (consumed
  by the Zed extension SHA-256 download flow)
- Credential handling per CONTEXT.md D-11 (local `pass` only; no CI
  secrets; annual rotation)

Threats mitigated by this checklist: T-06-06-01 (PAT leakage via CI),
T-06-06-06 (registry rate-limit DoS), partial T-06-06-02 (unsigned
binary — full mitigation is Phase 7 HARD-21 code signing).

---

## VSCode Marketplace (EXT-05)

### One-time setup

1. Create (or reuse) an Azure DevOps organization:
   [https://dev.azure.com](https://dev.azure.com). Organization name:
   `iron-lang`. If `iron-lang` is taken, pick an alternate and document
   it here.
2. Create a Personal Access Token (PAT) scoped to **Marketplace: manage**:
   - User Settings → Personal Access Tokens → New Token
   - Organization: **All accessible organizations**
   - Scopes: custom defined → **Marketplace → Manage**
   - Expiration: 1 year
3. Enable 2FA on the Azure DevOps account before first publish.
4. Install `@vscode/vsce` 3.9+ locally:
   ```bash
   npm install -g @vscode/vsce
   vsce --version    # expect 3.9.x+
   ```
5. Log in to the publisher namespace:
   ```bash
   vsce login iron-lang    # paste PAT when prompted
   ```
6. Claim the publisher name `iron-lang` via the Azure Marketplace
   publisher management page:
   [https://marketplace.visualstudio.com/manage](https://marketplace.visualstudio.com/manage).
7. Store the PAT in the project owner's `pass` (password manager). Key
   name: `vscode-marketplace/iron-lang/pat`.
   **DO NOT** store the PAT in:
   - the repository (even in `.gitignore`d files)
   - GitHub secrets
   - any CI configuration
   - any shared team password manager

### First publish (post Plan 06-06 merge + phase close)

Prerequisites:
- The real `v1.2.0-alpha.6` GitHub Release has been cut (see GitHub
  Release section below)
- The release workflow has produced per-platform `ironls` tarballs +
  `.sha256` sidecars + `iron.wasm`
- `editors/vscode/` builds green locally

Commands:
```bash
cd editors/vscode
npm ci
npm run prepackage             # copies grammars/textmate/*.json in
npm run bundle                 # esbuild → dist/extension.js
npx vsce package --no-dependencies    # produces iron-lsp-0.1.0.vsix
```

Local smoke-test the `.vsix`:
```bash
code --install-extension iron-lsp-0.1.0.vsix
# open a .iron file, confirm extension activates + highlights work
code --uninstall-extension iron-lang.iron-lsp
```

Publish:
```bash
npx vsce publish --no-dependencies
# or, with explicit PAT override if vsce login was not used:
npx vsce publish --pat "$(pass show vscode-marketplace/iron-lang/pat)"
```

Verify the listing at:
[https://marketplace.visualstudio.com/items?itemName=iron-lang.iron-lsp](https://marketplace.visualstudio.com/items?itemName=iron-lang.iron-lsp)

### CI dry-run (per PR)

Every PR touching `editors/vscode/**` or `grammars/textmate/**` exercises
the `vscode-package-dryrun` job in `.github/workflows/ci.yml`:
- `npm ci && npm run prepackage && npm run bundle`
- `npx vsce package --no-dependencies --out iron-lsp.vsix`
- Asserts `.vsix` size < 5 MB (Marketplace rejects larger)
- Uploads the `.vsix` as a 7-day artifact for inspection

The dry-run **never** needs the PAT (packaging alone does not). Real
publish is manual only.

---

## Zed extensions registry (EXT-09)

### One-time setup

1. Create (or reuse) a `zed.dev` account with a project-owned email
   (the project owner's; never a shared inbox without 2FA).
2. Claim the publisher namespace `iron-lang` on `zed.dev`. This may
   require outreach to the Zed team depending on the registry's
   onboarding flow at the time — see Zed extension publishing docs
   at [https://zed.dev/docs/extensions](https://zed.dev/docs/extensions).
3. Enable 2FA on the `zed.dev` account.
4. Generate a publish token via the Zed account settings.
5. Store the token in `pass`. Key name: `zed-registry/iron-lang/pat`.
6. Install the Zed CLI locally:
   ```bash
   curl -fsSL https://zed.dev/install.sh | sh
   zed --version
   ```

### First publish

Prerequisites:
- The Zed extension builds green locally:
  ```bash
  cd editors/zed
  cargo build --target wasm32-wasip2 --release
  ```
- `editors/zed/Cargo.lock` is committed (supply-chain pin — T-06-05-07
  mitigation)
- `zed extension validate editors/zed/` succeeds locally
- The `v1.2.0-alpha.6` GitHub Release exists (the extension downloads
  `ironls` from it at runtime)

Commands:
```bash
cd editors/zed
zed extension publish .    # prompts for publish token if not cached
```

Verify the listing on the Zed extensions registry (browse or search
**Iron LSP** inside Zed).

### CI dry-run (per PR)

Every PR touching `editors/zed/**` exercises the `zed-package-validate`
job in `.github/workflows/ci.yml`:
- `rustup target add wasm32-wasip2`
- `cargo build --target wasm32-wasip2 --release`
- `cargo clippy -- -D warnings` (best-effort in v1; tightened in
  Phase 7)
- `zed extension validate editors/zed/` (if the Zed CLI on the runner
  supports the subcommand)
- Uploads the `.wasm` artifact as a 7-day artifact

The dry-run **never** needs the publish token. Real publish is manual
only.

---

## GitHub Release (EXT-08 dependency)

Per CONTEXT.md D-11: GitHub Release tags match `IRON_VERSION_FULL` in
`CMakeLists.txt`. The Zed extension download flow (`editors/zed/src/lib.rs`)
consumes assets from the matching Release via
`zed::latest_github_release("iron-lang/iron-lang", ...)`.

### First real release (post Plan 06-06 merge)

1. Bump `IRON_VERSION_FULL` in `CMakeLists.txt` to the current target
   (`1.2.0-alpha.6` at Phase 6 close).
2. Commit the version bump + tag + push:
   ```bash
   git commit -am "chore: bump IRON_VERSION_FULL to 1.2.0-alpha.6"
   git tag v1.2.0-alpha.6
   git push origin main v1.2.0-alpha.6
   ```
3. `release.yml` (extended in Plan 06-05 Task 3) runs automatically on
   tag push:
   - Matrix: `linux-x86_64`, `macos-x86_64`, `macos-arm64`
   - Each matrix leg builds `iron`, `ironc`, and `ironls`
   - Stages `ironls` → tars → produces
     `ironls-v1.2.0-alpha.6-<os>-<arch>.tar.gz` + `.sha256` sidecar
   - The `release-wasm` job builds `iron.wasm` via
     `tree-sitter generate && tree-sitter build --wasm` and uploads it
   - All assets attach to the published Release
4. Verify the Release asset list at:
   `https://github.com/iron-lang/iron-lang/releases/tag/v1.2.0-alpha.6`
5. Confirm the Zed extension can fetch the binary (manual smoke test):
   ```bash
   cd editors/zed
   cargo build --target wasm32-wasip2 --release
   zed --dev-extension .
   # open a .iron file; confirm download + verify + extract succeeds
   ```
6. Announce the release on project channels.

### Release-cut checklist (short form)

Run this at the close of every Phase that produces a release:

- [ ] `IRON_VERSION_FULL` bumped in `CMakeLists.txt`
- [ ] Version bump committed
- [ ] Tag pushed (`git tag vX.Y.Z-suffix && git push origin vX.Y.Z-suffix`)
- [ ] `release.yml` succeeded on all matrix legs
- [ ] Per-platform `.tar.gz` + `.sha256` assets present on the Release
- [ ] `iron.wasm` asset present on the Release
- [ ] Zed extension dev-load smoke test passes against the new Release
- [ ] VSCode extension `npm run package` produces a green `.vsix` (no
      publish yet)
- [ ] Announcement drafted

---

## Credential rotation (annual)

At least once a year (or immediately on suspected compromise):

1. **VSCode PAT:**
   - Azure DevOps → User Settings → Personal Access Tokens → regenerate
   - `pass insert vscode-marketplace/iron-lang/pat` (overwrite)
   - Smoke test: `vsce login iron-lang` with the new PAT
2. **Zed publish token:**
   - zed.dev account settings → regenerate token
   - `pass insert zed-registry/iron-lang/pat` (overwrite)
3. Announce rotation in the project changelog. **Never** post key
   fingerprints or partial tokens publicly.

Timing convention: rotate on the anniversary of the first publish
(use `pass` timestamps / `git` annotated tags on a dated rotation
note).

---

## Incident response

If publisher-namespace compromise is suspected (unauthorized publish,
unexpected version bump, credential leak into a public artifact):

1. **Immediately** revoke the compromised PAT/token on the respective
   registry (Azure DevOps PAT revocation, zed.dev token revocation).
2. Regenerate a fresh token; update `pass`.
3. Audit the last-known-good published versions:
   - VSCode Marketplace: the extension page shows all versions
   - Zed registry: same
4. If a malicious version shipped: unpublish (both registries support
   unpublishing individual versions); publish a new clean version with
   a bumped suffix (e.g. `0.1.0-hotfix.1`).
5. File a post-mortem at `docs/dev/incidents/YYYY-MM-DD-publisher-compromise.md`
   documenting:
   - Timeline (when compromise happened, when detected, when revoked)
   - Attack vector (how the credential leaked)
   - Remediation (rotation, unpublish, any code changes)
   - Prevention (what process changes prevent recurrence)

---

## Not in scope for this checklist

These are tracked elsewhere and explicitly deferred:

- **Code-signing (macOS Developer ID)** — Phase 7 HARD-21.
  Phase 6 v1 ships unsigned binaries; macOS Gatekeeper quarantine is
  documented in `editors/zed/README.md` as a known limitation.
- **Notarization (macOS)** — Phase 7 HARD-21.
- **Authenticode signing (Windows)** — v2 (follows the compiler's
  Windows posture; Windows is currently excluded from the CI matrix).
- **Windows Marketplace listing** — v2.
- **Crash telemetry** — Phase 7 HARD hardening.
- **`ironLspCompatibleIronlsRange` hard-refuse gate** — Phase 7
  HARD-22. Today it's a warning only.
- **Auto-publish on tag push** — rejected (CONTEXT D-11). A broken
  `.vsix` pushed to Marketplace is harder to unroll than a manual
  gate catching it.

---

## Cross-references

- CONTEXT.md D-11 — publisher-namespace + release-pipeline decision
  (`.planning/phases/06-m5-grammars-editor-extensions/06-CONTEXT.md`)
- Phase 6 Plan 06-06 PLAN.md — this checklist is the EXT-05/EXT-09
  deliverable
- `.github/workflows/release.yml` — release automation (per-platform
  `ironls` + `iron.wasm`)
- `.github/workflows/ci.yml` — CI dry-run jobs
  (`vscode-package-dryrun`, `zed-package-validate`)
- `editors/zed/src/lib.rs` — SHA-256 verification flow consuming the
  GitHub Release (`ironls-v<version>-<os>-<arch>.tar.gz` + `.sha256`)
- `editors/vscode/package.json` — `ironLspCompatibleIronlsRange` field
  (UI-SPEC S9 version mismatch warning)
