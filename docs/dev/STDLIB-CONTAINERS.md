# Iron Stdlib Container Rewrite — Phase 33 Closeout

**Phase:** 33 — `stdlib-container-rewrite`
**Status:** COMPLETE (all 6 plans landed; phase33-invariant 17/17 GREEN; HARD-24 parity preserved byte-for-byte)
**Completed:** 2026-05-31

This document is the canonical reference for the Phase 33 outcome: the
container drop/copy protocol, the resolutions of OQ-01 and OQ-06, the new
nocopy resource surfaces, the RawPtr type-erased unchecked pointer, and the
explicit DROP-07 residual decision.

---

## 1. Scope recap

Phase 33 rewrote the Iron stdlib container layer to fit the v4 memory model
that landed across Phases 17–32. The high-level deliverables, indexed by the
plans that owned them:

| Plan | Wave | Deliverable |
| ---- | ---- | ----------- |
| 33-01 | 0 | Parser unblock for `box.iron`; Wave-0 RED corpus; `phase33-invariant` ctest scaffold |
| 33-02 | 2 | OQ-01 resolved: `interface Hashable` + primitive-key carve-out + `Map[K: Hashable, V]` / `Set[T: Hashable]` bounds enforced at the type-annotation site |
| 33-03 | 3 | OQ-06 resolved: per-element interface-method dispatch reuses the existing tagged-union mechanism (no new vtable struct) |
| 33-04 | 4 | Element-destructor-aware `List` `_free` / `_clone` monomorphization; `Iron_String` as trivially-copied UTF-8-preserving value handle (Open Question 3) |
| 33-05 | 5 | Nocopy resource types: `Mutex[T]` / `RWLock[T]` / `Channel[T]` / `FileHandle` with by-name dispatch + per-T emit synthesis |
| 33-06 | 6 | `RawPtr` type-erased member of the `*unchecked T` regime; `Ptr.cast[T]` extension; final re-baseline + this doc |
| 33-07 | 1 | (Landed parallel to 33-01..06) `Box[T]` end-to-end by-name dispatch (`Box.new` / `Box.unwrap` / `Box.free` / `Box.null` / `Box.is_null`) |

All container surface files are always-prepended into every compilation
through arms in `src/cli/check.c` (the LSP / `iron_analyze_buffer` facade
path) AND `src/cli/build.c` (the `ironc build` path). Prepending in only
one of the two is an anti-pattern (Pitfall 4 of the phase research) and
breaks the CORE-22 LSP-parity contract.

---

## 2. Container drop/copy protocol (STDLIB-01..04)

### 2.1 The split

The original `IRON_LIST_IMPL` macro was lifecycle-monolithic — its body
hard-coded `free(items)` for `_free` and `memcpy` for `_clone`. That works
for primitives and trivial structs, but it leaks the elements' own `drop`
and silently copies the elements' bit-pattern even when the element type
has a user-supplied `copy` block.

Plan 33-04 split the macro into two pieces:

- `IRON_LIST_IMPL_CORE(T, suffix)` — lifecycle-agnostic surface: per-`T`
  `Iron_List_<suffix>` typedef + the lifecycle-free methods
  (`create` / `push` / `pop` / `get` / `set` / `len`).
- `IRON_LIST_IMPL_TRIVIAL_LIFECYCLE(T, suffix)` — the original
  `free(items)` / `memcpy` body, kept as the FAST PATH for elements with
  no drop/copy.

The original `IRON_LIST_IMPL` macro is **retained as a standalone**
(NOT a composition of `_CORE` + `_TRIVIAL_LIFECYCLE`). Composition would
break the `##suffix` token paste because `bool` would expand to `_Bool`
before the paste sees it. The retained macro is the path the runtime
header uses for the primitive `Iron_List_int64_t` / `Iron_List_bool` /
etc. that ship with `iron_runtime`. The `_CORE` / `_TRIVIAL` macros are
strictly for the emitter, which always passes already-mangled names.

### 2.2 The detector + the synthesizer

`src/lir/emit_structs.c::emit_mono_list_decls` consults
`elem_lifecycle_flags(elem_type)` for every element type it
monomorphizes:

- Element has `od_has_drop_lir` (Phase 24 substrate) **or** is the
  bare-name `FileHandle` carve-out → emit a per-element loop in `_free`
  calling `Iron_<Elem>_drop(&items[i])` before `free(items)`.
- Element has `has_user_copy` → emit a per-element loop in `_clone`
  calling `Iron_<Elem>_copy(&dst->items[i], &src->items[i])` instead
  of `memcpy`.
- Neither → fall through to the `IRON_LIST_IMPL` macro fast path
  (no per-element calls; primitive `List[Int]` is unchanged).

The `<mangled>_drop` / `<mangled>_copy` helpers themselves live in
`ctx->lifted_funcs` (which renders AFTER `struct_bodies` in the output
C). The in-place `Iron_List_<Elem>_free` body lives in
`ctx->struct_bodies`. To resolve the forward reference at clang time,
plan 33-04 emits a **forward prototype** for each helper into
`struct_bodies` before the `_free` / `_clone` body is rendered. This
mirrors the FileHandle pattern already established in
`emit_helpers.c::emit_ensure_filehandle`.

### 2.3 Heap-origin tracking extension

Container `_free` only fires when the analyzer recognizes the container
value as a heap origin escaping the scope. Plan 33-04 extended
`emit_c.c::instr_list_create_has_managed_elem` so that
`Iron_List_<T>_create` CALLs whose element type owns a destructor are
tagged as heap origins. Without this, the empty `[]` literal path for
`var fhs: List[FileHandle] = []` would never trigger scope-exit `_free`,
and the contained fds would leak. The extension is gated on element
having a destructor — primitive `List[Int]` retains the no-allocation
fast path (zero behavior change for the common case).

The same extension audited the escape-detection scan: container self-
methods (`push` / `get` / `set` / `pop` / `len`) take the list as their
first arg, which the existing scan misread as an escape. Those four
methods are now explicitly excluded so a `[]`-built `List` with a managed
element type still gets its scope-exit `_free`.

### 2.4 String as a trivially-copied value handle (Open Question 3)

`Iron_String` is a tagged inline/heap value handle (SSO-style for ≤23
bytes; heap-interned for the longer ones, freed at process shutdown by
the runtime's intern table). A bitwise copy of `Iron_String` is **safe**:

- The SSO inline path needs no allocation — the bytes ride along.
- The heap path stores a borrowed pointer into the intern table; that
  table outlives every `Iron_String` value (shutdown-freed), so the
  borrowed pointer is permanently valid.

Consequences for containers:

- `List[String]` / `Map[K, String]` / `Set[String]` all use the
  `memcpy` / `free(items)` fast path.
- UTF-8 byte length is preserved across value copy AND container
  store/retrieve, because `byte_length` lives in the `Iron_String` struct
  (so it travels with the bitwise copy) and the bytes are never
  re-encoded.

The fixture `tests/integration/v4/7.5-stdlib/string_utf8_roundtrip.iron`
locks the contract.

---

## 3. OQ-01 — generic constraints on `Map` / `Set` (STDLIB-02 RESOLVED)

**Question (deferred from earlier):** How does the compiler enforce
`K: Hashable` on `Map[K, V]` and `T: Hashable` on `Set[T]` when the user
writes `var m: Map[NonHashable, Int]`?

**Answer (Plan 33-02):** Three pieces had to land together; missing any
of them silently passes a bad bound.

1. `src/stdlib/hashable.iron` declares a real
   `interface Hashable { pure func hash() -> Int; pure func equals(other: Self) -> Bool }`
   and is prepended UNCONDITIONALLY on both `check.c` and `build.c`,
   ordered BEFORE `map.iron` / `set.iron`, so it resolves as
   `IRON_SYM_INTERFACE` before any `Map[K, V]` / `Set[T]` instantiation
   is type-checked.

2. `typecheck.c::type_satisfies_constraint` gained a primitive-key
   carve-out: `Int` / `UInt` (all widths) / `Bool` / `String` satisfy
   `Hashable` directly; `Float` is excluded. The carve-out is gated on
   `strcmp(constraint_name, "Hashable") == 0` (using an if-chain rather
   than a switch to dodge `-Werror=switch-enum`).

3. The MISSING ARM: `check_generic_constraints` was only being fired at
   object-literal **construction** sites. `var m: Map[NonHashable, V]`
   resolves via `resolve_type_annotation` and never hits a construction
   call, so the bound silently passed. Plan 33-02 added the missing
   annotation-site enforcement arm in `resolve_type_annotation`. Without
   this, the primitive-key carve-out and the `Hashable` interface
   declaration would still leak bad bounds.

4. `src/stdlib/map.iron` declares `Map[K: Hashable, V]` and
   `src/stdlib/set.iron` declares `Set[T: Hashable]` so the bound is
   visible at the user's annotation site.

Result: `var m: Map[NonHashable, Int]` → E0206 (constraint violated);
`var m: Map[Int, String]` → clean (primitive carve-out).

---

## 4. OQ-06 — per-element interface-method dispatch on containers (RESOLVED, NOT a new vtable)

**Question (deferred from earlier):** How does
`List[Shape]::map(s => s.area())` dispatch `area()` per element when
`Shape` is an interface and the list holds heterogeneous concrete types?

**Original wrong assumption:** the phase research initially scoped this as
"design a vtable struct for interface containers". That was wrong.

**Actual resolution (Plan 33-03):** Iron already has a tagged-union dispatch
mechanism (`emit_c.c::Iron_<Iface>_<method>(self) { switch (self.tag) { ... } }`)
that has been carrying interface-method dispatch for individual interface
values since well before Phase 33. And `emit_split.c` already emits
`Iron_SplitList_<Iface>` storage for collections of interface values. The
two mechanisms compose: per-element method dispatch on a split collection
just calls the existing per-method switch on each element's tagged-union
form. **No new vtable struct was added.** The Plan 33-03 deliverable was
the end-to-end build+run fixture (`list_shape_dispatch.iron`) proving
the composition works, plus the byte-exact expected output that locks
the dispatch order.

The codegen blocker that previously gated this (the always-prepended
`Box`/`Arena` typedefs collided with the runtime header's own typedefs)
was resolved in Plan 33-07 (`feat(33-07): emit skip for runtime-provided
surface types`). Once that landed, `list_shape_dispatch` ran without
modification.

---

## 5. Nocopy resource types (STDLIB-07/08/09 — Plan 33-05)

Five nocopy resource surfaces, each declared in a tiny `.iron` file that
is always-prepended, with the actual C backing synthesized on demand by
`emit_helpers.c::emit_ensure_*`:

| Surface | File | Helper | Element type |
| ------- | ---- | ------ | ------------ |
| `Mutex[T]` | `src/stdlib/mutex.iron` | `emit_ensure_mutex` | generic |
| `MutexGuard[T]` | (synthesized inside mutex glue) | `emit_ensure_mutex` | generic |
| `RWLock[T]` | `src/stdlib/rwlock.iron` | `emit_ensure_rwlock` | generic |
| `RWReadGuard[T]` / `RWWriteGuard[T]` | (synthesized inside rwlock glue) | `emit_ensure_rwlock` | generic |
| `Channel[T]` | `src/stdlib/channel.iron` | `emit_ensure_channel` | generic |
| `FileHandle` | `src/stdlib/filehandle.iron` | `emit_ensure_filehandle` | non-generic |

Each surface is `nocopy object T {}` (Phase 24 substrate) so any `var b = a`
copy trips `IRON_ERR_COPY_OF_NOCOPY_TYPE` (E0286). The constructor
by-name dispatch (`Mutex.new`, `RWLock.new`, `Channel.new`,
`FileHandle.open`) and the receiver-form methods (`m.lock()`, `g.get()`,
`g.set(v)`, `ch.send(v)`, `ch.recv()`, `fh.close()`, etc.) are intercepted
in `typecheck.c` BEFORE the symbol-lookup path that would emit E0200 for
the uppercase namespace ident. The same mechanism that ships `Box[T]` ships
these (RESEARCH Pattern 3; Box's `Box.new` / `Box.unwrap` are the
precedent).

`FileHandle` is the only surface that ships a real synthesized drop:
its drop closes the wrapped fd. Containers of `FileHandle` therefore
release each contained fd at scope exit through the Plan 33-04
element-destructor-aware `_free` loop. The fixture
`tests/integration/v4/7.5-stdlib/list_drop_filehandle.iron` locks the
"closed fd" line per element.

---

## 6. RawPtr — the type-erased member of the `*unchecked T` regime (STDLIB-10 — Plan 33-06)

`RawPtr` is **not** a new pointer kind. It is the type-erased arm of the
existing Phase 25 `*unchecked T` regime. Internally:

```
RawPtr  ≡  IRON_TYPE_PTR { is_unchecked = true, pointee = Int }
```

The `Int` pointee is a stand-in for the 8-byte erased payload; the
type-erasure semantics are enforced in the `Ptr.cast` dispatch (skipped
size-equality check when the source is `*unchecked`) and in the dedicated
`RawPtr.of` builtin which IS the address-producer for the unchecked
regime (bypassing the E0294 `&`-cannot-produce-unchecked guard which
remains intact for user `&` expressions).

### 6.1 Regime guards (inherited)

Because `RawPtr` is `IRON_TYPE_PTR is_unchecked=true`, every existing
guard automatically applies:

- **Auto-address does NOT apply.** `auto_address_applies` in `typecheck.c`
  has the Phase 25 UNCK-05 guard `!param_type->ptr.is_unchecked` —
  unchanged.
- **`&` cannot produce a RawPtr.** Phase 25 PTR-05/UNCK-04 emits E0294
  when the declared type is `*unchecked T` AND the RHS is a `&expr`.
  `val rp: RawPtr = &x` therefore fires E0294 verbatim.
- **Checked/unchecked regimes are disjoint.** Phase 25 PTR-02/03 emits
  E0289 when the two regimes appear on either side of an assignment.

### 6.2 The two builtins

- **`RawPtr.of(x: T) -> RawPtr`** — the compiler-internal address-of for
  the unchecked regime. Lowering (`hir_to_lir.c`) materializes a fresh
  `ALLOCA` slot of `x`'s type, `STORE`s the value into it, then `CALL`s
  the per-T helper `Iron_RawPtr_of_<elemC>(<elemC>*) -> int64_t*` with
  `self_by_addr=true` so the call site emits `&_vN_alloca`. The
  ALLOCA materialization sidesteps the inline-constant trap (`&7LL` is
  not a valid C lvalue when the optimizer folds a constant init).

- **`Ptr.cast[T](p)` — extended to type-erased semantics for unchecked
  inputs.** Old contract (Phase 25 / Plan 25-02): require pointee size
  equality; result inherits the checked regime. New contract (Plan
  33-06):
  - If source is `*unchecked T` (or `RawPtr`), skip the size-equality
    check and produce `*unchecked Target`. This is the type-erased path.
  - If source is `*T` (checked regime), retain the old size-equality
    contract (UNCK-04 unchanged).

  Lowering (`hir_lower.c`) emits a no-op HIR `CAST` node — at the C level
  both source and target are pointer values, so a single
  `(target_type)source` suffices.

### 6.3 Stringification

`println("val: {p}")` where `p: *unchecked Int` now interpolates the
**dereferenced pointee value** (`(long long)*p`) rather than the
opaque pointer. The extension lives in `emit_c.c`'s
`IRON_LIR_INTERP_STRING` formatter and is gated on
`ptr.is_unchecked && pointee.kind ∈ {Int*, UInt*, Float*, Bool}`.
Structured pointees still trip W0602 (there is no `to_string`
contract for a bare pointer to a struct).

---

## 7. DROP-07 residual — DECISION: documented residual

**The question (from REQUIREMENTS.md:344):** A panicking `copy` hook is
documented Undefined Behavior. Types that genuinely need fallible copy
should expose an explicit `clone() -> T?` API. **Should Phase 33 ship
the `clone() -> T?` mechanism?**

**The deferral text (from Phase 24-03):** "include only if it falls out
of copy work; else document residual."

**Decision (Plan 33-06):** **Documented residual.** The Wave 4 container
copy hooks (Plan 33-04) did **NOT** naturally surface a need for fallible
copy. The element-destructor-aware `_clone` body calls the per-element
`Iron_<Elem>_copy` helper synthesized by `emit_ensure_copy` (Phase 24
substrate); the helper takes a `dst*` and `src*` and returns `void`, and
any `copy` block that panics still aborts via the documented-UB path
(`DROP-LAYOUT.md §7`). No call site in the Wave-4 container code asked
for "what if copy fails — give me a `T?`". The element types that do
need fallible construction (resource-handle wrappers like `FileHandle`)
are `nocopy`, so the copy hook is structurally absent.

**What stays in scope:**

- The documented UB contract (`docs/dev/DROP-LAYOUT.md §7`) is the
  authoritative answer for now.
- The `clone() -> T?` API is **deferred** to a future phase (call it
  Phase 33+1 if and when a real call site asks for it).
- The deferral is **NOT** a TODO in any stdlib surface — adding
  `clone()` to `Box` / `List` / `Map` / `Set` without a real need would
  bloat the per-T helper synthesis and slow every compile that uses
  any container.

This decision is consistent with the "include only if it falls out of
copy work" guard and is the explicit close-out asked for in Plan 33-06's
`must_haves.truths` list.

---

## 8. Acceptance evidence (verifiable on a clang host)

### 8.1 phase33-invariant: 17/17 GREEN

```
$ ctest --test-dir build -L phase33-invariant
... 17/17 tests passed ...

  v4_fail_7.5-stdlib_map_nonhashable      Passed   (E0206 negative — OQ-01)
  v4_fail_7.5-stdlib_mutex_copy           Passed   (E0286 negative — nocopy)
  v4_fail_7.5-stdlib_channel_copy         Passed   (E0286 negative — nocopy)
  v4_fail_7.5-stdlib_filehandle_copy      Passed   (E0286 negative — nocopy)
  v4_7.5-stdlib_map_hashable_ok           Passed   (STDLIB-03/04 hash-lookup)
  v4_7.5-stdlib_box_roundtrip             Passed   (OQ-02 / Box dispatch)
  v4_7.5-stdlib_list_shape_dispatch       Passed   (OQ-06 / tagged-union)
  v4_7.5-stdlib_mutex_guard               Passed   (STDLIB-07)
  v4_7.5-stdlib_channel_bounded           Passed   (STDLIB-08)
  v4_7.5-stdlib_filehandle_drop           Passed   (STDLIB-09)
  v4_7.5-stdlib_list_drop_filehandle      Passed   (STDLIB-02 + STDLIB-09)
  v4_7.5-stdlib_string_utf8_roundtrip     Passed   (STDLIB-01 / Open Q3)
  v4_7.5-stdlib_rawptr_cast               Passed   (STDLIB-10 — this plan)
  test_constraints                        Passed   (OQ-01 4/4)
  test_container_drop                     Passed   (STDLIB-02 3/3)
  v4_7.5-stdlib_list_copy_elements        Passed*  (WILL_FAIL — needs list copy-on-assign; documented residual)
  v4_7.5-stdlib_set_basic                 Passed*  (WILL_FAIL — needs Set method surface + OQ-02 generic-method dispatch; documented residual)
```

### 8.2 v4-acceptance TDD-11 jump (pre vs post)

| Counter | Pre-Phase-33 (`faccc3d`) | Post-Phase-33 (`f892203`) | Delta |
| ------- | ------------------------ | ------------------------- | ----- |
| PASS    | 0                        | 3                         | +3    |
| FAIL    | 30                       | 27                        | −3    |
| XFAIL   | 104                      | 115                       | +11   |
| TOTAL   | 134                      | 145                       | +11   |

The +11 TOTAL is the new fixtures added across waves 0/2/3/4/5/6. The
+3 PASS / −3 FAIL is the TDD-11 progress.

### 8.3 HARD-24 parity GREEN (zero `src/lsp/` edits)

```
$ ctest --test-dir build -R parity
... 5/5 tests passed ...

  test_parity_ironc_lsp                Passed
  test_parity_ironc_lsp_fmt            Passed
  test_parity_ironc_lsp_suggestions    Passed
  test_parity_v3_print_fixed_point     Passed
  test_lexer_doc_comment_parity        Passed
```

The byte-for-byte parity contract held across all six container plans
without a single `src/lsp/` source modification.

### 8.4 Build / test discipline

- Built on silvaserver (192.168.0.100) inside `iron-lsp-build:latest`
  podman container, `--memory=8g --memory-swap=8g`. CMake / clang are
  containerized; no local Mac builds touched the codebase.
- Local clang host required to actually flip the build+run rows GREEN
  (the container's clang produces the same output; the `WILL_FAIL`
  fixtures move out of `WILL_FAIL` once a real `ironc build` proves the
  byte-exact stdout).

---

## 9. Residuals (out of scope; documented for follow-up)

1. **`list_copy_elements` stays WILL_FAIL.** The per-element
   `Iron_<Elem>_copy` loop IS emitted (Plan 33-04), but list
   value-assignment (`val dst = src`) aliases — it does a shallow struct
   copy of the `items` pointer rather than calling
   `Iron_List_<T>_clone`. Wiring copy-on-assignment for lists is a
   separate codegen feature with broad blast radius (double-free risk if
   any consumer of an aliased list mutated it). Tracked as a follow-up
   in `.planning/phases/33-stdlib-container-rewrite/deferred-items.md`.

2. **`set_basic` stays WILL_FAIL.** Needs the `Set` method surface
   (`Set.new` / `Set.add` / `Set.has` / `Set.len`) + by-name dispatch
   interception, and the OQ-02 generic-method dispatch story (still
   unresolved at phase end). The `set.iron` surface explicitly defers
   methods to a later landing per Plan 33-02.

3. **`clone() -> T?`** — DROP-07 deferred per §7 above.

4. **Pre-existing TSan link failure** in `test_string_intern_race`
   (predates Phase 33; missing TSan runtime in container image).

5. **Three pre-existing `v4_4.13-defer_*` failures** (defer_then_drop,
   defer_early_return, defer_read_at_exit) — present in both
   pre-Phase-33 and post-Phase-33 builds; unrelated to container work.

---

*Phase 33 owner: STATE.md; this doc is the user-facing closeout.*
