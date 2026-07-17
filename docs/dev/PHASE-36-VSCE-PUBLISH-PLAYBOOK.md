# Phase 36 — VSCode Marketplace Publish Playbook

> Phase 36 Plan 04 (REL-11). Human-gated playbook for publishing the
> `iron-lsp` VSCode extension v4.0.0-alpha to the VSCode Marketplace.
> The agent prepared `editors/vscode/package.json` (version
> `4.0.0-alpha`) and `editors/vscode/CHANGELOG.md` (the new entry).
> This file is what the maintainer runs from an authenticated terminal.

## 0. Prerequisites

- Node.js >= 18:
  ```bash
  node --version    # e.g. v20.18.0
  ```
- `vsce` CLI available (either globally or via `npx`):
  ```bash
  npm install -g @vscode/vsce         # global option
  # OR
  npx @vscode/vsce --version          # one-shot option (no install)
  ```
- VSCode Marketplace publisher PAT (Personal Access Token):
  - Issued at `https://dev.azure.com/<your-azure-org>/_usersSettings/tokens`.
  - Scope: `Marketplace > Manage`.
  - Exported into the publish shell:
    ```bash
    export VSCE_PAT="<pat>"
    ```
- Publisher namespace `iron-lang` claimed per
  `docs/dev/publisher-namespace-checklist.md`.

## 1. Package locally (smoke test)

```bash
cd editors/vscode
npm install
npm run prepackage        # copies grammar from grammars/textmate -> syntaxes/, builds dist/extension.js
npx @vscode/vsce package --pre-release --no-dependencies
```

Output: `iron-lsp-4.0.0-alpha.vsix` in `editors/vscode/`. Verify the
contents:

```bash
npx @vscode/vsce ls iron-lsp-4.0.0-alpha.vsix | head -30
```

The list must include:
- `dist/extension.js`
- `syntaxes/iron.tmLanguage.json`
- `language-configuration.json`
- `snippets/iron.code-snippets`
- `package.json`
- `README.md`
- `LICENSE`
- `icon.png`
- `CHANGELOG.md`

## 2. Sanity-check the manifest

```bash
npx @vscode/vsce show iron-lang.iron-lsp
```

Expected: the current latest version on the Marketplace is the prior
release; the listing metadata (publisher, categories, repo URL, icon)
matches what `editors/vscode/package.json` describes.

## 3. Publish

```bash
npx @vscode/vsce publish --pre-release \
  --packagePath iron-lsp-4.0.0-alpha.vsix
```

The `--pre-release` flag is correct (this is an alpha). The Marketplace
renders a "Pre-release" badge on the listing and only users who have
opted into pre-releases will see this as the install target.

Expected output:

```
Publishing 'iron-lang.iron-lsp v4.0.0-alpha'...
Published iron-lang.iron-lsp v4.0.0-alpha.
```

## 4. Verify

Visit:

```
https://marketplace.visualstudio.com/items?itemName=iron-lang.iron-lsp
```

The version should display `4.0.0-alpha` with the **Pre-release**
badge. The CHANGELOG section should render with the new entry. The
sidebar should show today's date as "Last updated".

Install in a fresh VSCode profile and confirm the extension activates
on a `.iron` file:

```bash
code --profile temp-test \
  --install-extension iron-lang.iron-lsp@4.0.0-alpha \
  --pre-release
```

Open a `.iron` file; the status bar should show
`Iron Language Server: Running` within ~2 s. The Output panel should
have an `Iron Language Server` channel with an `ext.activate` event.

## 5. Failure modes

### 5.1 `vsce publish` returns 401 / 403

`VSCE_PAT` expired, has the wrong scope, or was issued against a
different Azure DevOps organization than the one that owns the
`iron-lang` publisher namespace. Re-issue the PAT with
`Marketplace > Manage` scope, against the right org.

### 5.2 `Publisher 'iron-lang' not found`

The publisher namespace has not been claimed by the PAT's identity. See
`docs/dev/publisher-namespace-checklist.md` for the one-time
publisher-creation flow:

```bash
npx @vscode/vsce create-publisher iron-lang
# Or via web UI at https://marketplace.visualstudio.com/manage
```

### 5.3 Package size warning (> 50 MB)

The extension should be ~1 MB. If `vsce package` reports a multi-MB or
>50 MB size, inspect what got included:

```bash
npx @vscode/vsce ls iron-lsp-4.0.0-alpha.vsix | awk '{print $NF}' | xargs -I{} du -h {}
```

Add the offender to `editors/vscode/.vscodeignore`. The most common
miss is `node_modules/` (should be excluded by default but verify) and
`out/` (TypeScript test build output).

### 5.4 `vsce publish` succeeds but Marketplace does not show the new version

Marketplace indexing latency. Wait 5–15 minutes and recheck. If after
1 hour the new version still isn't visible, contact
`vsmarketplace@microsoft.com` with the publish log.

## 6. Cross-references

- The `.vsix` produced in step 1 is also uploaded to the GitHub Release
  via `scripts/build-release-artifacts.sh` (Plan 36-04 Task 3).
- Zed extension publishes via a separate PR — see
  `docs/dev/PHASE-36-ZED-PUBLISH-PLAYBOOK.md`.
- macOS `ironls` binaries shipped to the GitHub Release are signed and
  notarized via `docs/dev/PHASE-36-MACOS-NOTARIZE-PLAYBOOK.md`.
- The tag must be cut before publishing — see
  `docs/dev/PHASE-36-TAG-PUSH-PLAYBOOK.md`.
