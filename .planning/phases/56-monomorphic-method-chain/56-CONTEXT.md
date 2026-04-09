# Phase 56: Monomorphic Method Chain - Context

**Gathered:** 2026-04-09
**Status:** Ready for planning

<domain>
## Phase Boundary

Fix the bug where monomorphic-collapsed collections (Phase 49's §8.3 specialization — collections proven to hold only one concrete type collapse to plain typed arrays) reference a `Iron_List_<ConcreteType>` C type that is never declared. The collapse pass at `src/lir/emit_c.c:4797-4814` removes collections from `split_collection_ids`, causing them to fall through to the plain typed array emission path — but the plain typed array path assumes `IRON_LIST_DECL/IRON_LIST_IMPL` macros have already been instantiated for the target type, which they are NOT for concrete object types. Only primitives (`int64_t`, `int32_t`, `double`, `bool`, `Iron_String`, `Iron_Closure`) are pre-declared at `src/runtime/iron_runtime.h:640-645`. Collapsed `Iron_Circle`, `Iron_Square`, etc. are referenced in generated C as `Iron_List_Iron_Circle` without ever being declared, causing clang to fail with `use of undeclared identifier 'Iron_List_Iron_Circle'`.

**Reproducible with two different symptom paths:**

1. **True monomorphic chain:** `val shapes = [Circle(3), Circle(4), Circle(5)]; shapes.map(f).sum()` — 3-element concrete literal triggers mono collapse, `.map()` codegen fails.
2. **Single-element narrowing path:** `var shapes = [Circle(1)]; shapes.push(Square(2))` — single-element literal also narrows to `[Circle]` (same mono collapse), subsequent `.push(Square(2))` hits the same undeclared `Iron_List_Iron_Circle_push` symbol.

Both symptoms share the same root cause: **the mono collapse path never registers the concrete object type for `IRON_LIST_DECL` emission.** Fixing the decl-emission fixes both.

**Phase 56 scope is broadened from CONTEXT.md decisions below** to include: the primary fix (decl emission), narrowing-semantics audit (verify `.push(WrongType)` on a narrowed collection produces a clean Iron type error instead of a codegen blowup), Phase 55 workaround cleanup (rewrite `push_interface_*.iron` tests now that mono collapse works, removing the 2-element literal workarounds where safe), and a negative test for the push-wrong-type case.

**Out of scope (belongs to other phases):**
- SoA + fusion composition (Stor type mismatch) → Phase 57
- `binary_tree_diameter` benchmark gap → Phase 58
- Interface arrays as function parameters (IFACE-PARAM-01 candidate, surfaced during Phase 55.1) → future phase
- Cross-statement type inference → outside Iron's explicit-type model

</domain>

<decisions>
## Implementation Decisions

### Fix scope breadth
- **Broad scope** — primary fix + narrowing audit + Phase 55 workaround cleanup + negative test. All four sub-concerns ship in Phase 56:
  1. **Primary fix:** Emit `IRON_LIST_DECL/IRON_LIST_IMPL` for every concrete object type that mono collapse touches. This is the single code change that unblocks both symptom paths.
  2. **Narrowing semantics audit:** Verify that `var shapes = [Circle(1)]; shapes.push(Square(2))` produces a CLEAN Iron type error from `typecheck.c` (not a C codegen blowup). After the primary fix, the codegen path succeeds for `Circle`-typed pushes but must reject `Square`. The type checker already rejects it today, but the ERROR currently surfaces as the same undeclared-symbol codegen error because the list type was missing. After the fix, the error should come from the type checker BEFORE codegen runs. Test verifies the error source and message quality.
  3. **Phase 55 workaround cleanup:** Rewrite `tests/integration/push_interface_*.iron` tests that currently use 2-element initial literals `[Circle(x), Square(y)]` as a workaround. Where the test is about interface split collection semantics (not mono), change to use an explicit annotation `var shapes: [Shape] = [Circle(1), Square(2)]` so the typecheck keeps them as split collections even though they're 2-element. Leaves tests that genuinely need mono (e.g., single-implementor tests) as explicit mono cases. Planner audits each test and decides on a case-by-case basis.
  4. **Negative test:** New test file verifying `.push(WrongType)` on a narrowed mono collection raises a clean Iron type error with a helpful message (e.g., "cannot push Iron_Square to [Circle]"). Documents the expected semantics: single-element mono literal stays mono; to push mixed types, annotate the var as `: [Interface]`.

### Decl emission strategy
- **Pre-scan in emit_structs.c** — mirror the existing split collection type emission pattern. Phase 52 extracted struct emission into `src/lir/emit_structs.c`; Phase 56 adds a new pass there that iterates `ctx->monomorphic_collections`, collects the concrete object type names, and emits `IRON_LIST_DECL(Iron_<Type>, Iron_<Type>)` + `IRON_LIST_IMPL(Iron_<Type>, Iron_<Type>)` at the top of the generated C file, before any function body.
- **Emission order:** decls before function bodies, same section as existing type declarations. Use the existing `emit_structs.c` pre-scan ordering.
- **Duplicate-emission protection:** stb_ds hash set keyed by concrete type name (e.g., `Iron_Circle`). Before emitting `IRON_LIST_DECL` for a type, check the set. If absent, emit and insert. Matches the Phase 49 specialization registry pattern.
- **Dedup scope:** per-compilation-unit (one generated C file). No cross-file dedup needed since Iron compiles to a single C file per program.
- **What triggers emission:** any concrete object type that appears as the value of `ctx->monomorphic_collections` or any CALL result that feeds a mono-collapsed collection (via the Phase 53 interprocedural propagation path in `coll_types`).

### Method/chain coverage matrix
**Full coverage for mono-collapsed collections** — all methods and all chain combinations work. The decl-emission fix should make all of these work automatically because they all use the same `Iron_List_<Type>_<method>` codegen path that the fix enables.

- **Methods in scope:** `.map`, `.filter`, `.reduce`, `.sum`, `.forEach`, `.push`, `.len`, `.pop`, `.get`, `.set` — all 10 methods. This is the same set Phase 55 covered for split collections, ensuring parity between mono and split paths.
- **Chain combinations tested:**
  - `.map()` alone (ROADMAP SC1 — non-negotiable)
  - `.map().filter()` — 2-op chain with intermediate result typing
  - `.map().filter().reduce()` or `.map().filter().sum()` — 3-op chain with terminal accumulator
  - `.map().filter().reduce()` **with fusion enabled** — verifies Phase 49's fusion engine handles the plain-typed-array path after mono collapse (see Fusion interaction below)
- **Parity sweep:** one small test per non-chain method (`.push`, `.len`, `.pop`, `.get`, `.set`, `.forEach`) on a mono-collapsed collection, proving each method works.

### Fusion interaction with mono collapse
- **Probe and fix if needed.** Write a `mono_fusion_chain.iron` test with `val circles = [Circle(1), Circle(2), Circle(3)]; val result = circles.map(f).filter(g).reduce(init, h)`. Run it after the decl-emission fix lands.
  - **If it compiles and runs correctly:** great — ship the test as-is and verify via generated C grep that fusion actually happened (see below).
  - **If it fails:** extend the fusion engine (`emit_fusion.c`) to support the plain-typed-array path. Scope flexes to cover the extension in Phase 56 since fusion on mono is a Phase 49 promise that's only become testable after Phase 56's decl fix.
- **Generated C inspection:** for the fusion test, grep the generated C for:
  - `Iron_List_Iron_Circle` struct reference (proves mono collapse + decl emission landed)
  - A single combined loop body containing the map transformation + filter predicate + reduce accumulation inline (proves fusion happened)
  - Absence of multiple sequential loops (would indicate fusion broke)
- This follows Phase 54's convention of stdout match + generated C grep for every integration test.

### Claude's Discretion
- Exact location in `emit_structs.c` for the new pre-scan pass (alongside split collection struct emission or as a separate pass)
- Whether to emit `IRON_LIST_IMPL` alongside `IRON_LIST_DECL` or only `DECL` (the latter would require a separate `IMPL` pass — `DECL` alone may be sufficient if all Iron_List operations are macro-inlined, but likely both are needed)
- How to handle the edge case where the same concrete type is used both as a split collection implementor (via `ctx->split_collection_ids`) AND as a standalone mono collection elsewhere — the dedup set should prevent double-emission but the planner verifies this during implementation
- Which specific Phase 55 tests get rewritten in the cleanup task (case-by-case audit — not every workaround needs removing)
- Exact error message wording for the push-wrong-type case (the gist is locked, the phrasing isn't)
- Whether to ALSO emit decls for the Phase 53 CALL-return-type path (interprocedural mono detection) or just the direct ARRAY_LIT path (may be automatic depending on how `monomorphic_collections` is populated — planner checks)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Bug site and mono collapse mechanics
- `src/lir/emit_c.c:4797-4814` — Phase 49 mono collapse pass. Removes collections from `split_collection_ids`, causing them to fall through to the plain-typed-array path. This is WHERE the fix plugs in (not the fix site itself — the fix lives in emit_structs.c).
- `src/lir/emit_c.c:5281-5530` — Phase 49/53 monomorphic detection scan. Populates `coll_types` hash map with concrete types per collection variable. Also handles escape analysis (lines 5487-5530) and interprocedural CALL result tracking (lines 5449-5465).
- `src/lir/emit_c.c:5372-5530` — The broader mono detection scan. Planner reads this to understand what `ctx->monomorphic_collections` actually contains by the time mono collapse runs.
- `src/runtime/iron_runtime.h:297-372` — `IRON_LIST_DECL` and `IRON_LIST_IMPL` macro definitions. Planner uses these as the template for what to emit.
- `src/runtime/iron_runtime.h:640-645` — Pre-declared primitive list types. Phase 56 extends this set dynamically for concrete object types via emit_structs.c.

### Emission infrastructure
- `src/lir/emit_structs.c` — Phase 52 extracted struct/type emission into this module. Phase 56's new pre-scan pass goes here.
- `src/lir/emit_structs.h` — public API for struct emission. May need a new exported function for the decl-emission pass.
- `src/lir/emit_c.c:2555-2588` — Array literal emission path. Reference for how concrete types are currently referenced in generated C.

### Typecheck reference (for narrowing audit)
- `src/analyzer/typecheck.c:1827-1888` — `IRON_NODE_ARRAY_LIT` case, where array literals are type-resolved. Phase 55.1 added the `check_expr_with_expected` helper here. The narrowing audit verifies that `.push(WrongType)` on a narrowed collection raises a proper type error from this area (not a codegen error from emit_c.c).
- `src/analyzer/typecheck.c` — search for `.push` method resolution and the narrowing logic for single-element literals. Planner locates during implementation.

### Prior phase context
- `.planning/phases/49-loop-fusion-monomorphic-specialization/49-CONTEXT.md` — Phase 49 mono collapse decisions. Specifically §8.3 monomorphic specialization design.
- `.planning/phases/49-loop-fusion-monomorphic-specialization/49-03-SUMMARY.md` — Phase 49 mono collapse implementation summary, including type emission deviations.
- `.planning/phases/53-analysis-improvements/53-03-SUMMARY.md` — Phase 53 interprocedural mono detection (func_return_types → coll_types wiring). Mentions the SplitList parameter limitation (IFACE-PARAM-01 candidate).
- `.planning/phases/54-test-hardening/54-01-SUMMARY.md` and `54-03-SUMMARY.md` — Documented the "mono + .map() chain is known compiler limitation" as the reason single-implementor tests used for-loops. This is the limitation Phase 56 resolves.
- `.planning/phases/55-push-on-interface-arrays/55-01-SUMMARY.md` — Documents the 2-element workaround pattern in `push_interface_*.iron` tests. Planner audits each test for the cleanup task.
- `.planning/phases/55.1-empty-typed-array-literal/55.1-CONTEXT.md` — Pattern reference: how to do a surgical frontend fix via a helper wrapper. Phase 56 is different (emitter, not typecheck) but the "single source of truth" philosophy applies.

### Requirements and phase spec
- `.planning/REQUIREMENTS.md` lines 87-88 — MONO-FIX-01, MONO-FIX-02 requirement statements.
- `.planning/ROADMAP.md` lines 1139-1150 — Phase 56 section with the 5 success criteria.

### Existing mono integration tests (read-only references)
- `tests/integration/mono_single_type_collapse.iron` — Basic mono collapse test (uses for-loop, pre-Phase-56)
- `tests/integration/mono_multi_type_no_collapse.iron` — Multi-type case that should NOT collapse
- `tests/integration/mono_interprocedural.iron` — Phase 53's interprocedural mono test
- `tests/integration/mono_specialization_registry.iron` — Phase 49 specialization registry
- `tests/integration/mono_specialization_heuristic.iron` — Phase 53 heuristic
- Phase 56 should NOT modify these. New tests have `mono_` prefix, new names.

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **`ctx->monomorphic_collections` hash map** — Already populated by Phase 49/53 mono detection scan at `emit_c.c:5281-5530`. Keys are LIR value IDs of collection variables; values track the single concrete type. The Phase 56 emit_structs.c pass iterates this map to know which concrete types need `IRON_LIST_DECL` emission.
- **`IRON_LIST_DECL` + `IRON_LIST_IMPL` macros** (`iron_runtime.h:297-372`) — Complete macro implementations for create, push, get, set, pop, len, free. Phase 56 just needs to instantiate them for concrete object types; no new runtime code.
- **Phase 52 emit_structs.c module** — Clean API for type emission, already handles struct bodies, SoA/AoS arrays, storage structs. New pre-scan pass fits naturally here.
- **stb_ds hash set pattern** — Used throughout emit_c.c/emit_structs.c for dedup (`emitted_type_decls`, `split_collection_ids`, etc.). Phase 56 adds one more: `emitted_mono_list_types`.
- **Phase 55's grep-verifiable test pattern** — Each test pair (.iron + .expected) with both stdout match AND generated C grep assertion. Phase 56 tests follow the same style.
- **Existing mono tests in tests/integration/** — Proof that the rest of the mono pipeline (detection, collapse pass, GET_INDEX lowering) works; Phase 56 fixes only the type decl emission gap.

### Established Patterns
- **Pre-scan passes in emit_structs.c** — Phase 52 refactored the emitter so type-related work happens in a dedicated module with pre-scan passes that run before function body emission. Phase 56 extends this pattern.
- **Interception-block extension** — Phases 47, 55, 55.1 all followed the pattern "find the existing dispatch block, add a new case" for uniform method emission. Phase 56 is a different shape (emitting missing decls, not adding dispatch branches), but follows the same "minimal localized surgery" philosophy.
- **Generated C grep for semantic proof** — Phase 54 established that every integration test should verify both runtime output AND a generated-C grep pattern proving the intended path was taken. Phase 56's mono tests grep for `Iron_List_Iron_Circle` struct reference and absence of SplitList fallback.

### Integration Points
- **emit_structs.c → emit_c.c call chain:** emit_structs.c runs its pre-scan passes before emit_c.c emits function bodies. Phase 56 adds one more pre-scan for mono list decls; emit_c.c's existing plain-typed-array codegen path then "just works" because the types are now declared.
- **Typecheck narrowing logic** — For the narrowing audit sub-task, planner locates the `.push()` method resolution path in `src/analyzer/typecheck.c` and verifies the error emitted when pushing a mismatched concrete type to a narrowed collection.
- **Phase 55 tests** — `tests/integration/push_interface_*.iron` files are rewritten in the cleanup task. Not every file needs changing; planner audits each.

</code_context>

<specifics>
## Specific Ideas

- The `monomorphic_collections` hash map stores LIR value IDs as keys; to get the concrete type NAME for `IRON_LIST_DECL`, the planner traces from the value ID through the LIR value types or looks up the tracked concrete type in the associated `coll_types` map (populated during the Phase 49/53 detection scan).
- The Phase 56 pre-scan pass runs ONCE per compilation unit and produces a sorted list of concrete object type names. Emission is deterministic (alphabetical or discovery order) so generated C diffs are stable.
- For the narrowing audit: Iron currently produces `E0202: type mismatch` for wrong-type pushes BEFORE Phase 56, but the symptom is masked by the codegen blowup because the type error never fires — the codegen path already errored first. After Phase 56's decl fix, the codegen path succeeds for correct pushes and the type error surfaces cleanly for wrong-type pushes. The audit verifies this by writing a test that expects a type error from the frontend, not a C build failure.
- The Phase 55 cleanup is OPPORTUNISTIC — if rewriting `push_interface_collection.iron` to use `var shapes: [Shape] = [Circle(1), Square(2)]` (annotated, non-narrowed) keeps the test semantically equivalent, do it. If the rewrite requires non-trivial changes to the test's assertions, leave the test alone and document why. The goal is "remove known workarounds", not "rewrite every test."
- The ROADMAP SC4 test name is `mono_method_chain.iron`. Phase 56's primary regression test should use this exact name.
- The fusion-on-mono test can be named `mono_fusion_chain.iron` to signal it's a composition test (mono + Phase 49 fusion).

</specifics>

<deferred>
## Deferred Ideas

- **Interface arrays as function parameters (IFACE-PARAM-01 candidate)** — Surfaced during Phase 55.1 when the executor hit `Iron_List_Iron_Shape_len` undeclared for a `[Shape]` parameter. Similar family of bugs (codegen path references a list type that wasn't declared) but in a different callsite (function parameter, not mono collapse). Not in Phase 56 scope. Planner notes it but does not act. Candidate for Phase 56.1 or a new phase before 57.
- **Cross-compilation-unit dedup** — If Iron ever supports multi-file compilation, the dedup set would need to become cross-file. Not relevant now (Iron compiles to a single C file per program).
- **Profile-guided mono detection** — Skip mono collapse for very short collections where the overhead of specialized code isn't worth the savings. Not Phase 56 scope; belongs with other profile-guided optimizations.
- **Specialization-registry merge** — Phase 49's specialization registry (for generic function instantiation) and Phase 56's `emitted_mono_list_types` set could theoretically share infrastructure. Defer until there's a clear benefit — YAGNI for Phase 56.
- **Alternative narrowing semantics** — One could argue `var shapes = [Circle(1)]` should auto-widen to `[Shape]` when an interface is in scope. Rejected for Phase 56 (explicit-type model is intentional). If the user wants polymorphism, they annotate `var shapes: [Shape] = [Circle(1)]`.

</deferred>

---

*Phase: 56-monomorphic-method-chain*
*Context gathered: 2026-04-09*
