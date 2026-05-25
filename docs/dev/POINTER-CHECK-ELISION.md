# POINTER-CHECK-ELISION.md — Iron Generation-Check Elision Optimizer Design Note

**Phase 30 (OPT-03 … OPT-08 / OQ-08) — locked 2026-05-25. Sibling of [RC-ELISION.md](RC-ELISION.md) (the Phase 29 refcount-elision note) and [UNCHECKED-LAYOUT.md](UNCHECKED-LAYOUT.md) (the checked/unchecked pointer ABI). Mirror template: RC-ELISION.md.**

## Overview

Phase 30 adds a **generation-check elision optimizer pass** on LIR (§12 step 16), modeled on Vale's Catalyst. Every checked deref of a generational pointer (heap / stack / arena) runs an `iron_check_{heap,stack,arena}_pointer_gen` guard that traps on a use-after-free. This pass deletes provably-redundant guards so idiomatic Iron pays for each distinct liveness fact at most once, while every guard that *could* catch a real stale deref is preserved.

This is **not** load-bearing for correctness — Iron is already memory-safe without it (every deref is checked); it is load-bearing for the milestone's performance claim. The pass is **deletion/hoist-only**: it removes a `GENCHECK` or moves it earlier (loop preheader); it never reorders a check after a barrier, never weakens a surviving check, and never removes the generation slot itself. Soundness is therefore *by construction* — the engineering risk is purely in correctly identifying the may-free barriers across which a check must never be elided.

The pass lives in `src/lir/lir_optimize.c` (`run_pointer_check_elimination`), runs in the `iron_lir_optimize()` driver tail immediately after `run_rc_pair_elimination` (CFG settled, func summaries computed) under the same `elision_enabled` gate, and the existing post-pass `iron_lir_verify` re-runs over the mutated stream. It reuses Phase 29's `src/lir/func_summary.{c,h}` `may_free` bit for Tier 4.

See [UNCHECKED-LAYOUT.md](UNCHECKED-LAYOUT.md) for the `Iron_FatPtr` / generation ABI the runtime guards operate over, and [RC-ELISION.md](RC-ELISION.md) for the sibling refcount pass whose structure this mirrors.

## 1. The GENCHECK Intrinsic

Before the optimizer, checked derefs are lowered to an explicit `IRON_LIR_GENCHECK ptr, root_alloc, gen_source` intrinsic (`lower_genchecks`, Plan 30-02), with side-effect semantics "may trap." This runs **unconditionally** at every optimization level — even at -O0 — so the IR shape is identical and only the *elision* of these intrinsics is gated. Surviving `GENCHECK` intrinsics expand late in `emit_c` to the existing per-source `iron_check_{heap,stack,arena}_pointer_gen` routing (byte-identical to the old inline check). Introducing the intrinsic lets standard CSE/LICM/DCE-style reasoning see the checks as first-class instructions without bespoke per-deref code.

## 2. OQ-08 — The "Provably-Fresh-for-Elision" Boundary (formalized)

> **Provably-fresh-for-elision.** A generation check on pointer `p` (canonicalized root `R`, gen source `S`) is provably redundant and may be elided **iff** on every control-flow path reaching this use there is a prior point at which `R`'s liveness was established for `S` — (i) `R`'s allocation / `&`-creation site, or (ii) an earlier `GENCHECK` on the same `(R, S)` that this use is dominated by — such that **no may-free op on `R` occurs on any path between that point and this use.**

This is established via **dominator analysis + may-free dataflow**: the dominating prior point (the allocation site or an earlier dominating check) is found over the dominator tree (`build_domtree` / `dominates`), and the absence of an intervening barrier is checked by the may-free region walk (§4). Redundancy is judged on **root-allocation identity** (§3), not pointer identity.

Tier 2 adds a **non-escaping stack-local freshness clause by construction**: a generation check on a STACK-sourced pointer whose canonicalized root ALLOCA never has its *address* escape the current frame is fresh for the frame's lifetime — the slot cannot be freed by anyone else — so the check is redundant without needing a dominating prior check. (Per the CONTEXT deferral, Tier 2 elides only the *check*, never the generation slot.)

## 3. Root-Allocation Canonicalization

The elision relation keys on **root-allocation identity**. Derived / field pointers canonicalize back to their outermost allocation: `&obj.a` and `&obj.b` share root `obj`, so a successful check on one covers the other (same root, no intervening free). `canonicalize_root(fn, ptr)` (Plan 30-02) walks address-producing chains — `ADDR_OF`, `PTR_OFFSET`, `GET_FIELD`, `GET_INDEX` — back to the underlying `ALLOCA` / `HEAP_ALLOC` / `RC_ALLOC` / `ARENA_ALLOC`. Two `GENCHECK`s with the same canonical root and gen source are candidates for redundancy elimination.

## 4. The may-free Barrier Set (as implemented)

`gencheck_instr_is_may_free()` classifies an instruction as a hard elision barrier — a point at which root `R`'s allocation may have been freed, so a later check on `R` is no longer covered by an earlier one. `gencheck_region_has_may_free()` clones the Phase 29 region walk (same-block strict-between, or cross-block: tail of the first block, head of the second, plus every forward-reachable intermediate block scanned in full; an unlocatable index is defensively treated as a barrier). The pass never elides a check whose may-execute region contains any of:

- **`CALL`** — barrier **unless** the callee is provably clean: a direct call to a module function whose `func_summary.may_free == false`. Extern / FFI, indirect-unresolved, or `may_free == true` callees are pessimistic barriers (this is **Tier 4**, woven into the CALL arm rather than a separate deletion). A clean call is transparent.
- **`FREE`** — explicit free of the root (or any pointer).
- **`RC_RELEASE` / `WEAK_RC_RELEASE`** — a release may drop the last strong ref and run the destructor, freeing the block.
- **Allocation barriers** — `HEAP_ALLOC`, `RC_ALLOC`, `ARENA_ALLOC` — a container grow / reallocation may move or invalidate the backing store (stale-after-grow).
- **`ARENA_PUSH` / `ARENA_POP`** — an arena reset frees everything allocated since the matching push (SAFE-05).
- **`STORE` / `SET_FIELD` / `SET_INDEX`** — assignment to an owning slot runs `drop` on the previous occupant (drop-on-assignment). v1 is conservative: **every** store is treated as a may-free barrier regardless of destination type (loses some Tier 1 wins, stays sound; the `drop_on_assignment` trap fixture pins it).
- **`SPAWN` / `PARALLEL_FOR`** — concurrency: another thread may free the allocation between the check and the use.
- **`defer` body / panic unwind** — covered transitively: a `defer free` lowers to a `FREE`, and a panic-reaching call is a `CALL` barrier; the `defer_panic` trap fixture straddles such a barrier and is preserved.

The classifier is an `if/else-if` chain over `IronLIR_InstrKind` (a sparse predicate; not a `switch`, to avoid forcing an exhaustive opcode list under `-Werror=switch-enum`).

## 5. The 4-Tier Design

All four tiers ship in v1 (phase goal mandate). Tier ordering inside `run_pointer_check_elimination`: per-function `rebuild_cfg_edges` → `build_domtree` → **Tier 2** (escape; never breaks redundancy) → **Tier 1** (dominator CSE) → block compaction → **Tier 3** (LICM hoist). Tier 4 is woven into the barrier classifier and func summaries are computed once per module.

- **Tier 1 — dominator redundant-check CSE (`by_tier[0]`):** delete a `GENCHECK` on `(R, S)` that is dominated by an earlier `GENCHECK` on the same `(R, S)` with no may-free op between (the EarlyCSE-equivalent; §2 clause ii).
- **Tier 2 — non-escaping STACK-local elision (`by_tier[1]`):** delete a `GENCHECK` on a STACK-sourced root whose address never leaves the frame (`gencheck_build_escape_map` / `gencheck_mark_ptr_escape`). Pointer-escape, not value-escape: returning the scalar slot value is not an escape; only a fat *pointer* to the slot flowing out (`ADDR_OF` / `PTR_OFFSET` / `GET_FIELD` / `GET_INDEX` producer) escapes.
- **Tier 3 — LICM hoist-to-preheader (`by_tier[2]`):** hoist a loop-invariant check (root defined outside the loop) to the loop preheader when the loop is provably entered (`preheader != 0 && dominates(preheader, header)`) and the loop body has no may-free op. A zero-trip loop (no single preheader) or a may-free body keeps the check in place.
- **Tier 4 — function-summary may-free (`by_tier[3]` reserved):** not a separate deletion pass — it is the refinement of the `CALL` arm of the barrier classifier (§4) using `func_summary.may_free`. A clean module call is transparent so a check straddling it is still elidable by Tier 1.

## 6. Opt-Level Gate

The elision pass is **optimization-level gated**: `elision_enabled = !no_optimize && !debug_build`, threaded into `iron_lir_optimize()` from `build.c`.

- **ON at -O1+/release.**
- **OFF at -O0/debug** — debug builds keep every generation check so the Phase 31 debug allocator / leak detector observes *every* deref. Eliding checks in debug would hide real deref traffic from the allocator instrumentation.

`lower_genchecks` runs **unconditionally** (the GENCHECK intrinsic shape is identical at all levels); only `run_pointer_check_elimination` is gated. The flag keeps the pass a pure test seam: the pass is independently callable and mutates only when invoked, which is how `test_gate_off_preserves` models the -O0/debug behavior in-binary.

## 7. Parity-by-Construction (LSP ↔ ironc)

The Core Value forbids any divergence between what the LSP reports and what `ironc` compiles. The elision pass preserves LSP↔ironc byte-for-byte **parity by construction**: it runs only on the codegen path (`build.c`, inside `iron_lir_optimize`). The LSP facade (`iron_analyze_buffer`, CORE-22) stops at analyze and never lowers to LIR or calls `iron_lir_optimize`. `grep -rn iron_lir_optimize src/lsp` is empty. The LSP therefore never sees, and cannot diverge on, generation-check elision.

Moreover, because elision is deletion/hoist-only and sound, it is a **no-op on observable behavior**: a pointer that WOULD trap still traps. Each soundness-trap fixture (`tests/integration/v4/4.11-ptr-check-elision/`) constructs a scenario where naive elision *would* be unsound — `concurrent_mutation`, `stale_after_grow`, `drop_on_assignment`, `defer_panic`, `polymorphic_dispatch` — and asserts the check is PRESERVED and the program's output is correct (the stale deref still fires its guard).

## 8. The `checks_elided` / `by_tier` Deterministic Stat

The pass fills an `IronLIR_GenCheckElisionStat { int checks_total; int checks_elided; int by_tier[4]; }` — a deterministic, queryable, **count-based** (never timing-based) statistic. `checks_total` is the number of GENCHECKs before the pass (= checked deref sites); `checks_elided` is the total deleted/hoisted; `by_tier[i]` is the per-tier breakdown. The `--dump-ir-passes` driver prints it (`=== After gencheck-elim: N checks elided (T1=… T2=… T3=…) ===`).

This is the **OPT-08 test oracle**: `tests/lir/test_gencheck_elision.c` asserts exact counts on hand-built LIR — two field reads off one root → `by_tier[0]==1`; a loop-invariant check → `by_tier[2]==1`; a check across any may-free barrier → `checks_elided==0`. The microbench cases (`test_microbench_multi_field`, `test_microbench_tight_loop`, `test_microbench_elision_rate`) assert per-tier counts and the computed elision *rate* (`checks_elided/checks_total`) on idiomatic shapes via integer cross-multiply — never wall-time.

## 9. OPT-08 Runtime gen-check Counter

Distinct from the compile-time `checks_elided` stat, the **runtime gen-check counter** (`src/runtime/iron_gencheck_count.c`, Plan 30-01) measures actual deref-check traffic at runtime. Three `_Atomic uint64_t` counters (heap / stack / arena), guarded by the opt-in `IRON_GENCHECK_COUNT` compile-time macro, are bumped (relaxed) inside the static-inline `iron_check_{heap,stack,arena}_pointer_gen` guards in `iron_runtime.h`:

- **OFF by default** — normal/release builds add ZERO instructions to the deref hot path; the deterministic phase-invariant test counts are unaffected.
- **ON under `-DIRON_GENCHECK_COUNT`** — an instrumented build accumulates per-source tallies, read via `iron_gencheck_counts(uint64_t *heap, uint64_t *stack, uint64_t *arena)` (always-declared; reports zeros when the macro is off, so callers compile/link identically in both configurations) and zeroed via `iron_gencheck_counts_reset()`. Exercised end-to-end by `tests/unit/test_gencheck_counter.c`.

This is the structural mirror of `iron_rc.c`'s `IRON_RC_COUNT` block. It is the **runtime data source for OPT-08** — Phase 30 only *emits* this counter; it does not act on it.

## 10. Deferred Items

- **Published milestone-close benchmark report** (the OPT-08 elision-rate report) — **DEFERRED to Phase 36**. This phase emits the raw counters only (the deterministic `by_tier` stat + the runtime `IRON_GENCHECK_COUNT` counter); the aggregated report against idiomatic corpora lands at milestone close.
- **Vale-style scope tethering** (a `u1` bit delaying free) — postponed; requires user-visible drop-timing semantics.
- **Full HGM / region borrowing** — out of scope for v1.
- **Per-type-set function summaries** — v1 is a single `may_free` bit (+ per-parameter refinement at most); richer summaries are future work.
- **Removing the generation slot for non-escaping stack locals** (Tier 2's strongest form) — only the check is elided; the slot is kept.
- **ABCD-style Tier 5 / range-check fusion** — out of scope.

## Cross-References

- [RC-ELISION.md](RC-ELISION.md) — the Phase 29 refcount-elision pass this mirrors (technique, proof, barrier list, gate, parity, stat, counter, deferral).
- [UNCHECKED-LAYOUT.md](UNCHECKED-LAYOUT.md) — `Iron_FatPtr` / generation ABI + the checked/unchecked pointer distinction.
- `src/lir/lir_optimize.c` — `run_pointer_check_elimination` (4 tiers) + `gencheck_instr_is_may_free` + `gencheck_region_has_may_free` + the escape map + driver registration.
- `src/lir/func_summary.c` — OPT-02 conservative function summaries (the Tier 4 `may_free` oracle, shared with Phase 29).
- `src/runtime/iron_gencheck_count.c` — OPT-08 `IRON_GENCHECK_COUNT` runtime gen-check counter.
- `.planning/research/v4-memory-model/POINTER-CHECK-ELISION.md` — Vale Catalyst / Cyclone / ABCD / LLVM EarlyCSE prior art, tier design, soundness traps.
