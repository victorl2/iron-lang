# EXT-07 — nvim-lspconfig v2 upstream contribution (post-v1)

This document is the **tracking artifact** for requirement EXT-07
(Phase 6 M5 Grammars & Editor Extensions). The requirement commits the
project to upstream a server definition for Iron to
`neovim/nvim-lspconfig` so that any Neovim user can install Iron LSP
support by dropping a single configuration line into their `init.lua`.

The **physical PR** is explicitly post-v1 per CONTEXT.md D-05 (locked
2026-04-19). This file is the bridge: it records the contribution
contract, the verification state at Phase 6 close, and the
step-by-step checklist that unblocks the PR once the external-user
validation prerequisite clears.

Related:
- [`editors/neovim/lsp/ironls.lua`](../../editors/neovim/lsp/ironls.lua) — the exact file to upstream
- [`editors/neovim/README.md`](../../editors/neovim/README.md) — user-facing Neovim setup
- `.planning/phases/06-m5-grammars-editor-extensions/06-04-SUMMARY.md`
  (local-only) — Assumption A4 verification result
- `.planning/phases/06-m5-grammars-editor-extensions/06-CONTEXT.md`
  D-05 (local-only) — the locked decision

---

## Why upstream

`neovim/nvim-lspconfig` is the default server-registry plugin for
Neovim. Once Iron is listed there, any Neovim user can enable the
language server with:

```lua
-- user's init.lua, after nvim-lspconfig is installed:
require('lspconfig').ironls.setup({})
```

Without upstreaming, each user has to copy our
`editors/neovim/lsp/ironls.lua` onto their runtimepath manually — a
higher-friction onboarding than every other LSP-supported language in
the ecosystem. Upstream adoption removes that friction and makes Iron
discoverable through the same plugin Neovim users already use for
~200 other language servers.

---

## Prerequisite — why the PR is post-v1

Per CONTEXT.md D-05 (locked 2026-04-19):

> Phase 6 v1 ships only `editors/neovim/lsp/ironls.lua` in-tree. After
> ≥ 1 external user validates the config, a PR to
> `neovim/nvim-lspconfig` registers `ironls` as a server entry.
> Tracked as a separate post-v1 task in the backlog; not a blocker
> for Phase 6 close.

The prerequisite exists because:

- **Cross-machine validation.** The project owner's machine is the
  only one the config has been exercised on. nvim-lspconfig
  maintainers expect server definitions to work across OS / shell /
  PATH layout variations before merge.
- **root_markers heuristic.** `root_markers = { 'iron.toml', '.git' }`
  reflects the project owner's workflow (the compiler itself is an
  `.git`-only workspace; user Iron projects will have `iron.toml`).
  Confirming at least one other workspace shape validates the
  heuristic before it hits upstream where it would be harder to
  change.
- **Platform-specific PATH edge cases.** macOS Homebrew (`/opt/homebrew/bin`
  vs `/usr/local/bin` on Intel), Nix (`~/.nix-profile/bin`), and
  Windows Scoop (`%USERPROFILE%\scoop\shims`) all have distinct PATH
  conventions. v1 tests on Linux (CI) + macOS (CI) + the author's
  machine; ≥ 1 external report catches paths we haven't seen.

### Tracking

A GitHub issue titled **"EXT-07: nvim-lspconfig upstream — awaiting
external-user validation"** opens in `iron-lang/iron-lang` when the
repo goes public. Users who've validated the config post a
confirmation comment. ≥ 1 confirmation unblocks this PR.

---

## Assumption A4 verification (from Plan 06-04)

Plan 06-04 Task 1 landed `editors/neovim/lsp/ironls.lua` as the
canonical Neovim configuration, shaped deliberately to be byte-compatible
with the nvim-lspconfig v2 `lsp/*.lua` contribution shape.

**Assumption A4 (Plan 06-04 RESEARCH):** "The `vim.lsp.config()` table
shape introduced in Neovim 0.11.3 is the same shape nvim-lspconfig v2
expects for `lsp/<server>.lua` upstream contributions."

**Verification result (from Plan 06-04 SUMMARY):**
**CONFIRMED — byte-compatible.** The file `editors/neovim/lsp/ironls.lua`
returns a table of the exact shape nvim-lspconfig v2 expects. No shape
adjustments required at submission time.

Specifically:

- `cmd = { 'ironls' }` — matches nvim-lspconfig convention (array of
  argv tokens, not string).
- `filetypes = { 'iron' }` — matches; nvim-lspconfig uses array form
  throughout `lsp/`.
- `root_markers = { 'iron.toml', '.git' }` — matches the shape seen
  in nvim-lspconfig `lsp/rust_analyzer.lua`,
  `lsp/lua_ls.lua`, `lsp/gopls.lua` (use `root_markers` not
  `root_dir` — the v2 shape).
- `settings = { ... }` — reserved namespace, present with empty map.
- `init_options = { clientName = 'neovim' }` — nvim-lspconfig
  convention is `init_options` (snake_case) not `initializationOptions`.
- `on_attach = function(client, bufnr) ... end` — standard signature.
- `compatible_ironls = "1.2.0..<1.3.0"` — non-standard field, accepted
  by v2 shape (any extra keys are preserved on the returned table).

If `editors/neovim/lsp/ironls.lua` is ever refactored, re-verify A4
against `neovim/nvim-lspconfig/CONTRIBUTING.md` before the next
upstream submission attempt.

---

## PR draft

The upstream PR submits `editors/neovim/lsp/ironls.lua` **as-is** to
`neovim/nvim-lspconfig`'s `lsp/` directory.

### Target

- Upstream repo: `neovim/nvim-lspconfig`
- Upstream branch: `master`
- File path in fork: `lsp/ironls.lua`
- Contents: **byte-identical** to `editors/neovim/lsp/ironls.lua` in
  this repo (no rewrite at submission time — keeping them byte-identical
  lets reviewer feedback flow back into the in-tree file verbatim).

### Additional files

- `doc/configs.md` — add an entry per
  `neovim/nvim-lspconfig/CONTRIBUTING.md`:
  - Server name: `ironls`
  - Filetypes: `iron`
  - Root markers: `iron.toml`, `.git`
  - Install instructions: link to
    `https://github.com/iron-lang/iron-lang/blob/main/INSTALL.md`
    + the direct `editors/neovim/README.md`.

---

## Submission checklist

Steps the project owner (or a contributor) follows once the prerequisite
clears:

1. [ ] External-user validation received (≥ 1 confirmation comment on
       the tracking issue)
2. [ ] Re-read `neovim/nvim-lspconfig/CONTRIBUTING.md` at HEAD — the
       contribution guidelines evolve independently of the code
3. [ ] Re-verify Assumption A4 against current upstream `lsp/*.lua`
       shape (spot-check 2-3 recently merged files)
4. [ ] Fork `neovim/nvim-lspconfig` on GitHub
       (`gh repo fork neovim/nvim-lspconfig`)
5. [ ] Create a branch: `add/ironls`
       (`git checkout -b add/ironls`)
6. [ ] Copy `editors/neovim/lsp/ironls.lua` from this repo to
       `lsp/ironls.lua` in the fork — byte-identical, no edits
7. [ ] Write the `doc/configs.md` entry per CONTRIBUTING.md
8. [ ] Run the fork's own local checks
       (`make` / `make test` / whatever CONTRIBUTING says)
9. [ ] Commit with a short conventional message:
       `feat(lsp): add ironls server definition`
10. [ ] Open the PR against `master` with a body that:
    - Links to `iron-lang/iron-lang` README
    - Links to the install docs (`INSTALL.md` + per-editor
      `editors/neovim/README.md`)
    - Quotes the external-user confirmation (screenshot or link to
      comment)
    - Notes Assumption A4 shape compatibility was verified
11. [ ] Address reviewer feedback — iterate on `lsp/ironls.lua` in the
       fork **and** in-tree simultaneously, keeping the two copies
       byte-identical throughout
12. [ ] Merge — maintainers squash-merge typical
13. [ ] Announce in the iron-lang CHANGELOG + project channels:
       *"Iron LSP now discoverable via nvim-lspconfig — `require('lspconfig').ironls.setup{}`
       in your init.lua."*

---

## Expected review cycle

nvim-lspconfig maintainers typically respond within 1-2 weeks on new
server definitions. Common review feedback seen on similar PRs:

- **`root_markers` ordering.** Some reviewers prefer `.git` last (as
  a fallback) or first (to match `rust_analyzer.lua`). Cosmetic —
  accept whichever the reviewer prefers.
- **`filetypes` stylistic tweaks.** Usually no-ops; occasionally a
  reviewer asks for alphabetization if we had multiple filetypes.
- **`on_attach` cleanup.** If reviewer finds our `pcall(require, ...)`
  pattern unusual, we can inline the S5 log emit into a no-op stub
  upstream and keep the plugin hook in-tree only. Cosmetic — either
  shape works.
- **`doc/configs.md` wording.** Often rewritten to match house style.

### Keeping fork and in-tree in sync

While reviewer feedback is in flight, any edits to
`lsp/ironls.lua` in the fork **must** be mirrored back into
`editors/neovim/lsp/ironls.lua` in this repo (or vice versa) to
preserve Assumption A4's byte-identical contract. Use:

```bash
diff editors/neovim/lsp/ironls.lua path/to/nvim-lspconfig-fork/lsp/ironls.lua
```

and fail-loud if the diff is non-empty.

---

## Post-merge

1. Close the `EXT-07` tracking issue as **complete**
2. Update `CHANGELOG.md` in iron-lang:
   *"Iron LSP registered with nvim-lspconfig — `require('lspconfig').ironls.setup{}`
   now works out of the box for Neovim 0.11.3+."*
3. Remove the "EXT-07 — tracked post-v1" note from
   `editors/neovim/README.md` (if present) and replace with a
   one-liner pointing at the new discovery path.
4. **Optional follow-up:** open a companion PR to
   `nvim-treesitter/nvim-treesitter` registering `iron` parser. Plan
   06-02 shipped `iron.wasm`; nvim-treesitter parser registration is
   the natural parallel ecosystem touchpoint.

---

## Risk / mitigation

- **Risk:** nvim-lspconfig ships a breaking `lsp/*.lua` shape change
  (v3+) between Phase 6 close and upstream submission → our config
  becomes outdated before we land it.
  **Mitigation:** re-verify Assumption A4 at submission time
  (checklist step 3 above). If the shape has changed, update
  `editors/neovim/lsp/ironls.lua` first, run
  `editors/neovim/test/e2e/harness.sh` to confirm it still works
  locally + in CI, then submit the updated shape upstream.
- **Risk:** nvim-lspconfig maintainers reject a field we rely on
  (e.g. `compatible_ironls`).
  **Mitigation:** accept removal upstream (the field is a Phase 6
  extension, not part of the core LSP contract); keep the in-tree
  copy intact and document the divergence in this file under a new
  "Known divergences" section if needed.
- **Risk:** external-user validation never arrives.
  **Mitigation:** the PR is post-v1 and not a Phase 6 blocker.
  Validation can arrive at any time during public beta; the tracking
  issue remains open and visible.

---

## File integrity check

Run this command before submitting the PR to confirm the fork's
`lsp/ironls.lua` is byte-identical to the in-tree file:

```bash
cmp editors/neovim/lsp/ironls.lua path/to/nvim-lspconfig-fork/lsp/ironls.lua \
  && echo "byte-identical — ready to submit" \
  || echo "DIVERGENCE — re-sync before submission"
```

Both files MUST be byte-identical at PR-open time. The in-tree file
is the source of truth during review.
