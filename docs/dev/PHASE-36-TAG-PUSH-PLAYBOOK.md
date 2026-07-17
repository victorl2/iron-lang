# Phase 36 — v4.0.0-alpha Tag-Cut Playbook

> Phase 36 REL-10. Human-gated playbook for cutting the `v4.0.0-alpha` tag, pushing it to `origin`, and creating the GitHub Release with the release-notes file. The agent prepared `docs/release/v4.0.0-alpha.md`; this file is what the maintainer runs from an authenticated terminal.

## 0. Prerequisites

- Local clone of `main` with push access to `origin` (do **not** push tags from a fork).
- `git --version` >= 2.30.
- `gh` CLI installed and authenticated (`gh auth status`).
- Optionally: GPG key configured for signed tags (`git config --get user.signingkey`). If unset, the playbook falls back to an annotated (unsigned) tag.
- All required checks GREEN on `main` per `docs/dev/PHASE-36-BRANCH-PROTECTION-PLAYBOOK.md` (run Plan 36-01's playbook first if not yet applied).
- Plan 36-04 has filled in the silvaserver-measured pass-rate in `docs/release/v4.0.0-alpha.md` (the `FILL-IN-PLAN-04-FROM-SILVASERVER` placeholder must be replaced — `grep "FILL-IN-PLAN-04" docs/release/v4.0.0-alpha.md` returns empty).

## 1. Verify version stamps cohere

```bash
grep '^set(IRON_VERSION_FULL' CMakeLists.txt
# Expected: set(IRON_VERSION_FULL "4.0.0-alpha")

grep '"version"' editors/vscode/package.json | head -1
# Expected: "version": "4.0.0-alpha",

grep '^version' editors/zed/extension.toml
# Expected: version = "4.0.0-alpha"  (or the value bumped by Plan 36-04)

grep 'compatible_ironls' editors/zed/extension.toml editors/vscode/package.json
# Expected: both pin ">= 4.0.0, < 5.0.0"
```

If any output is wrong, STOP and run Plan 36-04 packaging tasks before continuing.

Build remotely + verify `--version` on the silvaserver podman:

```bash
rsync -az --delete \
  --exclude='build*/' --exclude='.planning/' --exclude='.git/' \
  --exclude='.claude/' --exclude='node_modules/' \
  /Users/victor/code/iron-lsp/ \
  192.168.0.100:/home/victor/iron-lsp-remote/

ssh 192.168.0.100 'cd /home/victor/iron-lsp-remote && \
  podman run --rm --memory=8g --memory-swap=8g \
    -v $(pwd):/work -w /work iron-lsp-build:latest bash -c "\
      cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DIRON_BUILD_LSP=ON && \
      cmake --build build -j4 && \
      ./build/iron --version && \
      ./build/ironc --version && \
      ./build/ironls --version"'
```

All three must print: `4.0.0-alpha`.

## 2. Verify main is clean and at the release SHA

```bash
git checkout main
git pull --ff-only
git status
# Expected: working tree clean.

git log -1 --pretty=format:'%H %s'
# Note the SHA — this is what the tag will point to.
```

## 3. Cut the tag

The tag annotation body is the release-notes file itself (`-F` reads the file into the tag message).

### 3a. Signed (preferred if GPG configured)

```bash
git tag -a -s v4.0.0-alpha -F docs/release/v4.0.0-alpha.md
```

### 3b. Annotated unsigned (fallback)

```bash
git tag -a v4.0.0-alpha -F docs/release/v4.0.0-alpha.md
```

Verify locally:

```bash
git tag --list v4.0.0-alpha
git show v4.0.0-alpha --no-patch
# Expected: tag message is the full release-notes file.
```

## 4. Push the tag

```bash
git push origin v4.0.0-alpha
```

**Do not** force-push tags. Once published, a tag is immutable.

The push triggers `.github/workflows/release.yml`. Monitor under **Actions → Release** for the `v4.0.0-alpha` run.

## 5. Create the GitHub Release

If `release.yml` does not auto-create the Release object (check `.github/workflows/release.yml` for an `actions/create-release` or `softprops/action-gh-release` step), create it manually:

```bash
gh release create v4.0.0-alpha \
  --title "Iron v4.0.0-alpha — Memory Model Overhaul" \
  --notes-file docs/release/v4.0.0-alpha.md \
  --prerelease
```

`--prerelease` is correct because this is an alpha. Drop the flag when v4.0.0 GA is cut.

Verify the Release page exists:

```bash
gh release view v4.0.0-alpha
```

## 6. Upload artifacts (after Plan 36-04 packaging)

Plan 36-04 produces the binary tarballs + the `.vsix` in `dist/`. Upload them:

```bash
gh release upload v4.0.0-alpha \
  dist/ironls-v4.0.0-alpha-linux-x86_64.tar.gz \
  dist/ironls-v4.0.0-alpha-linux-x86_64.tar.gz.sha256 \
  dist/ironls-v4.0.0-alpha-macos-arm64.tar.gz \
  dist/ironls-v4.0.0-alpha-macos-arm64.tar.gz.sha256 \
  dist/ironls-v4.0.0-alpha-macos-x86_64.tar.gz \
  dist/ironls-v4.0.0-alpha-macos-x86_64.tar.gz.sha256 \
  dist/iron-lsp-4.0.0-alpha.vsix \
  dist/iron.wasm
```

The exact tarball naming follows the `docs/dev/release-runbook.md` convention. If Plan 36-04 produces a different name, update this section before running.

## 7. Verify the published release

```bash
gh release view v4.0.0-alpha --json assets --jq '.assets[].name'
```

Expected: one line per uploaded asset matching the upload list above.

## 8. Failure modes

- **`git tag` fails with `gpg: signing failed`** — GPG agent timed out or pinentry can't reach the TTY. Re-run `gpg --sign --armor /dev/null` to prime the agent, then retry. Or fall back to the §3b annotated-unsigned variant.
- **`git push origin v4.0.0-alpha` fails with `tag already exists`** — the tag was pushed previously. Delete locally and remotely (DESTRUCTIVE — only do this if the prior push was visibly broken AND no downstream user could have fetched it):

  ```bash
  git tag -d v4.0.0-alpha
  git push origin :refs/tags/v4.0.0-alpha
  ```

  Then redo from §3. **If anyone could have pulled the broken tag, cut `v4.0.1-alpha` instead.** Tags are immutable in practice.
- **`release.yml` fails on macOS notarization** — see Plan 36-04's `docs/dev/PHASE-36-MACOS-NOTARIZE-PLAYBOOK.md` for the manual recovery procedure.
- **`gh release create` fails with `422 tag not found`** — the tag push hasn't propagated to the API yet. Wait 30 s and retry.

## 9. Post-cut

- Update `.planning/STATE.md` (Plan 36-05 closeout handles this).
- Announce per `docs/dev/release-runbook.md` §10 (none at alpha; HN / Mastodon at GA).

## 10. Rollback

Per `docs/dev/release-runbook.md`: **never** rewrite a published tag. If a critical bug surfaces post-tag, cut `v4.0.1-alpha` with the fix. Extensions pinned to `>= 4.0.0, < 5.0.0` accept the patch without user action.

---

*Related: `docs/dev/PHASE-36-BRANCH-PROTECTION-PLAYBOOK.md` (REL-09), `docs/dev/PHASE-36-MACOS-NOTARIZE-PLAYBOOK.md` (REL-11, produced by Plan 36-04), `docs/dev/release-runbook.md` (legacy v1 procedure).*
