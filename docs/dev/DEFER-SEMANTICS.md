# DEFER-SEMANTICS.md — Iron `defer` Statement Semantics Design Note

**Phase 32 (DEFER-01 / DEFER-03 / DEFER-04) — locked 2026-05-25. Sibling of
[DROP-LAYOUT.md](DROP-LAYOUT.md) (the drop/copy ABI + scope-exit unwind lock)
and [RC-ELISION.md](RC-ELISION.md). Mirror template: RC-ELISION.md,
DROP-LAYOUT.md.**

## 1. Scope — generalized `defer <statement>`

Phase 32 (§12 step 18) generalizes `defer` from the Phase-21 `defer free <binding>`
idiom to **any statement**: an expression-statement, a call, or a block form
`defer { ... }`. The deferred statement runs at **scope exit** on all *normal*
control-flow edges:

- **fall-through** (reaching the closing brace of the scope),
- **`return`** (early or final),
- **`break` / `continue`** that leave the scope.

Multiple defers in a scope run **LIFO** (last-registered fires first, DEFER-03).
At scope exit the unified unwind sequence emits **defers first (LIFO), then local
destructors** in reverse declaration order (DEFER-04). Defers and local drops
share ONE ordered unwind sequence — the existing `defer_stacks` / `drop_stacks`
substrate in `src/hir/hir_to_lir.c` (`emit_scope_defers` for normal fall-through,
`emit_defer_cleanup` for early return) — not a separate parallel defer stack.

`defer free <binding>` remains the special-cased ergonomic idiom (DEFER-02,
Phase 21); the general path is the same machinery with the body widened from a
single `IRON_HIR_STMT_FREE` to an arbitrary lowered statement block.

## 2. Panic semantics (LOCKED) — defers do NOT run on panic

Iron's panic is **`abort()`-based process termination with NO C-stack
unwinding**. Every `iron_panic_*` entry point in `src/runtime/iron_panic.c`
finishes in `abort()` (the abort sites are `iron_panic.c:108, 148, 209, 255,
294`). There is no exception/unwind ABI, no `setjmp`/`longjmp` frame walk, and no
mechanism by which arbitrary user `defer` bodies execute between a panic call and
process death.

**Therefore user defers do NOT run on panic — the program aborts.** The DEFER-01
phrase "regardless of panic" is **scoped to normal control flow**
(fall-through / `return` / `break` / `continue`). This is documented behavior,
not a gap, and it honors the "reuse existing machinery / zero runtime rewrite"
decision (CONTEXT.md GA1, post-research correction).

The **only** panic-time cleanup is the partial-init field pump: each
`iron_panic_*` calls `iron_init_cleanup_run_and_clear()` when
`iron_init_cleanup_top != NULL` (the `IronInitCleanupEntry` TLS stack — see
`iron_panic.c:79, 125, 225, 269, 316, 370` and DROP-LAYOUT.md §4). Arbitrary user
defers and `_drop` destructor field-cleanup do **not** fire on the abort path.

The existing fixture
`tests/integration/v4/4.11-ptr-check-elision/defer_panic.iron` encodes the
**supportable** behavior: a callee's `defer free b` runs on the callee's
**normal return edge**; a later stale dereference in `main` then panics. The
panic is a *consequence* of the defer having already run on a normal edge — NOT
a defer running *during* a panic unwind. **Do not author a fixture asserting a
defer runs DURING a panic** — it cannot pass under the current runtime model.

## 3. Read-at-exit semantics (LOCKED)

A deferred statement's variables are **read at scope-exit time**, NOT snapshotted
at `defer`-registration time (this is explicitly NOT Go-style argument
snapshotting).

```iron
var x = 1
defer print(x)
x = 2
-- prints 2
```

This is **correct by construction**: `emit_scope_defers` / `emit_defer_cleanup`
**inline-lower the defer body's statements at the exit edge**, not at the
registration site. Every variable reference inside the defer body is emitted as a
fresh load at exit, after all intervening mutations have been lowered. There is no
capture/snapshot step. `tests/integration/v4/4.13-defer/defer_read_at_exit.iron`
witnesses this (expected stdout `2`).

## 4. DROP-05 residual (DOCUMENTED, NOT closed in Phase 32)

DROP-05 (partial-init cleanup: a panic mid-`init` destroys already-set fields in
reverse-assignment order) is **NOT a defer problem** and is **not closed** in
Phase 32. The residual is the **compound-literal init-inlining limitation**
quoted verbatim from [DROP-LAYOUT.md](DROP-LAYOUT.md) §4:

> Iron's compiler always inlines `init` method calls as compound literals
> (`Multi(true)` → `(Iron_Multi){.first = true}`), making the `multi_init`
> C function dead code. The `iron_init_cleanup_register` machinery is
> correctly emitted in the LIR body of `*_init` functions but is not
> exercised end-to-end by the current corpus.

Because `init` is emitted as a C **compound literal** (not a real function call),
the `iron_init_cleanup_register` calls in the `*_init` LIR body are **dead code**
— the register path never executes, so a panic-mid-init has nothing on the TLS
stack to run. The `IronInitCleanupEntry` register/run-and-clear runtime is
complete and linked into every binary; only the call path is absent.

The defer-generalization work touches function/block scope exit; it does **not**
touch the inlined-init compound-literal path. Closing DROP-05 end-to-end requires
a **separate** `emit_c.c` codegen change — emit `init` as a real function call
for objects with droppable fields, so the `iron_init_cleanup_register` calls
become live — INDEPENDENT of defer. Per the locked decision (CONTEXT.md GA2 +
Deferred Ideas), the residual is **documented here and deferred to a follow-up**;
it is not force-closed in this phase.

## Cross-References

- [DROP-LAYOUT.md](DROP-LAYOUT.md) §4 — `IronInitCleanupEntry` TLS partial-init
  pump + the compound-literal init-inlining limitation (the DROP-05 residual).
- [RC-ELISION.md](RC-ELISION.md) — sibling design note (deferred/limited-behavior
  documentation precedent).
- `src/hir/hir_to_lir.c` — `emit_scope_defers` (normal exit), `emit_defer_cleanup`
  (early return), the unified `defer_stacks` / `drop_stacks` substrate + DEFER-04
  interleave.
- `src/runtime/iron_panic.c` — `abort()`-based panic + `iron_init_cleanup_*` TLS
  pump (the only panic-time cleanup).
- `tests/integration/v4/4.13-defer/` — the Phase 32 defer fixture corpus.
- `tests/integration/v4/4.11-ptr-check-elision/defer_panic.iron` — the
  supportable defer-then-stale-deref-panic fixture.
