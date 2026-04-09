# Phase 57: SoA + Fusion Composition - Context

**Gathered:** 2026-04-09
**Status:** Ready for planning

<domain>
## Phase Boundary

Fix the SoA layout + fusion `Stor` type reference mismatch documented in Phase 54's verification. Phase 48 introduced SoA layout selection for collections where fewer than 50% of fields are accessed via the interface; when SoA is active, the split collection's per-type sub-arrays store `Iron_<Type>_Stor` structs (a reduced variant containing only the fields that survived dead-field elimination, potentially with value-range-compressed types). Phase 49's fusion engine then always emits `Iron_<Iface>_from_<Type>(items[_fi])` to wrap each per-type element into the interface tagged union before running the closure body — but the `_from_<Type>` constructor expects the full `Iron_<Type>` struct, not the `Iron_<Type>_Stor` variant. Clang rejects the mismatch with `passing 'Iron_<Type>_Stor' to parameter of incompatible type 'Iron_<Type>'`.

The bug site is explicit and honest: `src/lir/emit_fusion.c:319` reads `(void)is_soa;  /* SoA-aware access deferred -- use AoS for now */`. The developer who wrote the fusion engine detected SoA at line 313-318 but deliberately deferred the fix. Phase 57 is that deferred fix.

**Reproducible minimal case:**

```iron
interface Entity { func get_x() -> Int }
object Particle impl Entity { var x: Int; var y: Int; var z: Int; var mass: Int }
object Bullet impl Entity { var x: Int; var y: Int; var z: Int; var speed: Int }
func Particle.get_x() -> Int { return self.x }
func Bullet.get_x() -> Int { return self.x }
func main() {
  val entities = [Particle(10, 20, 30, 100), Bullet(5, 15, 25, 50), ...]
  val sum_x = entities.map(func(e: Entity) -> Int { return e.get_x() }).sum()
  println("{sum_x}")
}
```

Generates: `Iron_Entity _fuse_elem = Iron_Entity_from_Bullet(_v21.bullet_items[_fi]);` — compile error. Only 1 of 4 fields (`x`) is accessed through the interface, so Phase 48 picks SoA for both `Particle` and `Bullet`, and dead-field elimination removes `y/z/mass` from `Particle_Stor` and `y/z/speed` from `Bullet_Stor`.

**Out of scope (belongs to other phases):**
- Per-type closure specialization that operates directly on Stor types without reconstructing the full struct — deferred optimization, would preserve SoA performance end-to-end but requires LIR-level closure body rewriting. Not Phase 57 scope.
- `binary_tree_diameter` benchmark gap → Phase 58
- IFACE-PARAM-01 (interface arrays as function parameters) → surfaced in Phase 55.1, still deferred
- Other interactions between fusion and non-Phase-48 layout features

</domain>

<decisions>
## Implementation Decisions

### Fix approach: `_from_<Type>_Stor` helper
- **Emit a parallel constructor `Iron_<Iface>_from_<Type>_Stor(Iron_<Type>_Stor val)`** alongside the existing `Iron_<Iface>_from_<Type>(Iron_<Type> val)`. The new helper expands the reduced Stor variant into the full `Iron_<Iface>` tagged union by:
  1. Copying every surviving field from the Stor into the corresponding field of the full `Iron_<Type>` in the tagged union's `data.<Type>` slot.
  2. Zero-initializing dead fields (safe because dead-field elimination already proved those fields are never read via the interface).
  3. **Auto-widening** any value-range-compressed field during the copy. E.g. if Stor has `uint8_t x` (Phase 50 VRC narrowed), the full struct's `int64_t x` gets `full.x = (int64_t)stor.x;`. The helper handles both concerns (dead field zero-init AND compression widening) in a single code path.
- **`src/lir/emit_fusion.c:329-332` is updated** to branch on `is_soa`: when `is_soa == true`, emit `Iron_%s_from_%s_Stor(...)` instead of `Iron_%s_from_%s(...)`. The existing `(void)is_soa;` marker at line 319 is removed.
- **Rationale:** smallest correct change that restores SoA + fusion semantics without rewriting fusion closure bodies. Loses some theoretical SoA performance benefit (the fused loop reconstructs a full `Iron_<Type>` per iteration instead of accessing per-field arrays directly), but preserves correctness end-to-end. A future optimization pass can add per-type closure specialization to reclaim that benefit; Phase 57 is explicitly a correctness fix, not a performance fix.

### Helper generation trigger
- **Pre-scan all SoA-layout implementors of interfaces used in fusion.** For each interface that appears in any fusion chain, iterate its alive implementors. For each implementor with SoA layout (check `ctx->soa_types` keyed by `<iface>:<type>`), emit the `_from_<Type>_Stor` helper alongside the existing `_from_<Type>` constructor in `emit_structs.c`'s tagged-union emission section.
- **Single source of truth:** the existing emission loop that produces `_from_<Type>` for all implementors of all interfaces. Add a sibling emission after the existing call that checks SoA status and, if SoA, emits the `_from_<Type>_Stor` variant. No on-demand / just-in-time / fusion-callsite-driven generation.
- **Dedup:** the existing tagged-union emission already visits each (interface, implementor) pair once. No new dedup set needed — if the existing emission avoids duplicates, the new sibling will too.
- **Always emit for all SoA implementors, not just the ones currently used in a fused chain.** This keeps the emission pattern uniform with `_from_<Type>` (which is always emitted for all implementors regardless of usage) and avoids coupling emit_structs.c to fusion-chain detection.

### Scope: both compose_soa_fusion and compose_mega restoration
- **Restore both tests.** Phase 54 documented the same bug forced removal of fusion chains from both `tests/integration/compose_soa_fusion.iron` (the primary regression) AND `tests/integration/compose_mega.iron` (the mega composition test). Phase 57 restores both to exercise the fix fully.
- **compose_soa_fusion.iron:** rewrite the body from the current for-loop workaround (`for e in entities { sum_x = sum_x + e.get_x() }`) to the fusion form (`val sum_x = entities.map(...).sum()`). The `.expected` output (`26\nsoa_done\n`) stays the same since the computation is equivalent. ROADMAP SC2 explicitly targets this test.
- **compose_mega.iron:** restore the fusion chain portion that was removed during Phase 54's Plan 03. Planner reads the current state of the file to see what was removed and reconstructs the fused chain alongside the other composition elements (SoA + dead field + compression + arena + mono).

### Test matrix (4 regression/adjacent tests in addition to the 2 restorations)
- **`soa_fusion_map_sum.iron`** — minimum primary regression. 2 implementors (Particle, Bullet), fields accessed trigger SoA, `.map(...).sum()` chain. Essentially the minimal case from the Phase 57 reproduction above. Covers ROADMAP SC1/SC2 paths with a tighter scope than compose_soa_fusion.
- **`soa_fusion_dead_field.iron`** — explicit dead-field-elimination test. An implementor has 5+ fields, only 1 is accessed, verify the helper's zero-init path works. Covers ROADMAP SC5 "SoA + fusion + dead field".
- **`soa_fusion_compressed.iron`** — value-range-compressed fields exercised. One implementor has an `Int` field with `@range 0..99` (or the relevant Iron annotation from Phase 50) so the Stor stores `uint8_t x` but the full `Iron_<Type>` has `int64_t x`. Helper auto-widens. Grep generated C for both `uint8_t` (in Stor) and the widening cast `(int64_t)` in the helper body. Covers ROADMAP SC5 "SoA + fusion + compression".
- **`soa_fusion_many_types.iron`** — split collection with 3+ SoA-layout implementors in the same collection. Proves the helper emission loop runs for every SoA implementor correctly. Covers ROADMAP SC5 "SoA + fusion on split with many types".

Plus the 2 restored tests (`compose_soa_fusion.iron`, `compose_mega.iron`). **Total: 6 test files touched** (4 new + 2 modified).

### Generated C verification (Phase 54 convention)
Each test verifies both stdout match AND a grep of the generated C:
- `soa_fusion_map_sum`: grep for `Iron_Entity_from_Bullet_Stor(` AND absence of `Iron_Entity_from_Bullet(_v<N>.bullet_items` (proves the SoA path is taken, not a mixed emission)
- `soa_fusion_dead_field`: grep for `.x = stor.x;` AND `.y = 0;` (or equivalent dead-field zero-init patterns)
- `soa_fusion_compressed`: grep for `(int64_t)stor.` widening cast in the emitted `_from_<Type>_Stor` helper body
- `soa_fusion_many_types`: grep for each of the 3+ `_from_<Type>_Stor(` helper calls

### Claude's Discretion
- Exact wording of the emitted `_from_<Type>_Stor` helper body (the structure is locked, brace style and variable naming follow existing emit_structs.c conventions)
- How to detect which fields are alive vs dead vs compressed in the helper emission (planner checks the existing Phase 48 dead-field analysis metadata and Phase 50 VRC metadata — both should be accessible via `ctx` fields)
- Whether to place the new helper emission in an existing function in emit_structs.c or add a new helper like `emit_tagged_union_constructor_stor` — planner matches the file's existing structure
- Exact annotation syntax for the compressed-field test case (Phase 50's `@range` annotation or equivalent — planner reads existing VRC tests to match)
- Where in the fusion emission flow to branch on `is_soa` — the current `(void)is_soa;` marker gives the exact site, but the planner may inline the branch at lines 329-332 directly or refactor slightly for readability

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Bug site and fix location
- `src/lir/emit_fusion.c:305-390` — the per-type split collection fusion loop emission. Contains the SoA detection at lines 313-318, the deliberate `(void)is_soa;  /* SoA-aware access deferred -- use AoS for now */` marker at line 319, and the `_from_<Type>` call at lines 329-332 that needs to branch on `is_soa`.
- `src/lir/emit_fusion.c:370-383` — fusion terminal push. Same per-type pattern; same fix pattern applies if it also hits SoA issues (unlikely — the push path uses direct field writes, not tagged-union wrapping, so it should be unaffected).
- `src/lir/emit_structs.c` — tagged-union constructor emission. The new `_from_<Type>_Stor` sibling lives here. Planner finds the existing `_from_<Type>` emission loop to locate the insertion point.

### Phase 54's documented Phase 57 target
- `.planning/phases/54-test-hardening/54-VERIFICATION.md:122` — "SoA + fusion interaction bug: Fused loops on SoA-layout split collections pass `Iron_*_Stor` types to `Iron_*_from_*()` functions expecting full `Iron_*` types, causing C compilation failure. Tests adapted to for-loop. Affects: `compose_soa_fusion.iron`, `compose_mega.iron` (fusion chain portion)."
- `.planning/phases/54-test-hardening/54-03-PLAN.md` lines 100-260 — the original Phase 54 plan showing what the compose_soa_fusion.iron test SHOULD do (the fusion chain form) before it was adapted to for-loop.

### Prior phase context
- `.planning/phases/48-layout-optimizations/48-CONTEXT.md` — Phase 48 SoA selection rules, dead field elimination
- `.planning/phases/49-loop-fusion-monomorphic-specialization/49-CONTEXT.md` — Phase 49 fusion engine design, `_from_<Type>` wrapping pattern
- `.planning/phases/50-value-range-compression-arena-allocation/50-CONTEXT.md` — Phase 50 VRC, narrowed types in storage structs
- `.planning/phases/52-emitter-refactoring/52-CONTEXT.md` — Phase 52 emitter decomposition; explains why the fix site lives in emit_structs.c (tagged-union constructors) and emit_fusion.c (fusion emission)
- `.planning/phases/54-test-hardening/54-03-SUMMARY.md` — Phase 54 composition test results, documenting which tests used workarounds and why
- `.planning/phases/56-monomorphic-method-chain/56-CONTEXT.md` and `56-01-SUMMARY.md` — Phase 56 pattern reference: how to fix a fusion/emit interaction bug via a localized emit_*.c change plus adjacent test restoration

### Requirements and phase spec
- `.planning/REQUIREMENTS.md` lines 90-91 — SOA-FIX-01, SOA-FIX-02 requirement statements
- `.planning/ROADMAP.md` lines 1152-1163 — Phase 57 section with 5 success criteria

### Test infrastructure
- `tests/integration/compose_soa_fusion.iron` — current state (for-loop workaround). To be rewritten.
- `tests/integration/compose_soa_fusion.expected` — expected output `26\nsoa_done\n`. Stays the same.
- `tests/integration/compose_mega.iron` — current state (fusion portion removed). To be restored.
- `tests/run_tests.sh` — test runner; auto-discovers new `soa_fusion_*.iron` files.

### Read-only references for the helper implementation
- `src/runtime/iron_runtime.h` — how full object types and tagged unions are declared (for reference, not modification)
- Existing `Iron_<Iface>_from_<Type>` emissions in generated C (can be inspected by running an existing test in debug mode) — template for the new `_from_<Type>_Stor` sibling

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **`ctx->soa_types` hash map** — Phase 48 populated this with SoA status keyed by `<iface>:<type>`. Phase 57 checks it in two places: (1) emit_structs.c during tagged-union constructor emission to decide whether to emit the `_Stor` sibling, and (2) emit_fusion.c at the existing line 313-318 SoA detection (already in place, just needs to be used instead of `(void)is_soa;`).
- **Existing `_from_<Type>` emission loop** in emit_structs.c — the new `_from_<Type>_Stor` is a sibling, emitted from the same loop, for each (iface, impl) pair where the impl has SoA layout.
- **Phase 48 dead-field metadata** — tells the helper which fields survived dead-field elimination. The helper's expansion uses this to decide which fields get copied from Stor vs zero-initialized.
- **Phase 50 VRC metadata** — tells the helper which fields are compressed and their narrowed types. The helper's expansion casts them back to the canonical full type during copy.
- **`is_soa` detection at emit_fusion.c:313-318** — already wired, just needs to be honored instead of discarded.

### Established Patterns
- **Sibling constructor emission** — if the existing loop emits `_from_<Type>`, adding a sibling `_from_<Type>_Stor` right after it is a natural extension. No new emission pass needed; extend the existing one.
- **Phase 54 test convention** — integration tests use `.iron` + `.expected` pairs, verify via stdout match AND generated C grep. Phase 57 tests follow this.
- **Localized fusion emitter edit** — Phases 55/56 established that fusion-related fixes live in single emit_*.c branches with minimal surface area. Phase 57 follows the same pattern: one branch in emit_fusion.c:319-332 + one sibling emission in emit_structs.c.
- **Phase 56's probe-and-verify pattern** — write the failing test first, land the fix, verify via both runtime and generated C. Phase 57 does the same.

### Integration Points
- **emit_structs.c → emit_fusion.c:** the new `_from_<Type>_Stor` helpers are emitted before function bodies (in the tagged-union section), so they exist by the time emit_fusion.c's per-type loop runs. No ordering changes needed.
- **Phase 48 layout analysis → emit_structs.c:** the helper generation reads Phase 48's dead-field metadata per (iface, impl) pair to know which fields to copy vs zero-init.
- **Phase 50 VRC → emit_structs.c:** the helper generation reads Phase 50's compressed-field metadata to know which fields need widening casts.

### What NOT to touch
- Phase 49's overall fusion chain detection. Only the per-type loop body emission changes, not the chain recognition.
- `_from_<Type>` (the existing full-type constructor). It stays unchanged; the new `_from_<Type>_Stor` is a sibling, not a replacement.
- The fusion terminal push at emit_fusion.c:370-383 — it uses direct field writes, not tagged-union wrapping, so it shouldn't hit the same issue. Planner verifies during implementation but should not preemptively modify it.

</code_context>

<specifics>
## Specific Ideas

- The helper body for a 4-field Particle (x surviving, y/z/mass dead) looks like:
  ```c
  static inline Iron_Entity Iron_Entity_from_Particle_Stor(Iron_Particle_Stor val) {
      Iron_Entity u;
      u.tag = _TAG_Particle;
      u.data.Particle.x = val.x;
      u.data.Particle.y = 0;
      u.data.Particle.z = 0;
      u.data.Particle.mass = 0;
      return u;
  }
  ```
  Dead fields are zero. Surviving fields are copied. Simple and correct.
- For a compressed field (e.g., Phase 50 narrowed `x: uint8_t` in the Stor when Iron source declares it as `x: Int`):
  ```c
  u.data.Particle.x = (int64_t)val.x;  // widening cast
  ```
- The `emit_fusion.c:329-332` change is minimal:
  ```c
  // BEFORE:
  iron_strbuf_appendf(sb, "%s _fuse_elem = %s_from_%s(",
      sp_iface, sp_iface, impl->type_name);
  // AFTER:
  const char *ctor_suffix = is_soa ? "_Stor" : "";
  iron_strbuf_appendf(sb, "%s _fuse_elem = %s_from_%s%s(",
      sp_iface, sp_iface, impl->type_name, ctor_suffix);
  // AND remove the (void)is_soa; marker at line 319
  ```
- After Phase 57 lands, the Phase 54 VERIFICATION.md entry at line 122 ("SoA + fusion interaction bug") can be updated to reference Phase 57 as the resolution. Planner notes this as a documentation cleanup but does not require it as a task.
- compose_soa_fusion.iron's existing `.expected` output (`26\nsoa_done\n`) should stay the same after the rewrite — the computation is equivalent between the for-loop and the fused `.map().sum()` form. If the output changes, something is wrong with the fusion path.

</specifics>

<deferred>
## Deferred Ideas

- **Per-type closure specialization on Stor types** — the "real" SoA-preserving fix. Specialize the fused closure body per concrete Stor type so it operates directly on `Iron_<Type>_Stor` fields instead of reconstructing the full `Iron_<Type>`. Requires LIR-level closure body rewriting. Would preserve the SoA cache locality benefit end-to-end. Candidate for a future performance phase; Phase 57 explicitly takes the correctness-first helper approach instead.
- **Direct SoA per-field array access in fusion** — even more aggressive than per-type closure specialization. The fused loop would read `particle_x[_fi]`, `particle_y[_fi]` directly instead of any struct-shaped access. Requires closure body analysis to identify which fields are actually used. Future optimization.
- **Remove `_from_<Type>` (the full-type constructor) entirely if all callsites are SoA-aware** — future cleanup, not Phase 57 scope. The full-type constructor is still needed for AoS paths.
- **Updating Phase 54 VERIFICATION.md line 122 to reference Phase 57 as the fix** — documentation cleanup, not required for Phase 57 to ship but worth doing.
- **IFACE-PARAM-01** (interface arrays as function parameters) — still deferred, still unrelated.

</deferred>

---

*Phase: 57-soa-fusion-composition*
*Context gathered: 2026-04-09*
