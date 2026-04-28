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
