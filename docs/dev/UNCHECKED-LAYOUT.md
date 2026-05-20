# UNCHECKED-LAYOUT.md — `*unchecked T` ABI + `Box[T]` Layout Reference

**Phase 25 (Plan 25-03) ABI lock.**
Parallel to `POINTER-LAYOUT.md` (Phase 19/20 checked-pointer ABI),
`BVEC-LAYOUT.md` (Phase 23 bounded-vector ABI), and `DROP-LAYOUT.md`
(Phase 24 drop/copy/nocopy ABI). This document covers the disjoint
unchecked pointer regime and the `Box[T]` stdlib type.

---

## Section 1 — Unchecked Pointer ABI (`*unchecked T`)

`*unchecked T` and `*var unchecked T` are **bare C `T*` pointers (8 bytes
on 64-bit targets)**. They carry NO generation field, perform NO runtime
check on dereference, and have NO `Iron_FatPtr` wrapping.

| Property            | Checked regime (`*T`)          | Unchecked regime (`*unchecked T`) |
|---------------------|-------------------------------|-----------------------------------|
| C type              | `Iron_FatPtr` (struct, 16B)   | `T*` (raw pointer, 8B)            |
| Generation field    | Yes (`.gen`, 8B)              | No                                |
| Deref runtime check | Yes (`iron_check_pointer_gen`) | No (bare `*p`)                   |
| ABI size (64-bit)   | 16 bytes                      | 8 bytes                           |
| Source              | `&expr` / `heap T(...)` / ... | `Box.unwrap()` ONLY (Phase 25)    |
| Phase 33 extension  | —                             | `RawPtr` + `Ptr.cast` between regimes |

**Codegen:** `emit_c.c` branches at `IRON_LIR_PTR_LOAD` and `IRON_LIR_PTR_STORE`
on `is_unchecked`:
- Checked path: `iron_check_pointer_gen(fp, file, line)` then dereference via `.addr`
- Unchecked path: direct `*p` — no check, no `.addr` field access

**Function signatures:** parameters typed `*unchecked T` are emitted as `T*` in
generated C function signatures. Auto-address (`&`-coercion) is suppressed for
these parameters (UNCK-05); the programmer must provide an `*unchecked T` value
explicitly.

**Phase 30 implication:** the unchecked deref path is already zero-check. Phase 30
(Pointer Check Elision Optimizer) only targets the checked-deref paths
(`iron_check_pointer_gen` / `iron_check_stack_pointer_gen`). No Phase 30 work
required for `*unchecked T` paths.

---

## Section 2 — `Iron_Box_<T>` Layout

Each concrete `Box[T]` instantiation produces a synthesized C typedef via
`emit_ensure_box` in `src/lir/emit_helpers.c`:

```c
typedef struct {
    Iron_FatPtr inner;  /* 16 bytes: .addr = heap T*, .gen = allocation gen */
} Iron_Box_Point;       /* example for Box[Point] */
```

**Key properties:**
- `Iron_Box_<T>` is a **16-byte struct** wrapping the 16-byte `Iron_FatPtr`.
- The `inner.addr` field holds the bare `T*` to the heap allocation.
- The `inner.gen` field holds the allocation generation (from `IronAllocHdr`).
- `Box.null()` initializes `inner = {.addr = NULL, .gen = 0}`.
- `Box.is_null()` checks `inner.addr == NULL`.

**Deduplication:** `EmitCtx.emitted_boxes` (stb_ds string array) tracks which
`Iron_Box_<T>` typedefs have been emitted. Parallel to `EmitCtx.emitted_bvecs`
(Phase 23 pattern).

**Struct name convention:** `Iron_Box_<TypeName>` — e.g., `Iron_Box_Point`,
`Iron_Box_Config`, `Iron_Box_U8`. Matches the `Iron_BVec_<T>_<N>` naming
convention from Phase 23.

---

## Section 3 — `Box[T]` Helper Functions

Each `Iron_Box_<T>` typedef also synthesizes three C helper functions:

### `Iron_Box_<T>_new(T value) -> Iron_Box_<T>`

Wraps `iron_heap_alloc` (Phase 19 substrate):
```c
Iron_Box_Point Iron_Box_Point_new(Point value) {
    Iron_FatPtr fp = iron_heap_alloc(sizeof(Point));
    *(Point*)fp.addr = value;
    return (Iron_Box_Point){ .inner = fp };
}
```

### `Iron_Box_<T>_unwrap(Iron_Box_<T> *self) -> T*`

Returns bare `T*` by casting `.inner.addr`. Does NOT perform a generation
check — caller responsibility. Panics if `inner.addr == NULL`:
```c
Point* Iron_Box_Point_unwrap(Iron_Box_Point *self) {
    if (self->inner.addr == NULL) {
        iron_panic_null_box_unwrap("Point", __FILE__, __LINE__);
    }
    return (Point*)self->inner.addr;
}
```

### `Iron_Box_<T>_free(Iron_Box_<T> *self)`

Wraps `iron_heap_free` (Phase 21 substrate). After free, `inner.addr` is
set to NULL (guard against double-free):
```c
void Iron_Box_Point_free(Iron_Box_Point *self) {
    if (self->inner.addr != NULL) {
        iron_heap_free(self->inner);
        self->inner.addr = NULL;
        self->inner.gen = 0;
    }
}
```

**Synthesized drop:** The Iron `Box[T]` object has a synthesized `drop { ... }`
block (Phase 24 substrate) that calls `Iron_Box_<T>_free` at scope exit.
Explicit `Box.free()` is an alternative: faster (no drop-block overhead),
but equally correct. The programmer chooses based on performance requirements.

---

## Section 4 — `Box.unwrap` Lifetime Contract (DROP-07 Reference)

**CRITICAL:** `Box.unwrap()` returns a bare `T*` with no generation tracking.
`Box[T]` continues to own the allocation — it will be freed when the Box is
dropped (scope exit) or explicitly freed via `Box.free()`. The caller is
responsible for ensuring the returned `*unchecked T` is NOT used after the
Box is freed or dropped.

**Pre-free dereference is UNDEFINED BEHAVIOR:**

```iron
func main() {
    val boxed = Box.new(Point(x: 1, y: 2))
    val raw_ptr: *unchecked Point = boxed.unwrap()
    Box.free(boxed)
    -- raw_ptr.x is now UB -- Box has been freed
}
```

This is analogous to DROP-07 in `docs/dev/DROP-LAYOUT.md` §7 (panicking copy
leaves destination in undefined state). The unchecked regime explicitly trades
safety for performance; the programmer accepts the UB contract.

**Phase 31 forward-reference:** The Phase 31 Debug Allocator will add poison-on-free
and allocation-site tracking. A debug build will catch use-after-free of the
raw pointer returned by `Box.unwrap()` by detecting access to a poisoned page.
Until Phase 31, UAF is silent in release builds.

---

## Section 5 — Disjoint Regime Rule

`*T` (checked) and `*unchecked T` (unchecked) are **fully disjoint types** at the
Iron type system level. No implicit conversion exists in either direction.

| Rule | Error code | Diagnostic |
|------|------------|------------|
| Cross-regime assignment (`*T` → `*unchecked T` or reverse) | E0289 `IRON_ERR_PTR_REGIME_MISMATCH` | "pointer regime mismatch" |
| `&expr` producing `*unchecked T` | E0294 `IRON_ERR_PTR_AMP_NOT_UNCHECKED` | "& cannot produce unchecked pointer" |
| `Ptr.offset`/`Ptr.diff` on checked pointer | E0295 `IRON_ERR_PTR_ARITH_CHECKED` | "pointer arithmetic requires unchecked regime" |

**UNCK-05 auto-address suppression:** At call sites, the auto-address pass
(`auto_address_applies`) checks `!ptr.is_unchecked` before coercing a value `T`
to `*T`. Parameters typed `*unchecked T` are exempt: no implicit `&` is inserted.
The programmer must explicitly pass a `*unchecked T` value (e.g., from `Box.unwrap()`).

**Only escape (Phase 25):** `Box.unwrap()` is the sole path from the checked world
to the unchecked world in Phase 25:
- `Box[T]` wraps a heap allocation (Iron_FatPtr tracked internally).
- `Box.unwrap()` returns the bare `T*` — caller must manage lifetime.
- Phase 33 ships `RawPtr` (STDLIB-10) + extended `Ptr.cast` between regimes.

---

## Section 6 — Phase 30 Optimizer Forward Reference

**OQ-1 RESOLVED (Plan 25-02):** `Ptr.offset` and `Ptr.diff` use new LIR opcodes
(`IRON_LIR_PTR_OFFSET` and `IRON_LIR_PTR_DIFF`) rather than a flag on
`Iron_CallExpr`. Rationale: Phase 30 (Pointer Check Elision Optimizer) wants
explicit opcode visibility for pattern-matching.

`IRON_LIR_PTR_OFFSET`:
- Input: `*unchecked T` base + `Int` offset
- Output: `*unchecked T` advanced by `offset * sizeof(T)` (bare C pointer arithmetic)
- Emitted as: `(T*)base + offset` in `emit_c.c`

`IRON_LIR_PTR_DIFF`:
- Input: `*unchecked T` base + `*unchecked T` target (same-T constraint)
- Output: `Int` element count = `(base - target) / sizeof(T)`
- Emitted as: `(base - target)` with implicit element-count conversion

The flag-on-existing-node approach (Phase 20 `is_ptr_cast_builtin` precedent) is
simpler but **obscures Phase 30 visibility**: the elision pass pattern-matches on
LIR opcode kinds, not on flag fields of generic call nodes.

**Phase 30 scope:** unchecked-deref (`IRON_LIR_PTR_LOAD` / `IRON_LIR_PTR_STORE`
with `is_unchecked=true`) is already zero-check — no elision needed. Phase 30
exclusively targets checked-deref paths (`iron_check_pointer_gen` /
`iron_check_stack_pointer_gen`).

---

## Section 7 — Phase 26 Forward Reference (`rc Box[T]`)

**PHASE-26 HOOK** comments mark the integration points for rc Box[T] interaction:
- `src/analyzer/typecheck.c` — near regime-mismatch emission site (Plan 25-01)
- `src/lir/emit_helpers.c` — near `emit_ensure_box` function (Plan 25-02)

**Open question for Phase 26:** Is `rc Box[T]` a compile error?

- `Box[T]` is `nocopy` (Phase 24 substrate) — copying a `Box[T]` binding emits E0286.
- `rc T(...)` performs refcount copy semantics on `T`.
- If `T = Box[S]`, the `rc` copy attempt must copy a nocopy type — interaction unclear.

Phase 26 (rc Policy, HIGH RISK) decides: either
(a) `rc Box[T]` is a compile error citing "rc + nocopy incompatible" (simplest), or
(b) `rc Box[T]` is allowed with a unique-ownership-in-rc contract (complex).

Until Phase 26 resolves this, `rc Box[T]` source will trigger E0286
(copy of nocopy type) at the `rc` allocation site. This is the conservative
behavior — the programmer gets an explicit error rather than silent misuse.

---

## Section 8 — Phase 33 Forward Reference (`RawPtr` + `Ptr.cast`)

Phase 25 ships `Box.unwrap()` as the ONLY escape from the checked world to the
unchecked world. Two Phase 33 (STDLIB-10) deliverables extend this:

1. **`RawPtr`** — type-erased unchecked pointer (`*unchecked void`-equivalent).
   Allows casting between `*unchecked T` and `*unchecked S` at arbitrary sizes.
   `RawPtr.from(p: *unchecked T) -> RawPtr` + `RawPtr.cast[S]() -> *unchecked S`.

2. **`Ptr.cast[S](p: *unchecked T) -> *unchecked S`** — extended Ptr.cast for
   unchecked regime. Phase 20 `Ptr.cast` is checked-only (size-mismatch check);
   Phase 33 adds an unchecked-to-unchecked variant with no size restriction.

Both are STDLIB-10 scope. Phase 25 error hints reference Phase 33 for completeness
("use RawPtr (Phase 33) for type-erased pointer casts").

---

*Phase: 25-unchecked-t-box-t (Plan 25-03)*
*ABI locked: 2026-05-20*
*Parallel docs: POINTER-LAYOUT.md (checked-ptr ABI), BVEC-LAYOUT.md (bounded-vector ABI), DROP-LAYOUT.md (drop/copy/nocopy ABI)*
