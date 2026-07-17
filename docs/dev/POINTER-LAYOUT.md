# Iron Pointer Layout — Public ABI

> **Phase 19 contract.** This document commits the runtime substrate
> for Iron's checked-pointer regime: the 16-byte `Iron_FatPtr` ABI, the
> header-prepended `IronAllocHdr` layout (16 bytes release / 32 bytes
> debug), the relaxed-inc/acquire-load atomic semantics on the per-
> allocation generation counter, the `iron_heap_alloc` / `iron_heap_free`
> / `iron_check_pointer_gen` runtime API surface, and the
> `iron_panic_stale_pointer` panic format (text default; JSON via
> `IRON_PANIC_FORMAT=json`). Phases 20 (Checked Pointer Types), 21
> (Heap policy + free), 25 (Box[T] + *unchecked), 28 (Arena), 30
> (Pointer-check elision optimizer), 31 (Debug allocator), and 33
> (Stdlib container rewrite) all rely on this contract.

## Overview

This page is the source of truth for the runtime substrate that backs
every checked Iron pointer. It is read top-to-bottom by:

- Codegen authors (Phase 20+) wiring `*T` / `*var T` deref / `&` / `heap T(...)`
- Optimizer authors (Phase 30) elide-check transforms
- Debug-allocator authors (Phase 31) extending the debug header
- Arena authors (Phase 28) reusing the fat-pointer ABI with a different
  generation source
- Stdlib container authors (Phase 33) deciding which collections opt in
  to tracker registration

If you are about to change anything documented here — even a comment —
read the **Stability Commitment** section first.

## Stability Commitment

The contract on this page is **stable**. Any change to:

- `sizeof(Iron_FatPtr)` (currently 16 bytes)
- `sizeof(IronAllocHdr)` in release (currently 16) or debug (currently 64
  after the Phase 31 re-lock; was 32 in Phase 19)
- `Iron_FatPtr` field offsets (currently `addr` at 0, `gen` at 8)
- The atomic memory ordering pair (currently `memory_order_relaxed` on
  generation increment, `memory_order_acquire` on deref-check load)
- The runtime API signatures listed below
- The panic-format substrings asserted by `tests/unit/test_runtime_panic_stale.c`
  (`"iron: stale pointer dereference"`, `"deref site: "`, JSON `kind`,
  `deref_site`, `alloc_site`, `allocation` fields)

requires an **explicit `IRON_VERSION_FULL` major-or-minor version bump**
plus a documented migration plan. Internal helper symbol renames,
additional `#ifdef IRON_DEBUG_ALLOCATOR`-gated fields that preserve the
**64-byte** debug header size lock, comment edits, and refactoring that
leaves the tested substrings intact are permitted without a version bump.
The Phase 31 debug grow (32 → 64 bytes) was such an authorized, debug-only
ABI bump; the release 16-byte header was NOT touched.

Compile-time enforcement lives in `src/runtime/iron_runtime.h` and
`src/runtime/iron_heap_track.c`:

- `_Static_assert(sizeof(Iron_FatPtr) == 16, "...")`
- `_Static_assert(sizeof(IronAllocHdr) == 16, "...")` in release branch
- `_Static_assert(sizeof(IronAllocHdr) == 64, "...")` in debug branch
  (re-locked from 32 in Phase 31)
- `_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "...")` in
  `src/runtime/iron_heap_track.c`

Weakening any of these asserts is a release-blocker.

## Iron_FatPtr layout (16 bytes total)

```
offset  size  field
───────────────────────────────────────────────────────────
  0      8    void *addr   ; user-payload pointer
  8      8    uint64_t gen ; generation captured at creation
───────────────────────────────────────────────────────────
total: 16 bytes
```

```c
typedef struct {
    void     *addr;   /* points to user payload; header at addr - sizeof(IronAllocHdr) */
    uint64_t  gen;    /* generation captured at pointer-creation time */
} Iron_FatPtr;
_Static_assert(sizeof(Iron_FatPtr) == 16,
               "Iron_FatPtr must be 16B — System V AMD64 / AAPCS ARM64 "
               "2-register pass-by-value lock");
```

**ABI rationale:** System V AMD64 (Linux, macOS x86_64) passes structs
≤ 16 bytes in two integer registers (RDI+RSI / RDX+RCX / RAX+RDX).
AAPCS64 (ARM64) passes ≤ 16 byte HFA structs in two registers. Growing
past 16 bytes silently degrades to memory-passing — a 5-10× perf
regression on every pointer pass through a generated `func` parameter.

`addr=NULL, gen=0` is the canonical null/freed sentinel. The first
valid generation is 1.

## IronAllocHdr layout (release: 16 bytes / debug: 64 bytes)

The header is **prepended** to every `iron_heap_alloc` block. Recovery
is O(1) pointer arithmetic: `((IronAllocHdr *)fp.addr) - 1`.

> **Phase 31 ABI re-lock (Plan 31-01 / 31-03).** The debug header grew
> from 32 bytes (Phase 19) to **64 bytes** to carry the leak-detection
> registry links and double-free free-site (DBG-03/DBG-04). The release
> header is **UNCHANGED at 16 bytes** — byte-frozen. The release-build
> opt-in leak check (DBG-07, `IRON_LEAK_CHECK=1`) does NOT touch the
> release header: it tracks live allocations in a separate side-table
> (`src/runtime/iron_leakcheck.c`) keyed by the user pointer, so the 16B
> release ABI commitment below holds in every release build regardless of
> the env flag.

### Release build (16 bytes)

```
offset  size  field
───────────────────────────────────────────────────────────
  0      8    iron_atomic_u64 gen   ; atomic generation counter
  8      8    uint64_t        size  ; user payload size in bytes
───────────────────────────────────────────────────────────
total: 16 bytes   ; UNCHANGED across Phase 19 → Phase 31
```

### Debug build (`-DIRON_DEBUG_ALLOCATOR`, 64 bytes — Phase 31 re-lock)

```
offset  size  field
───────────────────────────────────────────────────────────
  0      8    iron_atomic_u64      gen              ; atomic generation counter
  8      8    uint64_t             size             ; user payload size in bytes
 16      8    const char          *alloc_site_file  ; __FILE__ literal pointer  (Phase 19 DBG-02)
 24      4    uint32_t             alloc_site_line  ; __LINE__                   (Phase 19)
 28      4    uint32_t             alloc_id         ; unique id from iron_alloc_id_counter (Phase 19)
 32      8    struct IronAllocHdr *reg_next         ; intrusive leak registry    (Phase 31 DBG-03)
 40      8    struct IronAllocHdr *reg_prev         ; intrusive leak registry    (Phase 31 DBG-03)
 48      8    const char          *free_site_file   ; first free-site, NULL until first free (Phase 31 DBG-04)
 56      4    uint32_t             free_site_line   ; first free-site line       (Phase 31 DBG-04)
 60      4    uint32_t             _pad             ; keeps the size a 16-byte multiple
 64      ─    (next field would start here)         ; size lock at 64 bytes exact
───────────────────────────────────────────────────────────
total: 64 bytes   ; was 32B in Phase 19; re-locked to 64B in Phase 31
```

Why 64 (a 16-byte multiple) and not 60: keeping `sizeof(IronAllocHdr)` a
multiple of 16 preserves the "user pointer stays 16-byte-aligned" guarantee
given malloc's `max_align_t` alignment. The trailing `_pad` exists solely to
round the debug header up to the next 16-byte boundary.

Both layouts yield a 16-byte-aligned user pointer given malloc's
`max_align_t` alignment guarantee on Linux glibc and macOS Libc.
Over-aligned types (e.g. `_Alignas(32) AVXVec`) remain out of scope; both
Phase 19 and Phase 31 document `max_align_t` as the guarantee.

## Atomic memory ordering

| Operation | Memory order | Macro |
|-----------|--------------|-------|
| Initial generation set on alloc | `memory_order_relaxed` (via `atomic_init`) | `IRON_ATOMIC_U64_INIT` |
| Generation increment on free | `memory_order_relaxed` | `IRON_ATOMIC_U64_FETCH_ADD_RELAXED` |
| Generation read on deref-check | `memory_order_acquire` | `IRON_ATOMIC_U64_LOAD_ACQUIRE` |

**Why relaxed inc:** the generation bump does not publish any other
writes — there is no payload data being released alongside. The user
payload is `free()`d immediately after the bump; there are no readers
of the payload after the bump (a stale pointer's
`iron_check_pointer_gen` will see the bumped gen and panic before any
payload load). Relaxed is sufficient and faster than sequentially
consistent.

**Why acquire load:** a check on a stale pointer must observe the bump
done by the freeing thread. `memory_order_acquire` synchronizes-with
the most recent release/seq-cst store on the same atomic. It pairs
with relaxed-fetch-add via the implicit release inherent to RMW
operations.

Pattern: Linux kernel `refcount.h` convention; Rust `Arc` discipline.

On Win32, the `IRON_ATOMIC_U64_*` macros wrap `Interlocked*64`
primitives, which are unconditionally sequentially consistent. This is
acceptable: Iron's parallel-LSP-request model is not bottlenecked on
heap-tracker atomics, and Windows is excluded from CI today.

## Generation lifecycle

| State | Generation value |
|-------|------------------|
| Null/freed sentinel | `0` |
| First valid generation after `iron_heap_alloc` | `1` |
| Each `iron_heap_free` bumps by | `+1` (atomic relaxed) |
| Saturating cap | `UINT64_MAX - 1` (abort beyond) |

UINT64_MAX free()s on a single allocation slot is physically
unreachable (~5 billion years at 100M free/sec). The saturating cap
aborts deterministically at the boundary rather than wrap, which would
risk false-positive validation against a re-used slot.

## Runtime API surface

```c
/* src/runtime/iron_runtime.h + src/runtime/iron_heap_track.{h,c} */

Iron_FatPtr iron_heap_alloc(const char *site_file, int site_line, size_t size);
void        iron_heap_free(Iron_FatPtr fp);

/* Static-inline so Phase 30 optimizer can elide redundant checks: */
static inline void iron_check_pointer_gen(Iron_FatPtr fp,
                                          const char *deref_file,
                                          int deref_line);

/* src/diagnostics/diagnostics.h + src/runtime/iron_panic.{h,c} */
__attribute__((noreturn))
void iron_panic_stale_pointer(const char *deref_file,
                              int deref_line,
                              const struct IronAllocHdr *hdr);
void iron_panic_init_from_env(void);  /* called from iron_runtime_init */

/* Debug-build-only allocation-id counter; declared extern in iron_runtime.h */
extern iron_atomic_u64 iron_alloc_id_counter;
```

### `iron_heap_alloc` contract

- Allocates `sizeof(IronAllocHdr) + size` contiguously via `malloc`.
- On malloc failure: calls `iron_oom_abort("iron_heap_alloc")` (no NULL return).
- Returns `Iron_FatPtr {user_payload_ptr, gen=1}` (first valid gen).
- Debug builds: stores `__FILE__`/`__LINE__` (string-literal pointer; no
  `strdup`), bumps `iron_alloc_id_counter` atomically, stores `alloc_id`.
- Release builds: ignores `site_file` and `site_line` arguments.

### `iron_heap_free` contract

- `fp.addr == NULL` → no-op return.
- Recovers `IronAllocHdr *hdr = ((IronAllocHdr *)fp.addr) - 1`.
- Validates `fp.gen == hdr->gen` (acquire load); on mismatch panics
  via `iron_panic_stale_pointer("<iron_heap_free>", 0, hdr)`. This is
  the double-free detector.
- Saturating overflow guard: if `cur >= UINT64_MAX - 1`, calls
  `iron_oom_abort("iron_heap_free: generation counter overflow")`.
- Bumps `hdr->gen` via relaxed-fetch-add.
- Calls `free(hdr)` on the entire block.

### `iron_check_pointer_gen` contract

- `fp.addr == NULL` → panics with `iron_panic_stale_pointer(deref_file, deref_line, NULL)`.
- Recovers header, acquire-loads `hdr->gen`, compares to `fp.gen`.
- On mismatch: panics with `iron_panic_stale_pointer(deref_file, deref_line, hdr)`.
- **Always on** in every build mode. There is no `--release-skip-checks`
  flag and no internal toggle. SAFE-04.

## Panic format

### Text (default, stderr)

```
iron: stale pointer dereference
  deref site: <file>:<line>
  allocation site: <file>:<line>     ← debug builds only (when hdr non-NULL)
  allocation: id=<N> size=<B>        ← debug builds only (when hdr non-NULL)
[process aborts via SIGABRT]
```

### JSON (`IRON_PANIC_FORMAT=json`, single line on stderr)

```json
{"kind":"stale_pointer",
 "deref_site":{"file":"<file>","line":<n>},
 "alloc_site":{"file":"<file>","line":<n>}|null,
 "allocation":{"id":<n>,"size":<n>}|null}
```

`alloc_site` and `allocation` are `null` in release builds OR when
`hdr` is NULL (e.g., null-pointer deref).

### Environment variable resolution

`IRON_PANIC_FORMAT` is read **once** by `iron_panic_init_from_env`,
called from `iron_runtime_init` (`src/runtime/iron_string.c:275`)
BEFORE any allocation that could panic. The result is cached in a
static file-scope flag in `src/runtime/iron_panic.c`. Subsequent
`setenv()` calls are NOT honored (Pitfall 6: per-panic `getenv` is
not async-signal-safe and may itself allocate or take a lock).

### Termination

`abort()` (matches `iron_oom_abort` precedent). SIGABRT, core dump,
debugger-friendly. **Process-mode** — the entire process dies; per-
thread isolation is explicitly out of scope. Pointer safety is global.

### Panic-during-panic defense

`src/runtime/iron_panic.c` uses ONLY `fputs` / `fprintf` with
compile-time format strings + integer/pointer arguments. No `malloc`,
no `Iron_String`, no `iron_heap_alloc`. The `__FILE__` strings stored
in `IronAllocHdr` are string literals (static-storage); pointer
dereference yields stable memory.

Verified by post-commit grep audit returning 0:

```
grep -ciE "malloc|calloc|realloc|iron_heap_alloc|iron_heap_free|Iron_String|iron_string_" src/runtime/iron_panic.c
```

## Allocation-id counter

`iron_alloc_id_counter` is an `iron_atomic_u64` initialized to 0 in
`iron_runtime_init`. Each `iron_heap_alloc` in debug builds bumps the
counter via `IRON_ATOMIC_U64_FETCH_ADD_RELAXED` and stores the result
in `IronAllocHdr.alloc_id`. Phase 31 (debug allocator) extends this
counter for leak-detection / poison-on-free / double-free reports.

Release builds initialize the counter for forward-compatibility but
do NOT bump it (the field does not exist in the 16-byte release
header).

## What does NOT register with the tracker

The tracker is **exclusively** for user-level `heap T(...)` allocations
that Phase 21 will codegen against. The following Iron stdlib types
deliberately bypass the tracker:

| Type | Allocates via | Reason |
|------|---------------|--------|
| `Iron_String` | `malloc` directly (intern table) | Stdlib opaque type with its own lifecycle; SSO + interning are orthogonal to the user-pointer regime |
| `Iron_List<T>` | `malloc` (in IRON_LIST macros) | Stdlib opaque type; growth needs `realloc`, not generation tracking |
| `Iron_Map`, `Iron_Set` | `malloc` (stb_ds shmap/hmap) | Same |
| `Iron_Rc` | `malloc` (Iron_RcControl + value bytes) | Phase 26 ships rc-policy; rc allocations slot into the same 16B `Iron_FatPtr` layout when Phase 26 wires them, but the generation source is the rc control block, NOT this tracker |
| `Box[T]` (Phase 25) | underlying `malloc` | Spec §4.3 — `Box[T]` is in the unchecked-pointer regime; does NOT register |
| `*unchecked T` (Phase 25) | obtained via `Box.unwrap()` or stdlib `RawPtr` | Spec §4.3 — does NOT register |

Phase 19 makes this explicit by ensuring `iron_heap_alloc` is the ONLY
tracker-registering API.

### Leak-detection scope: intended false-negative (Phase 31)

Because only `iron_heap_alloc` routes through the tracker, the Phase 31 leak
machinery (the debug intrusive registry / atexit dump AND the DBG-07 release
side-table) sees **heap allocations only**. `Iron_Rc` (`iron_rc.c`) and arena
allocations (`iron_arena_rt.c`) each `malloc` their own block and hand-init an
`IronAllocHdr` with `gen` + `size` only — they never call `iron_heap_alloc`
and therefore **never appear in the leak dump**. This is a deliberate, documented
false-negative (CONTEXT GA3 scope: "heap allocator only"), NOT a bug; both dump
headers state "heap allocations only; rc/arena not tracked". A leaked `rc` value
will not show in either dump — by design.

### DBG-07 release opt-in leak check (side-table, no header growth)

The release 16B `IronAllocHdr` has **no registry slots** (and growing it is
forbidden by the stability commitment above). The release-build opt-in leak
check therefore lives entirely OUTSIDE the header, in
`src/runtime/iron_leakcheck.c`: a hand-rolled `malloc`'d node list keyed by the
returned user pointer, armed only when `IRON_LEAK_CHECK=1` (read once at init,
never per allocation). When the env var is unset — the default — the alloc/free
hooks early-return on a single `if (!s_leak_check_enabled) return;` branch, take
no lock, allocate no node, and never poison, so the env-unset release binary is
byte-for-byte behaviourally identical to the pre-Phase-31 release allocator. No
poison is ever applied in release (poison is debug-only). The dump goes to
stderr (Pitfall 7: stdout stays byte-identical across build modes / env
settings).

## Verification

| Property | Test |
|----------|------|
| `sizeof(Iron_FatPtr) == 16` | `_Static_assert` in `iron_runtime.h` + runtime probe in `tests/unit/test_runtime_heap_alloc.c::test_iron_fatptr_layout_is_16_bytes` |
| `sizeof(IronAllocHdr) == 16` (release) / `== 64` (debug, Phase 31 re-lock) | `_Static_assert` in `iron_runtime.h` + runtime probes in `test_iron_alloc_hdr_release_is_16_bytes` (and the debug-header probe) |
| DBG-07 release opt-in leak check (env-on dump / env-off silent / no poison) | `tests/unit/test_leakcheck_release.c` (release-path TU, no `IRON_DEBUG_ALLOCATOR`) |
| Unique generations across N allocs | `tests/unit/test_runtime_heap_alloc.c::test_iron_heap_alloc_returns_unique_gens` |
| Generation bump on free | `tests/unit/test_runtime_heap_free.c::test_iron_heap_free_increments_gen` |
| Double-free panic | `tests/unit/test_runtime_heap_free.c::test_iron_heap_free_double_free_detected` |
| Stale-deref panic (text format) | `tests/unit/test_runtime_panic_stale.c::test_iron_panic_stale_text_*` |
| Stale-deref panic (JSON format) | `tests/unit/test_runtime_panic_stale.c::test_iron_panic_stale_json_*` |
| Release build retains check | `tests/unit/test_runtime_panic_stale_release` (separate Release-mode build) |
| Concurrent counter atomicity | `tests/unit/test_runtime_heap_concurrent.c::test_iron_heap_concurrent_per_thread_alloc_free` (8 × 100k iters) |
| Cross-thread free panic | `tests/unit/test_runtime_heap_concurrent.c::test_iron_heap_cross_thread_free_first_succeeds_rest_panic` |
| Cross-thread deref panic | `tests/unit/test_runtime_heap_concurrent.c::test_iron_heap_cross_thread_deref_after_free_panics` |

All tests roll up under the `phase19-invariant` CTest label. The
thread-stress test additionally carries the `tsan` label and runs
under `-fsanitize=thread` on Linux.

## Phase plan history

- **Plan 19-01** — runtime data structures: `Iron_FatPtr`, `IronAllocHdr`,
  `iron_heap_alloc` / `iron_heap_free`, `IRON_ATOMIC_U64_*` macros,
  static-inline `iron_check_pointer_gen`, allocation-id counter init.
  See `.planning/phases/19-generational-pointer-infrastructure/19-01-SUMMARY.md`.
- **Plan 19-02** — panic mechanism: `iron_panic_stale_pointer` (text +
  JSON), `iron_panic_init_from_env`, declaration appended to
  `src/diagnostics/diagnostics.h`. See 19-02-SUMMARY.md.
- **Plan 19-03** — closeout: this doc + thread-stress test +
  parity audit + STATE/REQUIREMENTS/ROADMAP closeout. See 19-03-SUMMARY.md.

## Forward references

| Phase | Relies on this doc for |
|-------|------------------------|
| Phase 20 (Checked Pointer Types) | `*T` / `*var T` codegen against `Iron_FatPtr` ABI; `&` operator captures generation; auto-deref calls `iron_check_pointer_gen`; stack-frame pointer tracker (TLS slot) added alongside |
| Phase 21 (Heap policy + free) | `heap T(...)` codegen calls `iron_heap_alloc(__FILE__, __LINE__, sizeof(T))`; `free <binding>` calls `iron_heap_free` |
| Phase 25 (Box[T] + *unchecked T) | Box's underlying allocation does NOT register with this tracker; Box.unwrap() returns `*unchecked T` which bypasses `iron_check_pointer_gen` entirely |
| Phase 28 (Arena allocation) | Arena allocations slot into the same 16B `Iron_FatPtr` layout; generation comes from the arena's counter, not a per-allocation header; ABI compatible |
| Phase 30 (Pointer-check elision optimizer) | `iron_check_pointer_gen` is static-inline so the optimizer can recognize the canonical `acquire-load + compare + panic` shape and elide redundant checks |
| Phase 30 (Pointer-check elision optimizer) | `iron_check_stack_pointer_gen` (Phase 20 OQ-B Option C) is also static-inline with isomorphic shape (load gen, compare, panic); Phase 30 EarlyCSE/GVN/LICM is parameterized over both patterns |
| Phase 31 (Debug allocator) | Extends `IronAllocHdr` debug-build fields with poison-on-free, double-free reports, leak detection; size lock at 32 bytes may be relaxed in Phase 31 with a documented version bump |
| Phase 33 (Stdlib container rewrite) | New v4 stdlib containers may opt in to register their allocations with the tracker; ABI is documented and stable |

## Phase 20 surfaces

Phase 20 (Checked Pointer Types) surfaces the runtime substrate locked
above to Iron source as user-visible types and operators. The Phase 19
ABI is unchanged — `Iron_FatPtr` is still 16B, `IronAllocHdr` is
unchanged, `iron_check_pointer_gen` is unchanged. Phase 20 ADDS:

### User-visible surface

- **Type annotations:** `*T`, `*var T`, `?*T`, `?*var T`. Multi-level
  (`**T`) parses but is rejected at typecheck time (auto-deref is
  single-level only).
- **Operators:** `&` (unary address-of, expression-position prefix at
  PREC_UNARY). `*` in expression-position is RESERVED for Phase 25
  (`*unchecked T` regime); attempting `*p = value` emits
  `IRON_ERR_UNEXPECTED_TOKEN`.
- **Auto-deref:** `p.field` and `p.method()` for `p: *T`/`*var T`
  auto-deref through one level. Compile error on `pp.field` for
  `pp: **T`.
- **Auto-address:** `f(my_local)`, `f(obj.field)`, `f(arr[i])`
  auto-address when the parameter is `*T`/`*var T`. Rvalues (literals,
  function-call results) are rejected with E0270.
- **Nullability:** `?*T` encodes null as `addr=NULL` (Phase 19 reserved
  `gen=0` as freed-sentinel; `addr==NULL` is the additional null
  sentinel). Unwrap via flow-typing: `if p != null { use_p_as_T(p) }`.
- **Casting:** `Ptr.cast[T](p)` is a compiler builtin; same-size types
  only; size mismatch is E0269. Phase 25's `Ptr.offset` / `Ptr.diff`
  work on `*unchecked T` only.
- **Closure capture (OQ-02 RESOLVED Plan 20-03):** closures over `*T`
  and `*var T` are LEGAL; closure captures the 16B `Iron_FatPtr` by
  value into its captured-state struct (existing
  `emit_capture_rhs` + C struct-copy semantics handle 16B value-type
  captures unchanged); closure-body deref panics on stale gen at
  invocation site through the same `iron_check_pointer_gen` /
  `iron_check_stack_pointer_gen` path the surrounding code already uses.
  See `tests/integration/v4/4.2-checked-ptr/closure_pointer_capture.iron`
  and `closure_var_pointer_capture.iron` for positive-corpus pinning.

### New runtime artifacts

- **`extern _Thread_local uint64_t iron_stack_gen;`** — per-thread
  stack-frame generation counter. Initial value 1 (`gen=0` reserved).
  Bumped on entry/exit of every function body containing `&local_var`
  syntactically (whole-function pessimistic detection per OQ-E).
- **`static inline void iron_check_stack_pointer_gen(Iron_FatPtr fp,
  const char *deref_file, int deref_line);`** — separate deref-check
  path for stack-source pointers (OQ-B Option C). Compares `fp.gen` to
  `iron_stack_gen` directly; on mismatch calls
  `iron_panic_stale_stack_pointer`. Phase 30 elision target (isomorphic
  shape with `iron_check_pointer_gen`).
- **`__attribute__((noreturn)) void iron_panic_stale_stack_pointer(
  const char *deref_file, int deref_line, uint64_t captured_frame_gen);`**
  — stack-pointer panic emission. Same channels as
  `iron_panic_stale_pointer` (text + JSON via `IRON_PANIC_FORMAT` env).
  Text format: `"iron: dangling stack pointer to frame #N (current
  frame #M) at <file>:<line>"`. JSON format:
  `{"panic":"stack_pointer","deref_site":...,"captured_frame_gen":N,
  "current_stack_gen":N}`.

### Codegen behavior (informative)

- `&x` lowers to
  `(Iron_FatPtr){ .addr = (void *)&x, .gen = source_gen }` where
  `source_gen` is `iron_stack_gen` (stack-rooted) or `hdr->gen`
  (heap-rooted). The LIR opcode `IRON_LIR_ADDR_OF` carries an
  `IronLIR_GenSource` tag (HEAP/STACK) that picks the right gen value
  and the right deref-check path.
- Auto-deref `p.field` lowers to
  `iron_check_pointer_gen(p, file, line); ((PointeeT *)p.addr)->field`
  — or `iron_check_stack_pointer_gen` for stack-source.
- Field-pointer `&x.field` carries the parent `x`'s outermost-allocation
  generation (PTR-08); element-pointer `&arr[i]` carries `arr`'s
  generation (PTR-09). For `&heap_obj.field_pointing_to_local`, gen =
  `heap_obj.hdr->gen` (the field's value, even if itself a pointer, is
  just data — OQ-C).
- Functions with `takes_local_addr=true` emit `iron_stack_gen += 1;` on
  entry AND immediately before each `return ...;` statement (OQ-E
  per-call lock).

### Diagnostic codes added (cross-reference)

See `docs/dev/diagnostic-codes.md` Phase 20 section for the full table.
Codes 268–272 are reserved for Phase 20.

### Stability commitment for Phase 20 surfaces

The Phase 20 surfaces section is **informative** (describes user-visible
behavior). The ABI sections above (Iron_FatPtr layout, IronAllocHdr
layout, atomic semantics, runtime API signatures, panic format string
discriminators) remain the binding contract. Phase 20 adds the
`iron_stack_gen` TLS slot, `iron_check_stack_pointer_gen` static-inline,
and `iron_panic_stale_stack_pointer` helper as additive surface; their
removal or signature change requires the same `IRON_VERSION_FULL`
major-or-minor bump discipline as the Phase 19 ABI items above.

### Forward references (Phase 20 additions)

- Phase 21 (Heap Policy + Free): `heap T(...)` syntax provides the heap
  allocation expression that Phase 20's `&` consumes; PTR-04's
  `&heap_alloc` requires Phase 21 syntax to materialize.
- Phase 25 (Unchecked Pointers + Box[T]): hosts `*unchecked T`,
  `Box[T]`, `Ptr.offset`, `Ptr.diff`, `Ptr.set`. Phase 20
  forward-references via diagnostic codes 268 + 269 hint strings.
- Phase 30 (Pointer-check elision optimizer): targets BOTH
  `iron_check_pointer_gen` (Phase 19) and `iron_check_stack_pointer_gen`
  (Phase 20) for elision. Both are static-inlines with isomorphic
  shapes.
- Phase 34 (LSP adaptation): consumes
  `docs/dev/diagnostic-codes.md` Phase 20 quickfix-target column to
  wire LSP-06 quickfix actions.

## Phase 21 surfaces

Phase 21 (Heap Policy + Free) surfaces the runtime substrate locked above to
Iron source as user-visible heap allocation, free, leak, and defer-free idioms.
The Phase 19 ABI is unchanged — Iron_FatPtr is still 16B, IronAllocHdr is
unchanged, iron_heap_alloc / iron_heap_free / iron_check_pointer_gen are
unchanged. Phase 21 ACTIVATES these existing surfaces from Iron source for the
first time:

### User-visible surface

- **Allocation:** `heap T(...)` is the sole allocation form. The expression
  returns a value of type `T` (NOT `*T` and NOT `rc T`); the underlying
  allocation is generation-tracked via Phase 19's IronAllocHdr.
- **Position lock (POL-03):** `heap` is RESERVED to allocation-expression
  position. Three illegal positions emit `IRON_ERR_HEAP_BAD_POSITION` (E0273)
  with hint distinguishing the position:
  - In type annotation: `val p: heap T = ...` → "got `heap` in type annotation"
  - On binding declaration: `heap val p = ...` / `heap var p = ...` → "got `heap` in binding declaration"
  - On parameter: `func f(heap p: T) {}` → "got `heap` in parameter declaration"
- **Free:** `free <binding>` invalidates the heap allocation. Lowers to
  `iron_heap_free(fp)` (Phase 19 substrate), which atomically bumps the
  IronAllocHdr gen and frees the IronAllocHdr+value block. Double-free
  protection is automatic via Phase 19's gen-mismatch panic path.
- **Free target restriction (POL-04):** `free` accepts ONLY identifier
  (binding) targets. `free p.field` / `free arr[i]` / `free obj.method()`
  emit `IRON_ERR_FREE_NOT_BINDING` (E0274). Phase 24 may relax this once
  `drop` semantics arrive.
- **Leak:** `leak <binding>` marks an allocation intentionally permanent. Pure
  compile-time analyzer marker (NO runtime emission) — sets
  `Iron_Symbol.is_leaked = true` on the resolved binding. Phase 31 (DBG-05
  forgotten-`free` warning) reads this flag and SUPPRESSES the warning for
  leaked bindings.
- **Leak target restriction (POL-05):** `leak` accepts ONLY identifier
  (binding) targets, symmetric with `free`. Non-identifier targets emit
  `IRON_ERR_LEAK_NOT_BINDING` (E0275).
- **Defer-free idiom (DEFER-02):** `defer free <binding>` is the canonical
  scope-bound free idiom available in v3.0-alpha.1. Lowers through the existing
  IRON_HIR_STMT_DEFER → IRON_LIR_FREE → iron_heap_free path; the existing
  `emit_defer_cleanup` LIFO machinery in `src/hir/hir_to_lir.c` emits frees in
  LIFO order before every IRON_LIR_RETURN.
- **Defer form restriction:** All other defer forms emit
  `IRON_ERR_DEFER_FORM_UNSUPPORTED` (E0276) with hint forward-referencing Phase
  32 for full defer semantics. Plan 21-01 added the structural check at
  hir_lower.c's IRON_NODE_DEFER arm; the check fires BEFORE HIR emission to
  prevent invalid HIR nodes from reaching codegen.

### Codegen behavior (informative)

- `heap T(...)` lowers to
  `Iron_FatPtr _vN = iron_heap_alloc(__FILE__, __LINE__, sizeof(T))`
  followed by `*((T *)_vN.addr) = inner_value`. The local IS the Iron_FatPtr
  (16B value type), NOT a raw `T *`; this is the post-Phase-21 codegen
  invariant. All downstream dereferences emit `((T *)_vN.addr)` form (e.g.,
  `((T *)_vN.addr)->field` for field access, `*((T *)_vN.addr) = ...` for
  store).
- `free p` lowers to `iron_heap_free(_vN)` preceded by a
  `/* PHASE-24 HOOK: drop call insertion before iron_heap_free */` comment
  stub. Phase 24 will replace the stub with a destructor invocation when the
  freed type has a `drop { ... }` block.
- `leak p` produces NO LIR emission. The IRON_HIR_STMT_LEAK arm in
  hir_to_lir.c evaluates the expr (for any side-effects in `p`'s computation,
  though `p` is always an ident here) and discards output.
- `defer free p` is registered on the existing per-function `defer_stack`
  during HIR lowering; `emit_defer_cleanup` at hir_to_lir.c:~1730 emits LIFO
  `iron_heap_free` calls before every IRON_LIR_RETURN.
- `&p` for heap-rooted `p` (Phase 20 ADDR_OF) uses gen_source=HEAP path:
  `(Iron_FatPtr){ .addr = (void*)((T *)_vN.addr), .gen = _vN.gen }`. The
  `.gen` field reads directly from the Iron_FatPtr local — the legacy
  IronAllocHdr arithmetic gen-recovery is no longer applicable post-Phase-21
  codegen migration (the local IS the Iron_FatPtr).

### `auto_free` flag disconnection

The escape.c v1/v2 classifier sets `auto_free` on heap expressions that do not
escape. This flag flows into `IRON_LIR_HEAP_ALLOC.heap_alloc.auto_free`. Phase
21 EXPLICITLY DOES NOT connect this flag to automatic `iron_heap_free` emission
— emit_c.c at the HEAP_ALLOC site carries a
`/* PHASE-31: auto_free connects to DBG-05 forgotten-free warning; do NOT emit iron_heap_free here */`
documentation guard. Phase 31 will consume `auto_free` to drive the DBG-05
forgotten-`free` warning at lint-time.

### Diagnostic codes added (cross-reference)

See `docs/dev/diagnostic-codes.md` Phase 21 section for the full table. Codes
273-276 are reserved for Phase 21.

### Forward references

- Phase 22 (`readonly` purity tightening): readonly-compatible return-type
  checker covers heap allocations (heap allocation that escapes a readonly
  function is a violation per READ-04).
- Phase 24 (drop / copy / nocopy): replaces the `/* PHASE-24 HOOK */` stub at
  IRON_LIR_FREE with destructor invocation; the fixture
  `tests/integration/v4/3.2-heap/boundary_destructor_runs.iron` (retagged in
  Plan 21-01) flips XFAIL→GREEN when Phase 24 lands DROP-01.
- Phase 26 (`rc` policy): introduces `rc T(...)` allocation form; OQ-09 lock
  (NO `heap → rc` promotion) is consistent with Phase 26's POL-11
  closed-policy enforcement.
- Phase 31 (debug allocator + leak detection): consumes
  `Iron_Symbol.is_leaked` (Plan 21-02) and
  `IRON_LIR_HEAP_ALLOC.heap_alloc.auto_free` (escape.c) to drive DBG-05
  forgotten-`free` warning; full unification of escape.c with the Phase 21
  identifier-only path lands here.
- Phase 32 (defer statement): generalizes `defer` to all forms (LIFO across
  all forms, panic-safe, drop interaction); Phase 21's `defer free <binding>`
  becomes a special case of the general defer machinery.
- Phase 34 (LSP adaptation): consumes diagnostic-codes.md Phase 21
  quickfix-target column to wire LSP-06 quickfix actions.

### Phase 19 ABI substrate UNCHANGED

Phase 21 only CONSUMES existing Phase 19 surfaces. The following remain
UNCHANGED per the Stability Commitment in this document's Phase 19 sections:

- `Iron_FatPtr` 16B layout (`{ void *addr; uint64_t gen; }`)
- `IronAllocHdr` 16B release / 32B debug layout
- `iron_heap_alloc(file, line, size)` returning `Iron_FatPtr`;
  `iron_heap_free(Iron_FatPtr)`; `iron_check_pointer_gen` static-inline;
  `iron_panic_stale_pointer` panic format (text + JSON channels)
- Atomic semantics + memory ordering on the gen field

Phase 21 modifications are confined to `src/lir/emit_c.c` (codegen migration;
no Phase 19 source files touched), `src/parser/parser.c` +
`src/analyzer/typecheck.c` + `src/hir/hir_lower.c` +
`src/diagnostics/diagnostics.h` (frontend surface; no runtime files touched),
and `src/analyzer/scope.h` + `src/analyzer/resolve.c` (single bool field +
one-line setter; not in src/runtime/).
