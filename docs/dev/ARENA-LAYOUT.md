# ARENA-LAYOUT — Phase 28 Arena Allocation ABI Lock

**Status:** Locked (Phase 28 Plan 28-02 closeout).
**Scope:** Runtime ABI for the user-facing `Arena` stdlib type's bump-allocator
backing (`src/runtime/iron_arena_rt.{c,h}`). Distinct from the compiler's
internal `Iron_Arena` (`src/util/arena.c`).

Changing any layout below is a public ABI break requiring an explicit version
bump + migration plan, the same discipline as `POINTER-LAYOUT.md` (Phase 19)
and `RC-LAYOUT.md` (Phase 26/27).

---

## 1. `IronArenaAllocHdr` — per-allocation prefix header (16B)

Every arena allocation is laid out as `[IronArenaAllocHdr][user payload]`, with
the user payload 16B-aligned. The header is the minimal information needed for
deref-routing and destructor walks — strictly smaller than the full Phase-19
`IronAllocHdr`.

```c
typedef struct IronArenaAllocHdr {
    iron_atomic_u64 *arena_gen;  /* offset 0 — back-ref to owning Arena's live gen */
    uint64_t         size;       /* offset 8 — payload size (destructor walk + accounting) */
} IronArenaAllocHdr;             /* sizeof == 16 */
```

| Field       | Offset | Size | Role |
|-------------|--------|------|------|
| `arena_gen` | 0      | 8    | Back-reference to the owning `Iron_Arena_RT`'s live generation counter. Deref-routing reads this FIRST. |
| `size`      | 8      | 8    | Payload size in bytes (destructor-walk + accounting). |

**ABI lock (compile-time `_Static_assert` in `iron_arena_rt.h`):**

```c
_Static_assert(sizeof(IronArenaAllocHdr) == 16, "ARENA-LAYOUT ABI lock");
_Static_assert(offsetof(IronArenaAllocHdr, arena_gen) == 0, ...);
_Static_assert(offsetof(IronArenaAllocHdr, size)      == 8, ...);
```

Header recovery from a user pointer is `((IronArenaAllocHdr *)fp.addr) - 1`
(equivalently `(const char *)fp.addr - 16`). `arena_gen` being the first field
lets `iron_check_arena_pointer_gen` read the back-ref counter pointer directly.

`Iron_FatPtr` stays 16B ABI-frozen (`iron_runtime.h:160`). The arena back-ref
lives in this prefix header, NOT in the fat pointer; the fat pointer's `gen`
field holds only the generation SNAPSHOT taken at allocation time.

---

## 2. `Iron_ArenaSave` — save point (16B on LP64)

```c
typedef struct {
    uint64_t offset;        /* bump offset at save() time */
    uint64_t gen_snapshot;  /* arena generation at save() time */
} Iron_ArenaSave;           /* sizeof == 16 on LP64 */
```

`save()` captures the current bump offset + the generation snapshot.
`restore(save)` lowers the bump pointer back to `offset` AND bumps the arena
generation (ARENA-07), invalidating every pointer allocated after the save
point. An opaque value struct at the Iron surface level.

---

## 3. Generation discipline (GA1 — O(1) mass-invalidation)

- **Arena-LEVEL shared generation counter** — one `iron_atomic_u64 gen` per
  `Iron_Arena_RT`, NOT per allocation. Allocations read/advance it but do not
  each own a counter.
- **`gen` starts at 1.** `gen == 0` is the Phase-19-reserved freed sentinel
  (`iron_runtime.h:153`); a fresh arena must never hand out a pointer whose
  snapshot is 0.
- **Monotonic per-allocation snapshot.** `alloc()` does
  `fp.gen = IRON_ATOMIC_U64_FETCH_ADD_RELAXED(gen, 1)` — the snapshot is the
  PRE-increment value, and the live counter advances by one. Snapshots are
  therefore strictly increasing in allocation order. A pointer is **valid iff
  `snapshot < live gen`** (so a fresh allocation, `old < old+1`, is valid the
  instant it is returned).
- **`reset()`** lowers the live counter to the floor (1) via
  `IRON_ATOMIC_U64_INIT(gen, 1)`. Every outstanding snapshot is `>= 1`, so all
  go stale in O(1) — no per-allocation sweep. Subsequent allocations resume
  handing out `1, 2, 3, ...`.
- **`restore(save)`** lowers the live counter to the saved `gen_snapshot` via
  `IRON_ATOMIC_U64_INIT(gen, save.gen_snapshot)`. Allocations made AFTER the
  save (snapshot `>= gen_snapshot`) go stale; allocations made BEFORE the save
  (snapshot `< gen_snapshot`) survive. This selective invalidation is the
  reason the counter is advanced per-allocation rather than per-reset — a
  single shared counter bumped only on reset/restore could not distinguish
  pre- from post-save pointers (they would share one snapshot value).
- **Deref check** — `iron_check_arena_pointer_gen` (the 3rd isomorphic sibling
  of `iron_check_pointer_gen` / `iron_check_stack_pointer_gen`, in
  `iron_runtime.h`) recovers the header, loads `*hdr->arena_gen` (the arena's
  CURRENT live counter), and panics via `iron_panic_arena_stale` iff the fat
  pointer's snapshot is `>= cur` (valid iff `snapshot < cur`). Phase 19's
  substrate stays UNTOUCHED — this is an additive sibling so Phase 30's elision
  pass templates over all three.

### Threadsafe vs plain bump

`Arena.new(size)` uses a plain `offset += need` bump. `Arena.new_threadsafe(size)`
(ARENA-02) bumps a DEDICATED `atomic_offset` field via
`IRON_ATOMIC_U64_FETCH_ADD_RELAXED` — never `gen` (which is the invalidation
counter, not the bump pointer). `used()` reads whichever field the arena's mode
uses.

### OOM (ARENA-10)

`iron_arena_rt_alloc` / `iron_arena_rt_new` panic via
`iron_panic_arena_oom(name, requested_size, capacity)` on capacity exhaustion.
The bump-pointer contract never returns null; this is a deterministic
`noreturn` abort whose message carries the arena name + requested size +
capacity.

---

## 4. OQ-11 — minimal internal allocator interface (internal-only)

OQ-11 is resolved **internal-only** this phase: `Arena` exposes its allocation
API but NO public `interface Allocator` is surfaced. User-defined polymorphic
allocators are deferred.

The minimal internal allocator interface signature — the shape a future public
`interface Allocator` would generalize — is:

```c
Iron_FatPtr iron_arena_rt_alloc(Iron_Arena_RT *arena, uint64_t size);
```

i.e. `alloc(arena, size) -> Iron_FatPtr`. Conceptually `(self, size) -> ptr`.
This is documented here as the internal contract ONLY; it is NOT a stable
user-facing polymorphic surface and carries no `interface` declaration in this
phase.

---

## 5. Public runtime API (Plan 28-01 unit-test contract)

> **Symbol-namespace lock (Plan 28-02 decision):** every runtime arena function
> carries the `iron_arena_rt_` prefix. The bare `iron_arena_*` namespace is owned
> by the compiler-internal arena (`src/util/arena.h`:
> `iron_arena_create/alloc/strdup/free/track/realloc_tracked`, ~209 call sites).
> The runtime allocator deliberately does NOT reuse those names — a 2-arg
> `iron_arena_alloc(Iron_Arena_RT*, uint64_t)` would have collided with the 3-arg
> compiler `iron_arena_alloc(Iron_Arena*, size_t, size_t)` under `-Werror` in any
> TU including both headers. The whole runtime API is `iron_arena_rt_`-prefixed
> for namespace coherence with the `Iron_Arena_RT` type.

```c
Iron_Arena_RT *iron_arena_rt_new(uint64_t capacity, bool threadsafe, const char *name);
Iron_FatPtr    iron_arena_rt_alloc(Iron_Arena_RT *a, uint64_t size);
void           iron_arena_rt_reset(Iron_Arena_RT *a);                       /* ARENA-06 */
Iron_ArenaSave iron_arena_rt_save(Iron_Arena_RT *a);                        /* ARENA-01 */
void           iron_arena_rt_restore(Iron_Arena_RT *a, Iron_ArenaSave s);   /* ARENA-07 */
uint64_t       iron_arena_rt_used(Iron_Arena_RT *a);                        /* ARENA-01 */
uint64_t       iron_arena_rt_capacity(Iron_Arena_RT *a);                    /* ARENA-01 */
void           iron_arena_rt_destroy(Iron_Arena_RT *a);

/* ARENA-11 thread-local active-arena stack */
extern _Thread_local Iron_Arena_RT *iron_arena_rt_tls_top;
void           iron_arena_rt_push(Iron_Arena_RT *a);
void           iron_arena_rt_pop(void);
Iron_Arena_RT *iron_arena_rt_current(void);   /* NULL = general allocator (ARENA-05) */
```

---

## 6. Scope Boundary (OQ-11 / OQ-12 / OQ-13 — locked at Phase 28 closeout)

The three open questions that bound the arena phase are resolved as follows.
This section is the authoritative scope lock; the design rationale lives in
`28-CONTEXT.md` GA3/GA4.

### OQ-11 — allocator interface: RESOLVED, internal-only (this phase)

`Arena` exposes its allocation API but Phase 28 surfaces **no public
`interface Allocator`**. The minimal internal allocator signature
(`iron_arena_rt_alloc(arena, size) -> Iron_FatPtr`, conceptually
`(self, size) -> ptr`) is documented in §4 as an INTERNAL contract only — it
carries no `interface` declaration and is NOT a stable user-facing polymorphic
surface. User-defined polymorphic allocators are out of scope for v4.

### OQ-12 — cross-arena pointer escape static warning: DEFERRED to Phase 30

A *static* analysis warning for a pointer that escapes the arena it was
allocated in (e.g. returning an arena-allocated `heap(in: arena) T` past the
arena's lifetime) is **DEFERRED to Phase 30** (the pointer-check-elision
optimizer phase), per the requirement text. Phase 28 ships ONLY the **runtime
generation-panic safety net**: a stale cross-arena pointer dereferenced after
the owning arena is `reset()`/`restore()`/`destroy()`-ed panics deterministically
via `iron_check_arena_pointer_gen` -> `iron_panic_arena_stale` (§3). The runtime
guarantee is sound today; the *compile-time* escape diagnostic is the Phase 30
deliverable. No E03xx/W06xx escape-warning code is allocated in Phase 28.

### OQ-13 — nested arena-in-arena allocation composition: OUT OF SCOPE

Allocating one arena's backing region *out of another arena* (allocation
composition — an arena whose bump region is itself sub-allocated from a parent
arena) is **reaffirmed OUT OF SCOPE** (default = no). This is NOT revisited in
v4 unless explicitly lifted at milestone close.

**Important distinction — lexical nesting IS allowed (ARENA-11).** Writing

```iron
in outer_arena {
    in inner_arena {        -- lexical scope nesting: legal
        val x = heap T(...)  -- resolves to inner_arena (innermost active wins)
    }
}
```

is **scope nesting**, NOT allocation composition: each `in arena {}` block
pushes/pops the thread-local active-arena stack (§5
`iron_arena_rt_push`/`pop`/`current`), and bare `heap` inside resolves to the
innermost active arena. The two arenas are independent malloc-backed regions;
neither is sub-allocated from the other. OQ-13's "out of scope" forbids the
*composition* (inner arena's memory drawn from outer arena), never the lexical
nesting.

---

## 7. References

- `src/runtime/iron_arena_rt.{c,h}` — implementation + ABI `_Static_assert`s.
- `src/runtime/iron_runtime.h` — `iron_check_arena_pointer_gen` (3rd sibling),
  `Iron_FatPtr` (16B), `IRON_ATOMIC_U64_*` family.
- `src/runtime/iron_panic.c` — `iron_panic_arena_stale` + `iron_panic_arena_oom`.
- `tests/unit/test_arena_layout.c` — runtime ABI witnesses.
- `tests/unit/test_arena_gen_invalidation.c` — headline soundness proof.
- `docs/dev/POINTER-LAYOUT.md` / `RC-LAYOUT.md` — sibling ABI-lock docs.
- `.planning/phases/28-arena-allocation/28-CONTEXT.md` GA1 — design decisions.
