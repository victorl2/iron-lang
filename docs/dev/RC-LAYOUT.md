# RC-LAYOUT.md — Iron `rc` Policy ABI Lock

**Phase 26 locked 2026-05-20; Phase 27 re-locked 2026-05-20 (24B header for weak rc). Mirror template: POINTER-LAYOUT.md, DROP-LAYOUT.md, UNCHECKED-LAYOUT.md.**

## Overview

Strong reference-counting policy for Iron user values:

```iron
val p = rc Point(1, 2)   // owning strong reference, refcount = 1
val q = p                // copy bumps refcount atomically to 2
                         // q and p both drop at scope-exit; final drop runs Point's destructor
```

Each binding holds an owning strong reference. Copy bumps the count atomically.
Drop decrements. Final drop runs the destructor (POL-06 + GA5) and frees the
block. `&` on an `rc` value is a compile error (POL-07, diagnostic E0296)
citing `weak rc` (Phase 27) as the alternative. Non-transitive (POL-10): outer
policy governs only its own struct memory; internal field allocations carry
their own policy independently.

Phase 26 ships the **strong** reference-counting half. Weak references and
`upgrade()` are Phase 27. Arena exclusion of rc and arena-aware allocation are
Phase 28. Refcount elision optimization is Phase 29. `arc` (lock-free atomic-
elided rc) is OQ-07, deferred to Phase 29.

The lifecycle policy set, locked at v3.0, is `{stack, heap, rc, weak rc}`.
`stack` is the implicit default at allocation expression; `weak rc` ships in
Phase 27. `arena` is keyword-reserved for the Phase 28 type-system Arena, NOT
a lifecycle policy. Plan 26-02 emits this canonical set in the E0298 parser
diagnostic.

## 1. `Iron_RcHeader` Layout

The runtime substrate is the `Iron_RcHeader` struct prepended before
`IronAllocHdr` (Phase 19) in every rc-allocated block:

```c
typedef struct Iron_RcHeader {
    iron_atomic_u64  refcount;    /* offset 0, 8 bytes — ABI-frozen (Phase 26).
                                     Relaxed-inc on retain, release-dec on
                                     release, acquire-fence on final drop. */
    void           (*drop_fn)(void *self);  /* offset 8, 8 bytes — ABI-frozen
                                               (Phase 26). <TypeName>_rc_drop
                                               trampoline (Plan 26-03); NULL
                                               for primitive payloads. */
    iron_atomic_u64  weak_count;  /* offset 16, 8 bytes — ABI-locked Phase 27.
                                     Starts at 1: the strong cohort owns one
                                     collective weak (Rust Arc scheme).
                                     Relaxed inc on downgrade; RELEASE dec on
                                     weak release. The weak counter's 1→0
                                     edge is the single free point. */
} Iron_RcHeader;
_Static_assert(sizeof(Iron_RcHeader) == 24, "Iron_RcHeader ABI re-lock — 24B on 64-bit POSIX/Win32");
```

**Block layout:**

```
+-------------------+  block start
| Iron_RcHeader     |   24 bytes (refcount + drop_fn + weak_count)
+-------------------+
| IronAllocHdr      |   16 bytes release / 32 bytes debug (gen + size [+ site])
+-------------------+
| user payload      |   `size` bytes (sizeof(T))
+-------------------+  block end
```

**User pointer** points at the payload start — Phase 19 ABI invariant preserved.

**Recovery:**

```c
Iron_RcHeader *iron_rc_header_of(void *user_ptr) {
    return (Iron_RcHeader *)((char *)user_ptr
                             - sizeof(IronAllocHdr)
                             - sizeof(Iron_RcHeader));
}
```

The Phase 19 substrate is **untouched**: `IronAllocHdr.gen` remains a
generation counter, NOT a refcount overlay. Any `*T` pointer derived from rc
internals walks the same `((IronAllocHdr*)addr) - 1` recipe used by Phase 19's
deref check. The rc header sits *outside* (before) the alloc header so the
existing Phase 19 arithmetic survives unchanged.

The reverse layout `[IronAllocHdr][Iron_RcHeader][payload]` would silently
break Phase 19's deref check on `*T` pointers derived from rc-allocated
memory. This is intentional and locked.

## 2. Allocation + Free Protocol

### `iron_rc_alloc(size, drop_fn)`

- `malloc(sizeof(Iron_RcHeader) + sizeof(IronAllocHdr) + size)` — single
  contiguous block. On allocation failure, `iron_oom_abort("iron_rc_alloc")`
  (matches Phase 19's deterministic-abort discipline).
- `refcount = 1` via `IRON_ATOMIC_U64_INIT(rch->refcount, 1)`.
- `drop_fn` stored in header (may be NULL for primitive payloads).
- `gen = 1` via `IRON_ATOMIC_U64_INIT(ahd->gen, 1)`; `size` recorded.
- Returns payload pointer (block + sizeof(Iron_RcHeader) + sizeof(IronAllocHdr)).

### `iron_rc_retain(user_ptr)`

- `IRON_ATOMIC_U64_FETCH_ADD_RELAXED(rch->refcount, 1)`.
- **Debug guard:** UINT64_MAX-1 saturation → `iron_oom_abort` deterministically.
  UINT64_MAX retains is physically unreachable (~5 billion years at 100M ops/sec),
  but the abort defines behavior over wrap.
- **NULL discipline:** NULL `user_ptr` is a no-op (Phase 19 mirror).

### `iron_rc_release(user_ptr)`

- `prev = IRON_ATOMIC_U64_FETCH_SUB_RELEASE(rch->refcount, 1)`.
- **Debug guard:** `prev == 0` → `iron_oom_abort` (underflow — programmer bug).
  iron_compiler's `iron_ice` is NOT linkable from `iron_runtime`, so the abort
  message preserves diagnostic intent via the runtime-canonical
  `iron_oom_abort`. Documented as a Plan 26-01 deviation.
- If `prev == 1`:
  - `IRON_ATOMIC_FENCE_ACQUIRE()` — synchronizes with prior releases from other
    threads so the destructor observes their writes.
  - `if (drop_fn) drop_fn(user_ptr);` — primitive-payload rc skips trampoline.
  - `free(block)` — frees the entire `[rc_header][alloc_header][payload]`.

### Atomic-ordering rationale (Rust Arc canonical)

- **Relaxed-inc on retain** (`memory_order_relaxed`): Copies don't observe
  ordering of unrelated writes. Monotonicity is the only invariant required.
- **Release-dec on release** (`memory_order_release`): Callers' prior writes
  synchronize-with the destructor that may eventually read them. Each
  holder's release-dec is the outgoing edge.
- **Acquire-fence on last-drop** (`memory_order_acquire`): When `fetch_sub`
  returns 1, this thread observed the linearization point. Emit an acquire
  fence before invoking the destructor so writes from other holders (which
  synchronized-with the releases via `memory_order_release`) are visible to
  `drop_fn`.

References:
- "Building Our Own Arc" — https://mara.nl/atomics/building-arc.html
- rust-lang/rust#62230 — original discussion of acquire fence
- C11 §7.17.4.3 (`atomic_fetch_sub_explicit`)

## 3. Non-Transitivity (POL-10)

**Outer-value policy governs only its own struct memory. Inner-field
allocations carry their own policy independently.**

The closed-policy lifecycle set is `{stack, heap, rc, weak rc}` (per ROADMAP
success criterion #4 + REQUIREMENTS POL-11). `stack` is the implicit default
at allocation expression; `weak rc` ships in Phase 27; `arena` is
keyword-reserved for the Phase 28 type-system Arena, NOT a lifecycle policy.
Plan 26-02 emits this canonical set in the E0298 parser diagnostic.

### Example: nested-policy isolation

```iron
object Container {
    val inner: heap Bar
}

func use_it() {
    val c = rc Container(heap Bar(...))
    // refcount = 1, c.inner holds a heap-allocated Bar
}  // c goes out of scope
   // refcount → 0
   // → Container_rc_drop invokes Container_drop (compiled user `drop {}` if any)
   // → Container_drop emits free chain for `inner: heap Bar` per Phase 24 DROP-02
   // → the inner heap Bar is freed because the user's drop body wires it,
   //   NOT because rc's free-block step cascades through fields
```

The final drop of the outer `rc` triggers refcount → 0 → `drop_fn`
(`Container_rc_drop` trampoline → `Container_drop`). The Phase 24 destructor
emits the free chain for `inner: heap Bar` as part of its compiled body. The
inner `heap Bar` is freed because the user's drop chain wires it — NOT because
rc's free-block step cascades through field types.

This is **non-transitive**: `rc` does not propagate down struct fields. Each
nested allocation manages its own lifecycle.

### 3.1 `rc Box[T]` interaction (rc + nocopy compatibility)

`Box[T]` is `nocopy` (Phase 24 / Phase 25). `rc T` requires copy semantics
(refcount-bump-on-copy). The combination is **rejected** by the existing
E0286 (copy of nocopy type) at the `rc` allocation site. No new diagnostic
code; the user gets an explicit nocopy-copy violation pointing at the
incompatible composition.

**Recommendation:** use either `rc T` (without Box wrapping) or `Box[T]`
(without rc wrapping). The two policies are alternatives, not composable.

Future direction: Phase 28 may relax this if `rc Box[T]` with
unique-ownership-in-rc semantics is justified. Not in v3.0.

## 4. Partial-Init Cleanup Extension

Cross-reference: DROP-LAYOUT.md §4 + `src/runtime/iron_panic.h:77-81`.

`IronInitCleanupEntry.drop_fn` is a function pointer. Phase 24 populated it
with `<TypeName>_drop` for object fields with destructors. **Phase 26 extends
the same mechanism**: for rc-typed fields, the partial-init cleanup pushes an
`iron_rc_release` entry instead. The runtime panic-trap then unwinds via the
rc-release path, properly decrementing the refcount of any rc-fields that
were initialized before the panic point.

This avoids leaking refcounts when:

```iron
object C {
    val a: rc Foo
    val b: rc Bar
}

init {
    self.a = rc Foo(...)   // partial-init pushes iron_rc_release(self.a)
    self.b = compute_bar() // panics here
}
// → panic-trap unwinds: pops cleanup stack, calls iron_rc_release on self.a
// → self.a's refcount decremented from 1 → 0 → drop_fn fires → block freed
```

**Note:** Per STATE.md [Phase 24-03], DROP-05 end-to-end coverage is deferred
to Phase 32 (init inlining limitation). Phase 26 extends the mechanism; the
end-to-end pump that drives `_init` function bodies remains future work. The
substrate is ready for the Phase 32 push.

**Phase 27 weak rc extension.** Weak rc fields participate in partial-init
cleanup via `iron_weak_rc_release` mirror of the rc path: for a field of
type `weak rc T` initialised before a mid-init panic, the cleanup stack
entry's `drop_fn` is `iron_weak_rc_release`. Same mechanism, same Phase 32
end-to-end coverage gap (init inlining limitation). Cross-reference:
DROP-05 PARTIAL in REQUIREMENTS.md.

## 5. Panic-Trap Reuse from Phase 24

Cross-reference: DROP-LAYOUT.md §5 + `src/lir/emit_c.c:5802-5810` (Phase 24
DROP-04 panic-trap prologue).

The Phase 24 `iron_in_destructor` TLS flag is set during the `<TypeName>_drop`
function's prologue (emitted by `emit_func_body`, Plan 24-03). The Phase 26
rc-drop trampoline `<TypeName>_rc_drop` is a `void *`-signed wrapper that
calls the same `<TypeName>_drop` — and therefore **inherits the Phase 24
panic-trap automatically** with no additional emission work.

```c
/* Synthesized by Plan 26-03 emit_ensure_rc_drop helper: */
static void <TypeName>_rc_drop(void *self_void) {
    <TypeName>_drop((<TypeName> *)self_void);   /* Phase 24 panic-trap inherits */
}
```

Panicking inside an rc-drop aborts via `iron_panic_destructor_aborted` — the
same handler installed by Phase 24 DROP-04 — because the underlying
`<TypeName>_drop` body has already set `iron_in_destructor = true`. Phase 26
does not need a new TLS flag and does not need a new panic handler.

**Debug visibility:** when an rc-drop panics, the panic message includes the
`iron_current_dropping_type` field (set by Phase 24 prologue) which names
exactly which object's destructor was executing. This is the same diagnostic
surface as plain heap-drop panics; no Phase 26 extension required.

## 6. Closure Interaction (OQ-03)

Closures capturing `rc T` by value follow the Phase 20 OQ-02 universal-path
discipline (value-type capture via `emit_capture_rhs` at
`src/lir/emit_c.c:110-128`) extended with synthesized retain/release at the
closure's lifecycle boundaries:

- **At closure construction:** `iron_rc_retain(captured_field)` — one retain
  per captured rc field per new closure instance. This is the "construction
  retain". The captured-state struct is allocated and field-initialized; the
  retain is emitted alongside the field-initializer.
- **At closure copy:** synthesized copy block calls `iron_rc_retain` on each
  captured rc field. This is the "shallow-copy retain", extending the
  Phase 24 closure copy block synthesis. Each closure-instance copy is a
  shallow C struct-copy of the captured-state struct, and the synthesized
  copy block adds the rc-retains to balance the future drops.
- **At closure drop:** synthesized drop block calls `iron_rc_release` on each
  captured rc field. This is the "destruction release", extending the
  Phase 24 closure drop block synthesis. Each closure-instance drop releases
  exactly the references it owns.

**Invariant:** `count(retains) == count(releases)` per captured rc field
across the closure's entire lifetime. This is the codegen-balance pin that
Plan 26-03 enforces via a unit test on
`tests/integration/v4/4.8-rc-policy/closure_captures_rc.iron`.

**Anti-pattern:** double-retain at construction (once for "new captured-state
struct", once for "Phase 24 copy block treats this as a copy"). Phase 26
explicitly separates the two paths: construction-retain runs ONCE per
closure-instance creation, copy-retain runs ONCE per closure-instance copy.
The captured-state struct is **constructed** the first time and **copied**
on every subsequent assignment.

Plan 26-03 wires the LIR-level synthesis in `emit_helpers.c` alongside the
Phase 24 copy/drop block emission, with the dedup pin
`emitted_rc_drops` mirroring Phase 24's `emitted_drops`.

## 7. Phase 27 weak rc — Live Spec

Phase 27 lands `weak rc T` (POL-08) and `upgrade(): T?` (POL-09). The
`Iron_RcHeader` structure is now 24B with the new `weak_count` field
appended at offset 16. The Phase 26 ABI invariants are preserved verbatim:

- (a) `refcount` stays at offset 0 (relaxed-inc / release-dec hot path
  unchanged; required by the Phase 29 elision optimizer's pattern-matching).
- (b) `drop_fn` stays at offset 8 (final-drop call site; required by the
  static-dispatch convention locked by GA5 in Phase 26).
- (c) `weak_count` appended at offset 16 (relaxed inc / RELEASE dec;
  initialised to 1 — see below).

**Block free condition:** the `weak_count` 1→0 edge. `iron_rc_alloc`
initialises `weak_count = 1`: the strong cohort collectively owns ONE weak
reference (Rust Arc's scheme, `library/alloc/src/sync.rs`). The final
strong release runs `drop_fn` and then releases that collective weak; the
thread whose weak decrement takes the counter to 0 frees the block. A
SINGLE counter therefore linearizes the free.

> Historical note: the original Phase 27 protocol ("free when
> `weak_count == 0 AND strong_count == 0`, whichever counter trips zero
> last frees") was a check-then-act race across two atomics — a racing
> last-strong / last-weak release pair could each decrement its own
> counter, then each observe the other counter at 0, and both free the
> block (or the weak side could free while `drop_fn` was still running).
> The collective-weak scheme closes that window.

The intermediate state (strong=0, real weak refs > 0) means the payload
destructor has run but the header persists so weak refs remain readable —
upgrade() observes refcount==0 and returns NULL (safe; no UAF).

The strong-rc trio (`iron_rc_alloc`, `iron_rc_retain`, `iron_rc_release`)
remains the Phase 26 substrate; Phase 27 adds the weak-side quartet
(`iron_weak_rc_retain`, `iron_weak_rc_release`, `iron_rc_downgrade`,
`iron_rc_upgrade`). See §8 for the semantics + upgrade race state diagram
and §9 for the closure-capture-of-weak-rc invariant.

## 8. Weak Count Semantics + Upgrade Race

### 8.1 API summary

| Function                  | Strong/Weak | Returns                          | Effect                                          |
|---------------------------|-------------|----------------------------------|-------------------------------------------------|
| `iron_rc_alloc(s, drop)`  | strong=1, weak=1 | user_ptr                    | Allocates block; weak=1 is the strong cohort's collective weak |
| `iron_rc_retain(p)`       | strong+=1   | void                             | Relaxed fetch_add on refcount                   |
| `iron_rc_release(p)`      | strong-=1   | void                             | Release fetch_sub; on prev==1: acquire fence + drop_fn + release the collective weak (`iron_weak_rc_release`) |
| `iron_rc_downgrade(p)`    | weak+=1     | user_ptr (same)                  | Relaxed fetch_add on weak_count                 |
| `iron_weak_rc_retain(p)`  | weak+=1     | void                             | Relaxed fetch_add on weak_count                 |
| `iron_weak_rc_release(p)` | weak-=1     | void                             | RELEASE fetch_sub; on prev==1: acquire fence + free(block) |
| `iron_rc_upgrade(p)`      | strong+=1 (race-won) | user_ptr OR NULL        | Rust-Arc CAS loop; NULL on observed refcount==0 |

`iron_rc_downgrade` and `iron_weak_rc_retain` are semantically equivalent
for an existing live block; the distinction is type-system level —
`downgrade` converts the static type `rc T` to `weak rc T` and is the
entry point from compiler-emitted strong-to-weak transitions, while
`iron_weak_rc_retain` is invoked by Phase 27 OQ-04 closure-capture code
synthesis on a value already typed `weak rc T`.

### 8.2 Block lifecycle state machine

Adapted from Mara Bos *Rust Atomics and Locks* Ch. 6 + Rust
`library/alloc/src/sync.rs` Weak::upgrade:

```
State 1: ALIVE
  - strong_count > 0, weak_count >= 0
  - Payload valid; header valid; all operations supported.

State 2: PAYLOAD_DESTROYED (strong=0, weak>0)
  - drop_fn has run; user payload memory is logically dead but the block
    is NOT yet freed.
  - Header (Iron_RcHeader) still resident; weak refs remain readable.
  - upgrade() loads refcount and observes 0 -> returns NULL safely.
  - downgrade() N/A (no strong handle exists at this state).

State 3: HEADER_FREED (weak=0; strong necessarily 0)
  - Block malloc-freed; ALL pointers (strong and weak) dangling.
  - Reached exclusively via the weak_count 1→0 decrement in
    iron_weak_rc_release (the collective weak guarantees strong hit 0
    and drop_fn completed before weak can reach 0).

TRANSIENT: in-destructor (strong=0, weak>=0, drop_fn running)
  - Covered by Phase 24 iron_in_destructor TLS flag.
  - upgrade() during this window observes refcount==0 -> returns NULL.
  - Phase 26 DROP-04 panic-trap covers panicking destructor.
```

**Transition gates:**

- `strong -> 0`: in `iron_rc_release` when prev==1. Acquire-fence + drop_fn,
  then release the collective weak via `iron_weak_rc_release` (weak-=1).
  If that decrement is the 1→0 edge the same call frees; otherwise real
  weak holders keep the header alive.
- `weak -> 0`: in `iron_weak_rc_release` when prev==1 (RELEASE fetch_sub;
  acquire fence before the free). Triggers `free(block)` unconditionally —
  weak reaching 0 implies the collective weak was already released, i.e.
  strong hit 0 and drop_fn completed.

### 8.3 Upgrade race state diagram (POL-09 critical correctness)

```
Thread A (holds last strong)     Thread B (holds weak, wants upgrade)
─────────────────────────────    ─────────────────────────────────────
FETCH_SUB_RELEASE(refcount, 1)
  prev = 1                       LOAD_ACQUIRE(refcount)
                                   sees 0  -> return NULL    [safe]
                                 OR
                                   sees N>0 -> CAS(refcount, N, N+1)
                                     fails (A's dec committed) -> loop
                                     loads 0 -> return NULL  [safe]
                                 OR
                                   sees N>0 -> CAS(refcount, N, N+1)
                                     succeeds (race won)
                                     return user_ptr         [safe;
                                                              A has not
                                                              yet hit
                                                              prev==1]
ACQUIRE_FENCE
drop_fn(user_ptr)
FETCH_SUB_RELEASE(weak_count, 1)   // release the collective weak
  prev == 1 -> ACQUIRE_FENCE; free(block)
  prev >  1 -> block persists — B still holds a weak handle; B's final
               iron_weak_rc_release performs the free on its 1→0 edge
```

The acquire-load + relaxed/relaxed CAS pattern guarantees: B never reserves
a strong ref to a soon-to-be-destructed payload. **CAS-success implies B
observed refcount > 0 strictly AFTER A's release-dec** (otherwise B's
acquire-load would have synchronized with A's release-dec and seen 0).
If A's release-dec lands BEFORE B's CAS commits, A's prev returns
something > 1 and A skips the destructor — B's bumped ref is then the new
last-strong and A's release counts as a non-final release.

### 8.4 Memory ordering rationale — Iron diverges from Mara Bos

Mara Bos's *Rust Atomics and Locks* Chapter 6 ("Building Our Own Arc")
uses **Acquire on weak_count downgrade-inc** and **Release on weak_count
drop-dec**. Iron uses **Relaxed on inc** and — like Mara/Rust — **Release
on dec** (with an acquire fence on the freeing 1→0 edge).

**Why the inc can stay Relaxed:** Mara's Acquire on the weak-inc exists
specifically to synchronize with `Arc::get_mut`, which checks
`weak_count == 1 && strong_count == 1` and needs both counters observed
consistently to make exclusive-access claims about the inner value. Iron
v3.0 does **not** surface get_mut-style exclusive-access APIs (no
`Arc::make_mut`, no `Arc::get_mut`); `rc T` is shared by design, and
`*T` access goes through Phase 19's generation-tracked deref check, not
through refcount-based exclusivity.

**Why the dec must be Release (+ acquire fence before free):** the thread
that frees on the 1→0 edge must observe every other holder's last header
access. Each holder's Release-dec pairs with the freeing thread's acquire
fence, ordering all prior accesses (including drop_fn, which the final
strong release sequences before its collective-weak dec) before the
`free(block)`. A Relaxed dec would let another holder's header access
race the deallocation.

### 8.5 References

- Mara Bos, *Rust Atomics and Locks* Ch. 6 — https://marabos.nl/atomics/building-arc.html
- Rust std `library/alloc/src/sync.rs` — `Weak::upgrade` (canonical CAS loop)
- RustBelt-Relaxed: Ralf Jung, "The Tale of a Bug in Arc" — https://www.ralfj.de/blog/2018/07/19/arc.html
- C11 §7.17.4.3 (`atomic_compare_exchange_weak_explicit`)

## 9. Closure Capture of `weak rc` (OQ-04)

Phase 27 mirrors Phase 26 OQ-03 closure-capture verbatim with weak-rc
opcodes. The pattern (closure captures a weak rc field by value):

- **At closure construction:** synthesized retain block calls
  `iron_weak_rc_retain` per captured `weak rc T` field — one bump per new
  closure instance. The captured-state struct is allocated and field-
  initialized; the retain is emitted alongside the field-initializer.
- **At closure copy:** synthesized copy block calls
  `iron_weak_rc_retain` on each captured weak-rc field. Each closure-
  instance copy is a shallow C struct-copy of the captured-state struct;
  the synthesized copy block adds the retains to balance the future drops.
- **At closure drop:** synthesized drop block calls
  `iron_weak_rc_release` on each captured weak-rc field. Each closure-
  instance drop releases exactly the weak refs it owns.

**Invariant** (mirror of Phase 26 OQ-03 Major #5 hardened invariant):

```
count(iron_weak_rc_release)
    == count(iron_weak_rc_retain) + count(iron_rc_downgrade)
```

The asymmetric ground truth (downgrade creates +1 weak_count with no
paired retain emit) matches Phase 26's "alloc creates refcount=1 with no
paired retain" precedent. Verified by the codegen invariant test
`tests/unit/test_oq04_closure_weak_rc_retain_release.c`.

**Implementation sites** (Plan 27-03 Task 1):

- `src/hir/hir_to_lir.c` IRON_HIR_EXPR_CLOSURE arm (current pinned line
  1629; mirrors Phase 26 OQ-03 retain block).
- `src/lir/emit_c.c` IRON_LIR_MAKE_CLOSURE arm (current pinned line 4327;
  mirrors Phase 26 Approach A env_drop synthesis).

The synthesized companion `<func_name>_env_drop` releases each weak-rc
env field via `iron_weak_rc_release`. The closure's drop_fn pointer is
filled with this synthesized companion at MAKE_CLOSURE time.

## 10. Stability Commitment

Iron v3.0+:

- `Iron_RcHeader` field layout (refcount @ 0, drop_fn @ 8, weak_count @ 16)
  is **ABI-frozen**. Programs compiled against v3.0 must continue to
  interoperate at the binary level with v3.x runtimes.
- The retain/release atomic discipline (relaxed-inc / release-dec /
  acquire-fence on last-drop for strong; relaxed inc/dec for weak) is
  **correctness-frozen**. Alternative orderings are out of scope; the
  elision optimizer (Phase 29) operates on pair-pattern matching (matched
  retain/release pairs across a function body), NOT on weakening
  individual atomic operations.
- The block layout `[Iron_RcHeader][IronAllocHdr][payload]` is **ABI-frozen**.
  The user pointer + Phase 19 deref-check invariant is preserved.
- The block free condition `weak_count == 0 AND strong_count == 0` is
  **correctness-frozen**. Phase 28 arena interaction MUST preserve this
  predicate.

Any future change requires:

- Bump `IRON_VERSION` major.
- Update this file's "locked" version stamp.
- Regression-test that old `<TypeName>_rc_drop` trampolines remain
  ABI-compatible (binary-format check of `Iron_RcHeader.drop_fn` offset on
  the new layout).
- Provide a migration path for compiled binaries — either a runtime shim that
  detects the old layout via the gen field's well-known initial value, or a
  recompilation requirement documented in the major-version release notes.

Cross-reference: REQUIREMENTS POL-06 (allocation form), POL-07 (no `&` on
rc/weak rc), POL-08 (weak rc non-owning), POL-09 (upgrade against last
drop), POL-10 (non-transitivity), POL-11 (closed-policy lifecycle set),
OQ-03 (closure rc semantics), OQ-04 (closure weak rc semantics). The Iron
v3.0 milestone success criterion #4 (closed policy set =
`{stack, heap, rc, weak rc}`) is locked here.
