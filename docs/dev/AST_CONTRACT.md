# AST Immutability Contract

Phase 3 NAV-15 lock. Every consumer under `src/lsp/*` and every future
NAV / hover / completion endpoint MUST treat the Iron AST as read-only
once it has come back from the analyzer.

## Contract

After `iron_analyze_buffer` returns with `result.program != NULL`, the
resulting `Iron_Program` and its transitive AST / Symbol / Type graph
are **immutable** for the lifetime of the owning `Iron_Arena`. Consumers
may read every field; **no TU outside `src/analyzer/` or `src/parser/`
may write to any field**.

Concretely, once `iron_analyze_with_mode` takes its last
`return result;`, `program->sealed` has been set to `true` and no
further mutation is permitted anywhere in the process.

## Enforcement

- A `bool sealed;` field lives on `Iron_Program` (added in
  `src/parser/ast.h`, Phase 3 Plan 01 Task 03).
- `iron_analyze_with_mode` sets `program->sealed = true;` on the single
  successful-return path in `src/analyzer/analyzer.c`.
- A macro

      IRON_AST_ASSERT_UNSEALED(program)

  is defined in `src/parser/ast.h`. In **debug builds** (when `NDEBUG`
  is NOT defined) it calls `iron_ice(...)` on a sealed program — an
  immediate `abort()` with a diagnostic message. In **release builds**
  (`-DNDEBUG`) the macro compiles down to `((void)0)`.

The trap is zero-cost at runtime for release builds and catches
accidental writes at dev time. It is **not** a security mechanism: it
is a contract violation detector.

## Rationale

Phase 2 established that every LSP feature flows through a single
facade call to `iron_analyze_buffer` (CORE-22). Phase 3 builds NAV and
hover handlers on top of the resulting `Iron_Program`. Those handlers
run concurrently on shared analyzer state; any one of them that mutates
the AST would silently corrupt the view every other handler has.

The sealed flag localises the whole problem to a single `iron_ice`
stack trace in any dev build, instead of a spooky action-at-a-distance
bug that reproduces only under mailbox coalescing pressure.

See `.planning/phases/03-m2-navigation-understanding/03-RESEARCH.md`
§Pattern 5 ("Sealed AST with Debug-Only Assert Macro") for the
clangd-prior-art analysis.

## Consumers

The following TU families are READ-ONLY consumers of sealed
`Iron_Program` graphs:

- `src/lsp/facade/nav/*` — symbol-id, node_at, definition, references
- `src/lsp/facade/hover.c` (Phase 4) — hover payload rendering
- `src/lsp/facade/document_symbol.c` (Phase 3 Plan 03) — outline
- `src/lsp/facade/workspace_symbol.c` (Phase 3 Plan 03) — workspace
  outline
- `src/lsp/facade/type_hierarchy.c` (Phase 3 Plan 05) — iface / object
  graph walks
- `src/lsp/facade/completion.c` (Phase 4) — completion candidates

Anything else that reads `Iron_Program` but does NOT write to it is
automatically compatible with the contract.

## Revision

To legitimately mutate a sealed program, re-parse + re-analyze in a
**fresh** arena. **Never clear the `sealed` flag.** The flag is a
one-way signal; resetting it would re-enable the very class of bug it
exists to prevent. Fresh arena + fresh analyze is cheap at current code
sizes (≈5 ms per file) and does not require any invalidation across
cached diagnostics or hovers.

If a future refactor finds that 5 ms per keystroke is too slow,
incremental re-analyze belongs ABOVE the sealed-AST layer: the shared
per-file cache would hold multiple sealed programs keyed by
content-hash, never mutate any of them in place.
