# Iron Diagnostic Code Registry

Authoritative table of all diagnostic codes emitted by `iron_compiler` and
surfaced through both `ironc` (CLI stderr) and `ironls` (LSP
`publishDiagnostics`). Both binaries link the same compiler frontend, so
every code listed here flows identically through the two surfaces — that
is the CORE-22 invariant.

Each row carries a Quickfix-Target column documenting the intended LSP
code-action wiring for Phase 34 (LSP-06). Phases 17 onward must add a row
here whenever a new code is allocated.

## Code-range conventions

- **Lexer errors:** 1–99 (`IRON_ERR_*`)
- **Parser errors:** 101–199 (`IRON_ERR_*`)
- **Semantic errors:** 200–299 (`IRON_ERR_*`); 290–293 reserved (Phase 8); 320–321 reserved (Phase 9)
- **LIR verifier errors:** 300–399 (`IRON_ERR_LIR_*`)
- **Lowering errors:** 400–499 (`IRON_ERR_*`)
- **HIR verifier errors:** 500–599 (`IRON_ERR_*`)
- **Warnings:** 600+ (`IRON_WARN_*`)
- **Web-target LIR errors:** 700–799 (`IRON_ERR_WEB_*`)

LSP wire format: codes are emitted as `E<NNN>` with three-digit zero-padded
numeric tail (e.g. `E0176`, `E0265`, `E0613`). Consumers comparing diagnostic
codes should accept either the bare integer or the `E<NNN>` string.

## Phase 17 — `val`/`var` field discipline (§5)

| Code | Symbol                       | Message (locked substring)                                       | Hint                                                                                | Quickfix-Target (Phase 34 LSP-06)                                | Phase | Spec § |
|-----:|------------------------------|------------------------------------------------------------------|-------------------------------------------------------------------------------------|------------------------------------------------------------------|------:|-------:|
| 176  | `IRON_ERR_MISSING_VAL_VAR`   | `must specify val or var`                                        | `insert 'val' for an immutable binding (or 'var' to allow reassignment)`            | Insert `val ` (preferred) or `var ` before the binding/field name | 17    | §5.1 + §5.2 |
| 265  | `IRON_ERR_VAL_FIELD_REASSIGN`| `cannot reassign 'val' field '<name>' after initialization`      | `declare field as 'var' to allow reassignment after init`                           | Replace `val ` with `var ` on the offending field declaration     | 17    | §5.2 + §5.3 |
| 613  | `IRON_WARN_UNUSED_VAR`       | `var binding never reassigned; declare as 'val'`                 | `change 'var' to 'val' for an immutable binding`                                    | Replace `var ` with `val ` on the binding (3-char keyword span)   | 17    | §5.5 |
| 614  | `IRON_WARN_UNUSED_VAR_PARAM` | `var parameter never mutated; remove 'var' modifier`             | `drop the 'var' modifier - parameters default to read-only`                         | Delete `var ` from the parameter declaration (3-char keyword span) | 17    | §5.5 |

### Notes

- Codes 176 and 265 are mutually exclusive sites: 176 fires at parser-time
  when val/var is missing entirely (field decl) or at resolver-time when an
  undefined IDENT is the LHS of an assign-stmt (local binding); 265 fires
  at analyzer-time when a non-pub val field is written outside `init`. The
  pre-existing code `IRON_ERR_VAL_REASSIGN=203` is intentionally retained
  for the pub-val branch so storage-class-distinct quickfix wording can
  route on three separate codes.
- Code 176 has a dual emission site by design (per Phase 17-01 CONTEXT.md
  amendment): `src/parser/parser.c:3702-3717` field-decl loop (VAL-02) and
  `src/analyzer/resolve.c` `emit_undefined` branch when
  `ResolveCtx.is_assign_lhs` is true (VAL-01). One code, two sites, single
  Phase 34 LSP-06 quickfix routing entry.
- Codes 613 and 614 anchor on the `var` keyword span (3 chars) rather than
  the binding name span — this gives Phase 34 LSP-06 a stable replacement
  target for the quickfix `TextEdit`.
- VAL-04 (var field reassignability) is deliberately code-less: it is the
  absence of an error, not the presence of one. The new VAL-03 branch in
  `src/analyzer/typecheck.c` is guarded by `!ff->is_var && !ff->is_pub` so
  var fields continue to compile cleanly.

## Phase 18 — Parameter modifier system (§5.3)

| Code | Symbol                              | Message (locked substring)                                          | Hint                                                                                | Quickfix-Target (Phase 34 LSP-06)                                          | Phase | Spec § |
|-----:|-------------------------------------|---------------------------------------------------------------------|-------------------------------------------------------------------------------------|----------------------------------------------------------------------------|------:|-------:|
| 266  | `IRON_ERR_PARM_READ_ONLY`           | `cannot mutate read-only parameter '<name>'`                        | `add 'var' modifier to grant in-body mutation: 'var <name>: T'`                     | Insert `var ` before the parameter name in the function declaration         | 18    | §5.3 |
| 267  | `IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT`  | `cannot pass read-only argument to 'var' parameter '<name>'`        | `make the argument source mutable (declare as 'var')`                               | Change argument-source binding from `val ` to `var ` (cross-edit at decl)   | 18    | §5.3 |

### Notes

- Code 266 (`IRON_ERR_PARM_READ_ONLY`) supersedes the legacy
  `IRON_ERR_VAL_REASSIGN=203` and `IRON_ERR_MUT_FIELD_IMMUT_RECV=234`
  paths *specifically* for the `IRON_SYM_PARAM`-rooted subset of
  mutations (direct rebind, compound assign, and field-write through
  the parameter binding). The legacy codes still fire for val-local
  rebinds (203) and non-param immutable-receiver field-writes (234).
  Mutual exclusion is enforced in `src/analyzer/typecheck.c` at the
  `IRON_NODE_ASSIGN` handler via a `sym_kind == IRON_SYM_PARAM`
  branch with else-fallthrough; Plan 18-01 unit test
  `test_parm_01_no_double_emit_direct` locks `COUNT(266)==1 AND
  COUNT(203)==0` against future drift.
- Code 267 (`IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT`) fires at the call
  site (argument expression span), distinct from code 266 which
  fires inside the called function's body. This duality maps to two
  separate Phase 34 LSP-06 quickfix actions: code 266 inserts `var `
  at the parameter declaration; code 267 changes the *caller-side*
  argument source's declaration from `val` to `var`. Plan 18-02
  ships the call-site check at both `IRON_NODE_CALL`
  (typecheck.c:2249-2319) and `IRON_NODE_METHOD_CALL`
  (typecheck.c:2753-2783) handlers via the recursive
  `arg_source_is_mutable` helper.
- **Phase 18 deviation from CONTEXT.md "reuse 265 with readonly hint"
  decision:** `IRON_ERR_READONLY_WRITE_SELF=238` (Phase 84) is
  RETAINED as the dedicated diagnostic code for readonly-method
  self-field-write violations. CONTEXT.md originally proposed
  re-routing readonly-self-write to `IRON_ERR_VAL_FIELD_REASSIGN=265`
  with a hint-string variation. RESEARCH.md Open Question #3
  recommended keeping 238 dedicated because (a) 265 carries
  storage-class semantics ("val field cannot be reassigned"), (b)
  238 carries tier-class semantics ("readonly method cannot mutate
  self"), and (c) Phase 34 LSP-06 emits two distinct quickfixes —
  *change `val` to `var` on the field* (265) versus *drop `readonly`
  modifier or remove the self.field write* (238). Plan 18-02
  honored the RESEARCH recommendation; the lock test
  `test_parm_04_amendment_e238_dedicated` asserts
  `COUNT(IRON_ERR_READONLY_WRITE_SELF) >= 1` against any future drift
  back to the original "reuse 265" routing.
- Hint strings for both 266 and 267 are *conservatively* worded — they
  do NOT mention the `*var T` checked-pointer form. Pointer-typed
  parameters are Phase 20's territory (PTR-*); when `*var T`
  parameters ship, the Phase 20 author may amend these hints to
  read e.g. `"...or pass a *var pointer"`. The current wording
  `"add 'var' modifier..."` and `"make the argument source mutable
  (declare as 'var')"` are deliberately stable so Phase 34 LSP-06's
  prefix-matching quickfix routing keeps working without a
  hint-string version pin.

## Phase 20 — Checked pointer types (§4.2)

| Code | Symbol                                | Message (locked substring)                                                            | Hint                                                                                              | Quickfix-Target (Phase 34 LSP-06)                                                       | Phase | Spec § |
|-----:|---------------------------------------|---------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------|------:|-------:|
| 268  | `IRON_ERR_PTR_NO_ARITH`               | `no pointer arithmetic in checked regime`                                              | `use Ptr.offset on *unchecked T`                                                                  | Rewrite as `Ptr.offset` call on `*unchecked T` (requires Phase 25 *unchecked T)         | 20    | §4.2 |
| 269  | `IRON_ERR_PTR_CAST_SIZE_MISMATCH`     | `Ptr.cast pointee size mismatch`                                                       | `use *unchecked T (Phase 25) for arbitrary pointer casts`                                         | Wrap target in `Box[T]` and `unwrap()` to obtain `*unchecked T` (Phase 25)              | 20    | §4.2 |
| 270  | `IRON_ERR_PTR_AMP_ON_RVALUE`          | `cannot take address of rvalue (literal or temporary)`                                 | `bind to a local first (val x = ...; sink(x))`                                                    | Insert `val x = <expr>; ... &x` rewrite at the call site                                | 20    | §4.2 |
| 271  | `IRON_ERR_PTR_ESCAPE_STACK_REF`       | `cannot return reference to stack-local variable`                                      | `allocate on the heap (Phase 21 'heap T(...)') or return the value by-copy`                       | Replace `return &local` with `return local` (by-value) OR with `heap T(...)` (Phase 21) | 20    | §4.2 |
| 272  | `IRON_ERR_PTR_NULL_DEREF`             | `cannot assign null to non-nullable pointer type` (compile-time) / `null deref` (runtime) | `use '?*T' for the nullable variant; narrow with 'if p != null { ... }' before dereference`     | Change type from `*T` to `?*T`; insert `if p != null { ... }` narrowing block          | 20    | §4.2 |

### Notes

- **Code 267 (`IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT`, Phase 18) is REUSED by Phase 20**
  at the call-site auto-address path: when a `*var T` parameter receives an
  auto-addressed argument whose source binding is `val`, code 267 fires (same
  code as Phase 18's PARM-03 path; same caller-side quickfix target). The hint
  string was conservatively worded in Phase 18 to NOT mention `*var` (Phase 18
  Notes line ~105); Phase 20 author may amend the hint in a future plan if
  Phase 34 LSP-06 needs the differentiation, but the current wording remains
  stable.
- **Code 272 (`IRON_ERR_PTR_NULL_DEREF`) is DUAL-USE.** Compile-time emission:
  `val p: *T = null` (binding mismatch) — fires at the binding-init site with
  substring "cannot assign null to non-nullable pointer type". Runtime
  emission: the existing `iron_panic_stale_pointer` path with `hdr=NULL`
  convention (Phase 19) reports null-deref panics from runtime `?*T` access
  without narrowing. Both surface as code 272 to the LSP/CLI; the Phase 34
  LSP-06 quickfix is the same in both cases (insert `if p != null { ... }`
  narrowing block OR change type to `?*T`).
- **Phase 25 forward-reference.** Codes 268 + 269 explicitly mention
  `*unchecked T` and `Ptr.offset` as the migration path. Phase 25 (Unchecked
  Pointers + Box[T]) ships these. Plan 25-NN should NOT change the wording of
  codes 268/269 — the Phase 25 implementer adds the matching emission paths
  in `*unchecked T` codepaths (`Ptr.offset` is the unchecked-only stdlib
  function).
- **OQ-A locked Plan 20-01 (object-pointer writes via auto-deref `p.field = value`):**
  the hint strings for codes 270 + 272 reference auto-deref (`bind to a local
  first`; `narrow with 'if p != null'`). Primitive-pointer-write
  (`*var Int` writes) is deferred to Phase 25's `Ptr.set` builtin; if user
  code attempts `*p = value` syntax, the parser emits the existing
  `IRON_ERR_UNEXPECTED_TOKEN` (code 101) because `*` in expression-position
  is reserved.
- **OQ-B locked Plan 20-02 (Option C — separate
  `iron_check_stack_pointer_gen` path):** stack-pointer panics fire via
  `iron_panic_stale_stack_pointer` (new Phase 20 helper) with text channel
  `"dangling stack pointer to frame #M"` and JSON channel
  `"panic":"stack_pointer"`. The text format mirrors Phase 19's
  `iron_panic_stale_pointer` but distinguishes by message and JSON
  discriminator. See `docs/dev/POINTER-LAYOUT.md` "Phase 20 surfaces" section
  for the full panic format.
- **OQ-02 RESOLVED (Plan 20-03):** Closures over `*T`/`*var T` are LEGAL
  with lifetime-extension + panic-on-invocation semantics. The closure
  captures the 16B `Iron_FatPtr` by value into its captured-state struct;
  closure-body deref through stale fp panics via `iron_check_pointer_gen`
  (heap-source) or `iron_check_stack_pointer_gen` (stack-source).
  `.planning/PROJECT.md` Key Decisions table has the canonical entry; the
  positive-corpus fixtures
  `tests/integration/v4/4.2-checked-ptr/closure_pointer_capture.iron` +
  `closure_var_pointer_capture.iron` pin the spec semantics on disk.

## Phase 21 — Heap policy + free / leak / defer-free (§4.5–4.6 + §10.4)

| Code | Symbol                              | Message (locked substring)                                             | Hint                                                                                              | Quickfix-Target (Phase 34 LSP-06)                                                                                 | Phase | Spec §        |
|-----:|-------------------------------------|------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------|------:|---------------|
|  273 | `IRON_ERR_HEAP_BAD_POSITION`        | `` `heap` only valid at allocation expression ``                       | `` got `heap` in <type annotation\|binding declaration\|parameter declaration> ``                  | Move `heap` to allocation-expression position (delete from type/binding/param; insert before constructor call)    |    21 | §4.5 / §3.2   |
|  274 | `IRON_ERR_FREE_NOT_BINDING`         | `` `free` target must be a binding name, not an expression ``          | (no hint — message is self-contained)                                                             | Replace expression with the binding name it dereferences (e.g., `free p` instead of `free p.field`)              |    21 | §4.6 / §3.2   |
|  275 | `IRON_ERR_LEAK_NOT_BINDING`         | `` `leak` target must be a binding name, not an expression ``          | (no hint — message is self-contained)                                                             | Replace expression with the binding name it dereferences (e.g., `leak p` instead of `leak p.field`)              |    21 | §4.6 / §3.2   |
|  276 | `IRON_ERR_DEFER_FORM_UNSUPPORTED`   | `` only `defer free <binding>` is supported in v3.0-alpha.1 ``         | `` full `defer` semantics ship in Phase 32 ``                                                    | Replace `defer <expr>` with `defer free <binding>` if cleanup is a heap free; else wait for Phase 32 full defer   |    21 | §10.4         |

### Notes

- **`leak` → DBG-05 forward-contract.** Plan 21-02 added a `bool is_leaked` field on `Iron_Symbol` (set in resolve.c's `IRON_NODE_LEAK` arm when the target identifier resolves to a non-NULL symbol). Phase 31 (DBG-05 forgotten-`free` warning) walks heap-bound symbols in scope and SUPPRESSES the warning when `Iron_Symbol.is_leaked == true`. The flag is the inter-phase consumer contract; Phase 21 ships the producer, Phase 31 ships the consumer.
- **`escape.c` complementarity.** The existing `src/analyzer/escape.c` v1/v2-era classifier emits codes E0212 (`IRON_ERR_FREE_NON_HEAP`), E0213 (`IRON_ERR_LEAK_NON_HEAP`), E0214 (`IRON_ERR_LEAK_RC`). These remain ACTIVE post-Phase-21 and complement the new identifier-only checks (E0274 / E0275). For example: `free p.field` triggers E0274 at typecheck.c (Phase 21 identifier-only check) AND, depending on the resolved type chain, MAY also trigger E0212 at escape.c (legacy non-heap classification). Both diagnostics are valid; the Phase 21 check fires earlier and more precisely. Full unification of escape.c with the Phase 21 identifier-only path is deferred to Phase 31.
- **Phase 24 DROP-01 forward-reference.** The IRON_LIR_FREE codegen in `src/lir/emit_c.c` emits `iron_heap_free(fp)` preceded by a `/* PHASE-24 HOOK: drop call insertion before iron_heap_free */` comment stub. Phase 24 will replace the stub with a destructor invocation when the freed type has a `drop { ... }` block. The fixture `tests/integration/v4/3.2-heap/boundary_destructor_runs.iron` is tagged `@expected-pass-after: phase-24` (Plan 21-01 retag) and stays XFAIL until Phase 24 lands DROP-01.
- **Phase 32 full-defer forward-reference.** v3.0-alpha.1 admits ONLY `defer free <binding>` (DEFER-02 ergonomic idiom). All other defer forms (defer of arbitrary statements, panic-safe defer, drop interaction, multi-form LIFO) ship in Phase 32 (DEFER-01, DEFER-03, DEFER-04). E0276's hint string explicitly forward-references Phase 32.
- **OQ-09 NO-promotion lock.** Policy promotion `heap → rc` is **NOT** supported. The closed policy set is enforced at type level: a programmer wanting reference counting must allocate via `rc T(...)` from the start (Phase 26 ships the `rc` allocation form). PROJECT.md Key Decisions table has the canonical row resolving OQ-09; consistent with POL-11's closed-policy-keyword guard.
- **`auto_free` flag disconnection (Phase 31 forward-reference).** The `IRON_LIR_HEAP_ALLOC.heap_alloc.auto_free` flag (set by escape.c for non-escaping heap allocations) is intentionally NOT connected to automatic `iron_heap_free` emission in Phase 21. Phase 31 (DBG-05 forgotten-`free` warning) will consume this flag to drive the warning. The emit_c.c HEAP_ALLOC site carries a `/* PHASE-31: auto_free connects to DBG-05 forgotten-free warning; do NOT emit iron_heap_free here */` documentation guard.

## Phase 22 — readonly Purity Tightening (§6 + §12 step 6/8)

| Code | Symbol | Message (locked substring) | Hint | Quickfix-Target (Phase 34 LSP-10) | Phase | Spec § |
|-----:|--------|----------------------------|------|-----------------------------------|------:|--------|
| 277 | `IRON_ERR_READONLY_PARAM_MUTATION` | `cannot assign to parameter '...' in readonly method` | `§6: readonly methods may not assign to any parameter` | Drop `readonly` modifier (LSP-10) | 22 (Plan 22-01) | §6 |
| 278 | `IRON_ERR_READONLY_IO` | `cannot call I/O function/method '...' in readonly method` | `§6: readonly methods may not perform I/O` | Drop `readonly` modifier (LSP-10) | 22 (Plan 22-01) | §6 |
| 279 | `IRON_ERR_READONLY_HEAP_ESCAPE` | `cannot allocate 'heap T(...)' in readonly method` | `§6: readonly methods may not allocate heap T(...) or rc T(...)` | Drop `readonly` modifier (LSP-10) | 22 (Plan 22-01) | §6 + §4.5 |
| 280 | `IRON_ERR_READONLY_RETURN_TYPE` | `readonly function/method return type '...' is not readonly-compatible` | `§6: readonly return types: primitives, fixed structs, [T; N], [T; <=N], tuples, T?` | Drop `readonly` modifier (LSP-10) | 22 (Plan 22-02) | §6 + §12 step 8 |
| 281 | `IRON_ERR_READONLY_IFACE_CONFORMANCE` | `interface readonly method '...' implementation '...' must be readonly or pure` | `§6: interface readonly method requires readonly or pure implementation` | Add `readonly` to impl (LSP-10) | 22 (Plan 22-02) | §6 + §13 OQ-05 |

### Notes

1. **§6 hint discipline (READ-09)** — All FIVE readonly diagnostics carry hint substring `§6:` + rule text. The two Phase 84 baseline emissions (`IRON_ERR_READONLY_WRITE_SELF`=238 + `IRON_ERR_READONLY_CALLS_MUTATING`=239) were retrofitted by Plan 22-01 Tasks 3 + 4 to gain §6 hints; codes/messages preserved.

2. **Pitfall 1 double-emit guard** — All Phase 22 body-violation checks are guarded with `ctx->in_readonly_method && !ctx->in_pure_method`. Without the `!ctx->in_pure_method`, a `pure` method (which has both flags true per typecheck.c:4438) would emit BOTH a pure-tier code (240-244) AND a readonly-tier code (277-279) for the same violation. The guard ensures pure-tier owns pure methods; readonly-tier owns non-pure-readonly only. Regression anchor: tests/unit/test_readonly_body.c case `pure_method_io_no_double_emit`.

3. **Pitfall 7 Phase 87 v3_iface_tier_mismatch.iron preservation** — READ-07 changed the diagnostic code for `sig->is_readonly && !sig->is_pure` failures from `IRON_ERR_IFACE_METHOD_TIER_MISMATCH` (257, Phase 87 baseline) to `IRON_ERR_READONLY_IFACE_CONFORMANCE` (281, Phase 22). Pure-sig violations PRESERVE code 257 to keep Phase 87 fixture compatibility. The semantic at typecheck.c (`ok = impl->is_readonly || impl->is_pure;` for readonly sig — `pure >= readonly`) is UNCHANGED.

4. **READ-08 sret RVO contract** — readonly methods/functions returning `[T; N]` (fixed array) or `[T; <=N]` (bounded vector — Phase 23 syntax accepted; semantics tightened in Phase 23) use sret ABI in generated C: `void fn(T_array *_sret, ...args)` with caller-provided slot. emit_c.c `emit_func_use_sret` helper centralizes the decision symmetrically at function-decl and call sites (Pitfall 6). `IronHIR_Func.is_readonly` + `IronLIR_Func.is_readonly` fields propagate from AST through hir_lower.c + hir_to_lir.c. Lambda lifting carries the readonly context via `LiftPending.is_readonly_context` (Plan 22-02 set; Plan 22-03 consume at lift Pass 3 setting `IronHIR_Func.is_readonly`).

5. **OQ-04 closure body-purity + capture-purity** — closures inside a readonly method/func are implicitly readonly (body-purity inheritance via `LiftPending.is_readonly_context` propagation). Captures of `var` bindings (mutable storage) and `*var T` pointers (writable through capture) are REJECTED with code 277 (`IRON_ERR_READONLY_PARAM_MUTATION`) emitted in capture.c after `find_captures` builds the capture set; `readonly_context` propagated AST-side per RESEARCH Pitfall 4 (TypeCtx not available post-typecheck). Captures of `val`, `*T` (read-only pointer), primitives, fixed structs are accepted. Hint substring `§6: closures in readonly...` distinguishes closure-capture from body-mutation.

6. **OQ-05 pure >= readonly** one-way subsumption: pure impl satisfies readonly sig; readonly impl does NOT satisfy pure sig. Formalized at typecheck.c `check_iface_tier_strengthening` with `ok = impl->is_readonly || impl->is_pure;` for readonly sig (Phase 87 baseline; Plan 22-02 only changes diagnostic code on failure). Positive corpus: `pure_satisfies_readonly_iface.iron` validates the subsumption.

7. **Phase 23 forward-reference (BVEC)** — the `[T; <=N]` syntax is accepted in Phase 22's readonly-compatible whitelist (`is_readonly_compatible_type` IRON_TYPE_ARRAY case with `size >= 0`). Phase 23 will land the inline-storage representation. `is_readonly_compatible_type`'s switch has a `/* Phase 23 BVEC: when IRON_TYPE_BVEC lands as a new kind, add explicit case */` future-extension comment per Pitfall 5.

8. **Phase 24 forward-reference (DROP/copy/nocopy)** — readonly-compatible struct walk treats all fields as candidates; Phase 24 may need to refine for nocopy field interaction with readonly returns.

9. **Phase 34 LSP-10 quickfix consumer** — every Phase 22 code carries Quickfix-Target column for Phase 34 LSP-10 quickfix wiring (typically "drop readonly modifier" or "add readonly to impl").

## Phase 23 — Bounded Vector `[T; <=N]` (§3.3 + §12 step 7 + §13 OQ-10)

| Code | Symbol | Message (locked substring) | Hint | Quickfix-Target (Phase 34 LSP-10) | Phase | Spec § |
|-----:|--------|----------------------------|------|-----------------------------------|------:|--------|
| 282 | `IRON_ERR_VEC_STRICT_LENGTH_MISMATCH` | `array literal has N element(s) but '[T; M]' requires exactly M` | `§3.3: [T; N] requires exactly N elements in the initializer literal` | Pad or trim the array literal to exactly N elements (LSP-10) | 23 (Plan 23-01) | §3.3 |
| 283 | `IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN` | `cannot assign bounded vector to strict array or vice versa` | `§3.3: [T; <=N] and [T; N] are disjoint types; Phase 33 ships to_fixed()/to_bounded() conversion helpers` | Suggest `to_fixed()` / `to_bounded()` call (Phase 33 — LSP-10 deferred) | 23 (Plan 23-01) | §3.3 |

### Notes

1. **§3.3 hint discipline** — Both VEC codes carry hint substring `§3.3:` + rule text. Applied consistently with Phase 22's §6-prefix discipline (READ-09). Every hint includes the spec section that mandates the restriction.

2. **types_assignable disjoint-shape early return** — `types_assignable` in `src/analyzer/typecheck.c` (lines 746-806) gains an early-return `false` guard for the bounded ↔ strict cross-assignment case. The guard fires before `iron_type_equals`, ensuring `[T; <=N]` is never silently treated as `[T; N]`. VEC-04 specialization then intercepts the `!types_assignable` block to emit E0283 instead of generic E0202.

3. **`iron_type_equals` + `iron_type_to_string` + `iron_type_make_array` changes** — Plan 23-01 updated three type functions:
   - `iron_type_equals`: IRON_TYPE_ARRAY case now compares `is_bounded` field, making `[T; <=N]` and `[T; N]` structurally inequal.
   - `iron_type_to_string`: emits `[T; <=N]` (with `<=`) for bounded arrays so error messages correctly show the bounded form.
   - `iron_type_make_array`: gains 4th `bool is_bounded` parameter; 11 call sites updated across 5 files.

4. **Pitfalls 1–8 mapped to plans:**
   - Pitfall 1 (is_bounded missing from one resolve_type_annotation branch) — resolved Plan 23-01 (both is_array branches updated).
   - Pitfall 2 (iron_type_equals not updated) — resolved Plan 23-01.
   - Pitfall 3 (emit_type_to_c called before emit_ensure_bvec) — resolved Plan 23-02 (emit_type_to_c calls emit_ensure_bvec before returning struct name).
   - Pitfall 4 (emit_func_use_sret RETURN site) — resolved Plan 23-02 (emit_type_to_c returning correct struct name; `*_sret = bv_struct` assignment correct for both bounded and strict).
   - Pitfall 5 (OQ-10 List[[T;<=N]] — Iron_List_Iron_BVec_T_N_N not pre-declared) — resolved Plan 23-03 (emit_mono_list_decls extension calls emit_ensure_bvec FIRST before List struct emission).
   - Pitfall 6 (iron_type_to_string) — resolved Plan 23-01.
   - Pitfall 7 (var bv: [T; <=N] default init) — resolved Plan 23-02 (ALLOCA emitter emits `= {0}` for bounded vec; init_check.c skips uninit registration for bounded vecs).
   - Pitfall 8 (implicit — LOAD-to-alloca resolution for push) — resolved Plan 23-02 (push intercept detects LOAD in args[0] and resolves to alloca ptr so mutations persist).

5. **OQ-10 closure (Plan 23-03):**
   - `List[[T; <=N]]`: `emit_mono_list_decls` extended to scan ARRAY_LIT instructions for bounded-vector elem types; calls `emit_ensure_bvec` before emitting the list typedef (Pitfall 5 mitigation).
   - `[[T; <=N]; <=M]`: `emit_ensure_bvec` recursive case emits the inner `Iron_BVec_T_N` typedef before the outer `Iron_BVec_Iron_BVec_T_N_M` typedef. Verified end-to-end by `nested_bvec.iron` corpus fixture.
   - Both shapes have v4 corpus fixtures in `tests/integration/v4/4.5-bounded-vector/`.

6. **Phase 33 forward-reference** — `to_fixed()` / `to_bounded()` conversion helpers are deferred to Phase 33 (Stdlib Container Rewrite). Code 283's hint string explicitly forward-references Phase 33 so programmers encountering the error know when the ergonomic path arrives. Phase 34 LSP-10 quickfix for E0283 will suggest the Phase 33 conversion helper calls when Phase 33 ships.

7. **CORE-22 invariant** — Zero `src/lsp/` source modifications across all 3 Phase 23 plans. Plan 23-03 adds `tests/lsp/smoke/test_did_publish_vec_violation.py` — a pytest-lsp test that USES the CORE-22 facade but does NOT modify it. The `lsp_phase23_smoke` CTest target is the regression anchor.

8. **ABI lock** — The `Iron_BVec_T_N` struct layout (`uint32_t len; T data[N]`) is locked as of Phase 23. See `docs/dev/BVEC-LAYOUT.md` for the full ABI document including nested layout, List[[T;<=N]] storage, alignment formula, and sizeof derivation.

## Phase 24 — Resource Types: drop / copy / nocopy (§7 + §12 steps 9–10)

| Code | Symbol | Message (locked substring) | Hint | Quickfix-Target (Phase 34 LSP-10) | Phase | Spec § |
|-----:|--------|----------------------------|------|-----------------------------------|------:|--------|
| 284 | `IRON_ERR_DROP_DUPLICATE` | `duplicate 'drop' block` | `§7.1: an object may declare at most one drop block` | Remove duplicate drop block (LSP-10) | 24 (Plan 24-01) | §7.1 |
| 285 | `IRON_ERR_COPY_DUPLICATE` | `duplicate 'copy' block` | `§7.2: an object may declare at most one copy block` | Remove duplicate copy block (LSP-10) | 24 (Plan 24-01) | §7.2 |
| 286 | `IRON_ERR_COPY_OF_NOCOPY_TYPE` | `cannot copy nocopy type` | `§7.2: nocopy objects may not be copied, passed by value, or returned by value` | Change binding to a reference or use explicit clone() (Phase 33 — LSP-10 deferred) | 24 (Plan 24-01) | §7.2 |
| 287 | `IRON_ERR_DROP_NOT_READONLY` | `'drop' body may not be marked readonly` | `§7.1: drop blocks run side-effectful cleanup; readonly is incompatible` | Remove readonly modifier from drop block (LSP-10) | 24 (Plan 24-01) | §7.1 |
| 288 | `IRON_ERR_DROP_NO_EARLY_RETURN` | `'return' not allowed in drop body` | `§7.1: drop bodies must run to completion; early return skips field destructor sweep` | Remove early return (LSP-10) | 24 (Plan 24-01) | §7.1 |

### Notes

1. **§7 hint discipline (DROP-01/06/08)** — All five DROP codes carry hint substring `§7.1:` or `§7.2:` + rule text, consistent with Phase 22's §6-prefix discipline (READ-09) and Phase 23's §3.3-prefix discipline. Every hint includes the spec section that mandates the restriction.

2. **Duplicate detection via top-level dispatch walk** — `IRON_ERR_DROP_DUPLICATE` (284) and `IRON_ERR_COPY_DUPLICATE` (285) are emitted in the object-declaration arm of `src/analyzer/typecheck.c`. The top-level dispatch walks the object's member list; when it encounters a second `IRON_NODE_DROP` or `IRON_NODE_COPY` node after already registering one, it emits the duplicate code on the second occurrence. This mirrors the Phase 18 duplicate-method-block pattern.

3. **Nocopy enforcement at 3 sites** — `IRON_ERR_COPY_OF_NOCOPY_TYPE` (286) is emitted in three distinct positions:
   - VAL/VAR declaration with a nocopy-typed RHS that forces a value copy (typecheck.c VAL_DECL arm).
   - Function call passing a nocopy argument by value (typecheck.c CALL arm — Pitfall 2: must check param mode, not just type).
   - `return` of a nocopy type by value (typecheck.c RETURN arm).
   Struct-assignment in compound-literal init is exempted because init blocks produce the first copy (construction), not a subsequent copy.

4. **Reverse declaration order for field destructors (Pitfall 6)** — `emit_helpers.c` `emit_field_destructors` walks the field list in reverse order, emitting `<FieldType>_drop(&self->field_N)` from last to first. This ensures LIFO destruction semantics: a field initialized after another is destroyed before it. The user-written `drop { ... }` body runs BEFORE the field destructor sweep, giving the body access to all fields in their initialized state.

5. **`iron_panic_destructor_aborted` for DROP-04** — The `iron_in_destructor` TLS flag is set in the **prologue** of every generated `<TypeName>_drop` function (detected by `emit_func_body` checking `fn->name` ending with `_drop`). Every existing `iron_panic_*` function checks this flag at its top; when set, it diverts to `iron_panic_destructor_aborted(type_name, __FILE__, __LINE__)` which prints `iron: destructor panicked` (text channel) or `{"panic":"destructor_aborted",...}` (JSON channel) and calls `abort()`. The ABI contract is fully documented in `docs/dev/DROP-LAYOUT.md` §5.

6. **Partial-init cleanup via TLS stack for DROP-05** — The `IronInitCleanupEntry` TLS linked list (`iron_init_cleanup_top`) is pushed per assigned field during init-method execution. `iron_init_cleanup_run_and_clear` is called from the panic path when `iron_init_cleanup_top != NULL`. In v3.0-alpha.1, Iron always inlines init calls as compound literals, making the `*_init` C function dead code; the end-to-end partial-init panic path is therefore not exercised by the current corpus. The mechanism is fully implemented and wired; Phase 32 `defer` integration will provide the execution context needed for end-to-end coverage. See `docs/dev/DROP-LAYOUT.md` §4 for the struct layout and limitation note.

7. **DROP-07 — panicking copy: documented UB** — A copy body that panics leaves the destination in an undefined state (partially initialized fields). This is documented UB in v3.0-alpha.1. Types needing fallible copy must expose `clone() -> T?` (Phase 33 Stdlib Container Rewrite). The compiler enforces no special handling beyond the `iron_in_destructor` panic-trap pattern on drop (not copy). The full contract and rationale are in `docs/dev/DROP-LAYOUT.md` §7. PROJECT.md Key Decisions table has the canonical resolution row.

8. **Phase 26 forward-reference (rc vtable polymorphic drop)** — `PHASE-26 HOOK` comment stubs in `src/lir/emit_c.c` and `src/lir/emit_helpers.c` mark where Phase 26 will wire `<TypeName>_drop` through a vtable pointer on the rc allocation header's `type_info` field. The `iron_in_destructor` prologue/epilogue pattern applies equally to vtable-dispatched drops — the flag is set by the drop function itself, regardless of how it is invoked.

9. **DROP-01 PARTIAL `[~]` in REQUIREMENTS.md** — The rc last-reference drop path is deferred to Phase 26 (`PHASE-26 HOOK` stubs left in `emit_c.c` + `emit_helpers.c`). Stack scope-exit drop and heap-`free`-site drop are complete as of Phase 24. REQUIREMENTS.md marks DROP-01 as `[~]` (partial) with citation `Plan 24-02 + Plan 24-03 + Phase 26 forward`. DROP-02/03/06/07/08 are `[x]` Complete.

## Adding new codes

1. Allocate the next free slot in the appropriate range. Verify uniqueness
   with `grep -nE '^#define IRON_(ERR|WARN)_' src/diagnostics/diagnostics.h | sort -k3 -n`.
2. Add a one-line `#define` plus a short comment block in
   `src/diagnostics/diagnostics.h`.
3. Append a row to the table in this file (group by phase under a new
   `## Phase NN — <subject>` heading).
4. Add a Wave 0 RED test referencing the symbol BEFORE the implementation
   lands (per the Phase 15 TDD-11 + Phase 16 TDD-10 atomic-commit
   discipline).
5. Document Quickfix-Target intent so Phase 34 LSP-06 (or any future
   quickfix-routing phase) has a stable contract to wire against.
