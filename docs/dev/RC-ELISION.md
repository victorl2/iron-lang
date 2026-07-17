# RC-ELISION.md — Iron Refcount Elision Optimizer Design Note

**Phase 29 (OPT-01 / OPT-02) — locked 2026-05-21. Sibling of [RC-LAYOUT.md](RC-LAYOUT.md) (the rc-policy ABI lock). Mirror template: RC-LAYOUT.md, DROP-LAYOUT.md.**

## Overview

Phase 29 adds an **atomic refcount elision optimizer pass** on LIR (§12 step 15). It deletes provably-cancelling `retain` / `release` pairs so single-threaded Iron code pays minimal refcount overhead while remaining sound for shared use. This is **not** load-bearing for correctness — Iron is already correct without it (just slower); it is load-bearing for the milestone's performance claim.

The pass lives in `src/lir/lir_optimize.c` (`run_rc_pair_elimination`), runs in the `iron_lir_optimize()` driver tail after `run_dead_alloca_elimination` (CFG settled) and before final lowering, and re-runs `iron_lir_verify` over the mutated stream. The conservative function-summary foundation (`src/lir/func_summary.c`, OPT-02) is a reusable module shared with Phase 30 (pointer-check elision).

See [RC-LAYOUT.md](RC-LAYOUT.md) for the `Iron_RcHeader` ABI and the runtime retain/release semantics this pass operates above.

## 1. v1 Technique: Redundant Retain/Release PAIR ELIMINATION

The dominant production technique for refcount overhead reduction (Swift `ARCSequenceOpts`, LLVM `ObjCARCOpts`) is **redundant pair elimination**: delete a `retain X … release X` pair when it provably cancels out entirely. Phase 29 implements exactly this, at the **LIR** level (locked decision GA1, overriding the ROADMAP's imprecise "on HIR" text — Iron LIR keeps retain/release opaque until C-emit and provides a ready dominator tree).

Explicitly **NOT** done in v1:

- **No atomicity-weakening** — the surviving atomic op is never downgraded atomic→non-atomic. No production system ships static atomicity elision without a region/borrow system, which Iron lacks.
- **No static thread-confinement / escape proof** — Iron has no borrow checker or region system, so sound static thread-escape proof is out of scope.
- **No full inter-procedural escape analysis** — v1 is intra-procedural pair elimination plus conservative function summaries. Any escaping or opaque call is a barrier.

The pass is **deletion-only**: it never reorders instructions, never weakens the surviving atomic, and never touches arena ownership. Soundness is therefore *by construction* — the only engineering risk is correctly identifying the barriers across which a pair must never be deleted.

## 2. Soundness Proof Obligation

A `retain`/`release` pair on the same `target` ValueId is eliminable only when **all three** hold:

- **(a) Dominance** — the `release` is dominated by its matching `retain` (`dominates(idom, retain_block, release_block)`; same-block requires the retain to precede the release in instruction order). A retain on one branch with a release in the join block is **not** dominated → preserved.
- **(b) Non-escape** — the rc value provably does not escape between them (the target ValueId does not appear as a `CALL` argument in the between-region).
- **(c) No-barrier** — no barrier instruction intervenes in the may-execute region between the retain and the release.

The may-execute region is walked by `region_between_has_barrier()`: same-block strict-between, or cross-block (tail of the retain block after the retain + head of the release block before the release + every forward-reachable intermediate block scanned in full). The region scan is conservative — when an instruction's index cannot be located it is defensively treated as a barrier.

## 3. Barrier List (as implemented)

`rcpe_instr_is_barrier()` classifies the following as hard elision barriers. The switch is exhaustive over `IronLIR_InstrKind` for `-Werror=switch-enum`. The pass never eliminates a pair whose region contains any of these:

- **`SPAWN` / `PARALLEL_FOR`** — concurrency: another thread may observe the refcount.
- **`WEAK_RC_UPGRADE`** whose source is the pair's target — an upgrade reserves a strong ref via the Rust-Arc CAS loop; the count must reflect true activity.
- **Allocation barriers** — `HEAP_ALLOC`, `RC_ALLOC`, `ARENA_ALLOC`, `ARENA_PUSH`, `ARENA_POP` — allocation/region transitions may publish or capture the value.
- **`CALL`** — barrier when the callee is `is_extern` (FFI/unknown), indirect-unresolved, has `summary.may_spawn || summary.may_free` (OPT-02 function summary), or the target appears in the call's argument list (escape).
- **comptime — by construction** — there is no comptime LIR opcode (comptime is analyzer step 10), so the comptime trap is satisfied as a behavioral no-op (Pitfall 4 confirmed). No explicit barrier instruction is needed.
- **panic-unwind** — covered transitively: a panic-reaching call is a `CALL` barrier per the rules above; the panic trap fixture straddles such a call and is preserved.

## 4. Opt-Level Gate

The pass is **optimization-level gated**: `elision_enabled = !no_optimize && !debug_build`, threaded into `iron_lir_optimize()` as a trailing `bool` from `build.c`.

- **ON at -O1+/release.**
- **OFF at -O0/debug** — debug builds keep every rc op so the Phase 31 debug allocator / leak detector observes *true* refcount activity. Eliding pairs in debug would hide real rc traffic from the leak detector.

The flag keeps the pass a pure test seam: `run_rc_pair_elimination` is independently callable and mutates only when invoked, which is how `test_rc_pair_elim_gate` models the -O0/debug behavior in-binary.

## 5. Parity-by-Construction (LSP ↔ ironc)

The Core Value forbids any divergence between what the LSP reports and what `ironc` compiles. The elision pass preserves LSP↔ironc byte-for-byte parity **by construction**: the pass runs only on the codegen path (`build.c`, inside `iron_lir_optimize`). The LSP facade (`iron_analyze_buffer`, CORE-22) stops at analyze and never lowers to LIR or calls `iron_lir_optimize`. `grep -rn iron_lir_optimize src/lsp` is empty. The LSP therefore never sees, and cannot diverge on, elision (Pitfall 6).

Moreover, because elision is deletion-only and sound, it is a **no-op on observable behavior**: each soundness-trap test constructs a scenario where naive elision *would* be unsound (e.g. a pair straddling `spawn`) and asserts the pair is PRESERVED and the program's observable output is byte-identical.

## 6. The `pairs_eliminated` Deterministic Stat

The pass emits an `IronLIR_ElisionStat { uint32_t pairs_eliminated; }` — a deterministic, queryable, **count-based** (never timing-based) statistic. Regression tests assert exact counts on small hand-built LIR fixtures: a scope-local clone-drop pair → `pairs_eliminated == 1`; a pair across `spawn` → `== 0`; a non-dominated join-block release → `== 0`. This is the test oracle that proves both the elimination *and* the barrier preservation.

## 7. OPT-08 Runtime rc-op Counter

Distinct from the compile-time `pairs_eliminated` stat, the **runtime rc-op counter** (Phase 29 Plan 04) measures actual refcount traffic at runtime. It is a pair of `_Atomic uint64_t` counters in `src/runtime/iron_rc.c`, guarded by the opt-in `IRON_RC_COUNT` compile-time macro, incremented (relaxed) inside `iron_rc_retain` / `iron_rc_release`:

- **OFF by default** — normal/release builds add ZERO instructions to the hot path; the deterministic phase-invariant test counts are unaffected.
- **ON under `-DIRON_RC_COUNT`** — an instrumented build accumulates retain/release tallies, read via `iron_rc_op_counts(uint64_t *retains, uint64_t *releases)` (always-declared; reports zeros when the macro is off, so callers compile/link identically in both configurations).

This is the **data source for OPT-08** — the benchmark that will inform the deferred OQ-07 decision below. Phase 29 only *emits* this counter; it does not act on it.

## OQ-07 — `arc` policy: DEFERRED pending OPT-08 benchmark data

**OQ-07 (the `arc` lock-free atomic-elided rc policy) is DEFERRED pending OPT-08 benchmark data.** No `arc` keyword is added in this milestone.

- The lifecycle policy set is locked at v3.0 as `{stack, heap, rc, weak rc}` (see [RC-LAYOUT.md](RC-LAYOUT.md)). `arc` is **not** part of it this phase.
- This phase ships only the *measurement infrastructure* (§7 the rc-op counter feeding OPT-08) so the deferred decision will have real data. The decision itself — whether a separate `arc` policy keyword is worth its complexity, given how much overhead pair elimination already removes — is **revisited at milestone close** once OPT-08 counter data exists.
- Grep-guard: no `arc` keyword exists in the lexer or parser. `grep -rn '"arc"' src/lexer src/parser` returns nothing. Adding an `arc` token is explicitly out of scope for Phase 29.

Static thread-confined elision (the technique an `arc` policy would unlock) would require a region/capability system Iron lacks; it is deferred alongside OQ-07.

## Cross-References

- [RC-LAYOUT.md](RC-LAYOUT.md) — `Iron_RcHeader` ABI lock + retain/release/upgrade runtime semantics.
- `src/lir/lir_optimize.c` — `run_rc_pair_elimination` + the 3-part proof + barrier scan.
- `src/lir/func_summary.c` — OPT-02 conservative function summaries (reused by Phase 30).
- `src/runtime/iron_rc.c` — OPT-08 `IRON_RC_COUNT` rc-op counter.
- `.planning/research/v4-memory-model/ATOMIC-ELISION.md` — pair-elimination prior art (Swift / LLVM), BRC, soundness traps.
