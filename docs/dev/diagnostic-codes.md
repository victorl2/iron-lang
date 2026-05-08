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
