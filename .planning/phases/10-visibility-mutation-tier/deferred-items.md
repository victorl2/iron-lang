# Phase 10 Deferred Items

Issues discovered during execution that are OUT-OF-SCOPE for the current task.
Per executor scope rule: only auto-fix issues directly caused by current task changes.

## Pre-existing build failures (not regressions from Phase 10 work)

### test_ast_sealed: -Werror=address on IRON_AST_ASSERT_UNSEALED macro

**File:** `tests/lsp/unit/test_ast_sealed.c:81`
**Macro:** `src/parser/ast.h:176` (`IRON_AST_ASSERT_UNSEALED`)
**Error:**
```
src/parser/ast.h:176:17: error: the address of 'p' will always evaluate as 'true' [-Werror=address]
```

The macro checks `if ((program) && ...)`. When the call site passes `&p` where `p` is a stack-local `Iron_Program`, gcc 11.5 raises `-Werror=address` because the address of a stack variable is always non-NULL. Reproduced on bare `git stash` of the base commit `03455c8`. Not introduced by Phase 10 work; pre-existing on the worktree base.

**Recommendation:** Fix in a separate plan. Either drop the NULL guard inside the macro (callers already guarantee non-NULL via the address-of operator), or wrap the macro arg in a way that avoids the address-of constant-truth warning (e.g., volatile cast).

**Phase 10 Plan 10-01 status:** Verified the failure pre-dates this plan — running `cmake --build build --target test_ast_sealed` after `git stash` still fails with the same error. Therefore this is not a Phase 10 regression and does not block plan completion.

### test_string_intern_race: missing libtsan.so on host

**Target:** `tests/unit/test_string_intern_race`
**Error:**
```
/usr/bin/ld: cannot find /usr/lib64/libtsan.so.0.0.0
```

The test is compiled with `-fsanitize=thread` but the host's `gcc-c++-libs` package is missing `libtsan.so.0.0.0`. Reproduced on the bare worktree base. Host environment issue, not a Phase 10 regression.

**Recommendation:** Either install `libtsan` on the host, or guard the TSAN-link in the test target on `find_library(TSAN_LIB tsan)` so missing-library hosts fall back to a SKIP shim.

### test_v3_symbol_id_corpus: baseline path is parent-repo-rooted

**Target:** `tests/lsp/unit/test_v3_symbol_id_corpus.c`
**Failure:**
```
FAIL: v2 zero-churn breach: divergence at byte 37: baseline="/home/victor/code/iron-lsp/iron-lang/tests/integration/adt_else_arm.iron…"
```

The Phase 9 corpus baseline was generated against the canonical iron-lang repo path (`/home/victor/code/iron-lsp/iron-lang/...`) but the worktree resolves `IRON_SOURCE_TREE_ROOT` to a different absolute path (`.../.claude/worktrees/agent-…/...`). The baseline test compares full canonical_path strings byte-for-byte and therefore diverges in any worktree. Reproduced on the bare worktree base. Not a Phase 10 regression.

**Recommendation:** Strip the absolute prefix before serialising the baseline (Phase 9 D-04 baseline format), or rebase the baseline at every CTest invocation when running inside a worktree. Either way, this is a parallel-execution infrastructure concern, not a Phase 10 deliverable.

---

## Phase 10 Plan-Level Deferrals (added by Plan 10-03 closeout)

These items are intentionally NOT shipped in Phase 10; tracked here so future phases pick them up.

### TIER-02 — context-aware keyword filtering for `readonly` / `pure`

- **Deferred to:** Phase 12 KW-03
- **Reason:** Per CONTEXT.md D-10 + RESEARCH § "Phase Requirements" verification at REQUIREMENTS.md:187, KW-03 already explicitly says "readonly + pure only immediately before func token". Same machinery as TIER-02. Phase 10 ships the rendering bits (TIER-01 verification, TIER-03 completion detail, TIER-04 signature help label); Phase 12 KW-03 owns the context-aware completion filtering for these keywords.
- **Owner ID in REQUIREMENTS.md:** TIER-02 (line 171) — to be addressed by KW-03 (line 187)
- **Frontmatter discoverability:** Plan 10-03 frontmatter `deferred:` block records this with `to: phase-12-KW-03` so the requirements coverage gate sees TIER-02 as addressed (deferred), not missing.

### VIS-05 hover prefix on Iron_ObjectDecl when AST gains is_private

- **Deferred to:** future compiler change (no current phase owns this)
- **Reason:** Per RESEARCH Conflict 3, `Iron_ObjectDecl` has no `is_private` field today; the parser drops the `private` keyword on top-level decls (parser.c:4047, 4606, 4737). Plan 10-03 hover currently renders `pub patch object` for ALL objects (the predicate defaults-true via the default arm in `ilsp_vis_is_public`). If a future compiler change adds `is_private` to `Iron_ObjectDecl`, the predicate's switch in `src/lsp/facade/nav/visibility.c` should be extended with an `IRON_NODE_OBJECT_DECL` arm so the LSP no longer renders `pub` on objects the AST marks private.
- **Why this is acceptable today:** The language has no way to express a private object today, so there is no false-pub case — every object that exists IS publicly visible.

### Stdlib pub migration (XXX_PHASE_14 marker)

- **Deferred to:** Phase 14 MIG-01
- **Reason:** D-08 carve-out lives at `src/lsp/facade/nav/visibility.c::ilsp_vis_can_see` (consults `ilsp_nav_path_is_stdlib`) until MIG-01 stamps `pub` onto `src/stdlib/*.iron`. The `XXX_PHASE_14 MIG-01` marker in visibility.c makes the future flip site grep-discoverable.

### `is_pub_setter` differentiated rename gating

- **Deferred to:** backlog (not in any current phase)
- **Reason:** Per RESEARCH Conflict 1, `Iron_Field.is_pub_setter` does not exist; that bit lives on `Iron_AssignStmt`. Plan 10-02 uses read-axis `is_pub` for rename gating per D-09. Differentiated write-axis gating (rename only the assignment site, not the read sites) is not a current concern; can be added later if the discipline matures.
