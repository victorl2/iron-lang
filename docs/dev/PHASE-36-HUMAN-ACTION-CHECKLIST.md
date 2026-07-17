# Phase 36 — Human-Action Ship Checklist

> Single-file copy-paste sequence the maintainer follows to ship v4.0.0-alpha. Every step references its detailed playbook; this checklist orchestrates the ordering.

**Estimated total time:** 60–120 minutes (dominated by notarization wait and Marketplace indexing latency).
**Headline metric being shipped:** **227 / 255 PASS** on the `v4-acceptance` corpus.

---

## Step 0 — Confirm engineering is complete

```bash
cd /Users/victor/code/iron-lsp
git checkout main && git pull --ff-only

# All 5 Phase 36 plan SUMMARYs must exist:
ls .planning/phases/36-release-v4.0.0-alpha-docs/36-*-SUMMARY.md | wc -l
# Expected: 5

# Release-notes file must have the measured pass-rate (no FILL placeholder):
grep "FILL-IN-PLAN-04" docs/release/v4.0.0-alpha.md
# Expected: no output.

# All 5 playbooks present:
ls docs/dev/PHASE-36-{BRANCH-PROTECTION,TAG-PUSH,MACOS-NOTARIZE,VSCE-PUBLISH,ZED-PUBLISH}-PLAYBOOK.md
# Expected: 5 files listed.

# Both release scripts present + executable:
ls -l scripts/sign-and-notarize-macos.sh scripts/build-release-artifacts.sh
# Expected: -rwxr-xr-x permissions on both.
```

If any check fails, return to the corresponding Plan 36-NN before proceeding.

---

## Step 1 — Apply branch protection (REL-09)

Detailed playbook: [`docs/dev/PHASE-36-BRANCH-PROTECTION-PLAYBOOK.md`](PHASE-36-BRANCH-PROTECTION-PLAYBOOK.md)

Quick form:

```bash
gh auth status   # confirm administration:write scope on iron-lang/iron-lang
bash scripts/setup-branch-protection.sh

# Verify the 9 required checks are wired:
gh api repos/iron-lang/iron-lang/branches/main/protection | jq '.required_status_checks.contexts'
```

Expected: 9-element array including `v4-acceptance`, `build-and-test (ubuntu-latest)`, `build-and-test (macos-latest)`, `vscode-e2e`, `neovim-e2e`, `zed-package-validate`, `parity`, `tsan`, `slos`.

---

## Step 2 — Build release artifacts (REL-11 part A)

On a Mac with Apple Developer credentials in env:

```bash
export APPLE_DEV_ID_APP_IDENTITY="Developer ID Application: <YOUR NAME> (<TEAM_ID>)"
export APPLE_ID_USERNAME="<your apple id email>"
export APPLE_ID_APP_PASSWORD="<app-specific password>"
export APPLE_TEAM_ID="<10-char Team ID>"
export VSCE_PAT="<vsce personal access token>"

bash scripts/build-release-artifacts.sh
ls dist/
```

Expected: `ironls-v4.0.0-alpha-{linux-x86_64,macos-arm64,macos-x86_64}.tar.gz` + `.sha256` sidecars + `iron-lsp-4.0.0-alpha.vsix` + `iron.wasm` + `MANIFEST.txt`.

The build script invokes `scripts/sign-and-notarize-macos.sh` internally for the macOS binaries — if notarization fails see [`docs/dev/PHASE-36-MACOS-NOTARIZE-PLAYBOOK.md`](PHASE-36-MACOS-NOTARIZE-PLAYBOOK.md) §Failure modes.

For pure-Linux builds (iteration on a non-Mac host), use `bash scripts/build-release-artifacts.sh --linux-only`.

---

## Step 3 — Publish VSCode Marketplace (REL-11 part B)

Detailed playbook: [`docs/dev/PHASE-36-VSCE-PUBLISH-PLAYBOOK.md`](PHASE-36-VSCE-PUBLISH-PLAYBOOK.md)

Quick form:

```bash
cd editors/vscode
npx @vscode/vsce publish --pre-release --packagePath ../../dist/iron-lsp-4.0.0-alpha.vsix
cd ../..
```

Verify at https://marketplace.visualstudio.com/items?itemName=iron-lang.iron-lsp (allow ~5–15 minutes for Marketplace indexing).

---

## Step 4 — Submit Zed extensions PR (REL-11 part C)

Detailed playbook: [`docs/dev/PHASE-36-ZED-PUBLISH-PLAYBOOK.md`](PHASE-36-ZED-PUBLISH-PLAYBOOK.md)

This is the longest-tail step — the Zed registry reviewers control the merge timeline. Submit the PR now; subsequent steps don't depend on the PR landing.

The playbook covers `gh repo fork zed-industries/extensions` + submodule pinning + `extensions.toml` alphabetical insertion + PR body template (which references the GitHub Release URL — that URL exists only after Step 5; the playbook orders this internally).

---

## Step 5 — Cut tag, push, create GitHub Release (REL-10)

Detailed playbook: [`docs/dev/PHASE-36-TAG-PUSH-PLAYBOOK.md`](PHASE-36-TAG-PUSH-PLAYBOOK.md)

Quick form:

```bash
# Annotated, signed tag (tag body == release notes file)
git tag -a -s v4.0.0-alpha -F docs/release/v4.0.0-alpha.md

# Push the tag
git push origin v4.0.0-alpha

# Create the GitHub Release (pre-release flag because this is alpha)
gh release create v4.0.0-alpha \
  --title "Iron v4.0.0-alpha — Memory Model Overhaul" \
  --notes-file docs/release/v4.0.0-alpha.md \
  --prerelease

# Upload all artifacts
gh release upload v4.0.0-alpha \
  dist/ironls-*.tar.gz dist/ironls-*.tar.gz.sha256 \
  dist/iron-lsp-*.vsix \
  dist/iron.wasm dist/iron.wasm.sha256

# Verify
gh release view v4.0.0-alpha --json assets --jq '.assets[].name'
```

Expected asset list: 3 platform tarballs + 3 sha256 sidecars + 1 vsix + iron.wasm + iron.wasm.sha256 = ~8 files (one more if MANIFEST.txt is uploaded).

If GPG signing is unavailable, use `git tag -a v4.0.0-alpha -F docs/release/v4.0.0-alpha.md` (no `-s`); see the playbook §3b for the unsigned variant.

---

## Step 6 — Smoke test the published extension

```bash
# VSCode (--pre-release flag required because Marketplace listing is pre-release)
code --profile temp-test --install-extension iron-lang.iron-lsp@4.0.0-alpha --pre-release

# In the new VSCode profile, open any fixture from tests/integration/v4/ and confirm:
# - Syntax highlighting renders for v4 keywords (val, var, heap, rc, weak rc, defer, drop, etc.)
# - Hover on a binding shows the memory-model annotation block (regime + readonly status)
# - Completion of v4 keywords gates correctly by context
# - Saving the file produces no spurious diagnostics
```

Repeat for Zed once the registry PR merges (Step 4 longest-tail).

For Neovim: install per `editors/neovim/README.md`; no marketplace publish required (nvim-lspconfig upstream PR is a separate, post-alpha workflow).

---

## Step 7 — Announce (optional at alpha)

Per `docs/dev/release-runbook.md` §10: no announcement at alpha. The GitHub Releases page is the source of truth; users discover the release via the Releases tab and (for VSCode users) the Marketplace listing.

---

## Step 8 — Update STATE.md (post-ship)

Plan 36-05 already marks Phase 36 ENGINEERING COMPLETE and milestone v3.0 READY TO SHIP in `.planning/STATE.md`. After the maintainer ships, optionally append a one-line note recording the actual ship date — but this is optional; `.planning/STATE.md` is gitignored (per `CLAUDE.md` Constraints) and local-only.

---

## Failure-mode triage

Each playbook has its own `§Failure modes` section covering the common cases:
- Branch protection: `gh` auth scope, REST-API contexts-name mismatch.
- Tag-push: GPG signing unavailable, push rejected by branch protection (resolve by pushing tag from a branch that bypasses the gates, or temporarily lift protection).
- macOS notarize: `errSecInternalComponent`, notarytool `Invalid`, signature invalidated by re-signing, `spctl --assess` fails after staple.
- vsce publish: 401/403 PAT issues, publisher-not-found, package-size limits, indexing latency.
- Zed publish: CI schema validation, submodule convention drift, registry-index latency.

If a step fails and the playbook's failure modes don't cover it, the failure is novel — open an issue tagged `release-v4.0.0-alpha` and document it for future releases' playbooks.

---

## Rollback

Per `docs/dev/release-runbook.md`: **never rewrite a published tag.** If a critical bug surfaces post-tag:
- For an unpublished tag (push not yet executed): `git tag -d v4.0.0-alpha` locally and re-cut.
- For a published tag: cut `v4.0.1-alpha` with the fix; never `git push --force origin v4.0.0-alpha`.

---

*Companion documents: [`docs/dev/PHASE-36-CLOSEOUT.md`](PHASE-36-CLOSEOUT.md), [`docs/dev/MILESTONE-V3.0-CLOSEOUT.md`](MILESTONE-V3.0-CLOSEOUT.md), and the 5 referenced playbooks.*
