#ifndef IRON_RUNTIME_H
#define IRON_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>  /* malloc/free — required on Windows because
                      * WIN32_LEAN_AND_MEAN strips transitive includes */

#include "runtime/iron_errors.h"
#include "diagnostics/diagnostics.h"  /* iron_oom_abort for IRON_LIST/MAP/SET OOM paths (FIX-01, Phase 67) */

/* ── Platform atomic abstraction ────────────────────────────────────────── */
#ifdef _WIN32
  /* Include winsock2.h BEFORE windows.h to avoid the winsock v1 vs v2
   * struct-redefinition clash (sockaddr, fd_set, WSAData, etc.). windows.h
   * by default pulls in winsock.h (v1) when WIN32_LEAN_AND_MEAN is not
   * defined, and then a later winsock2.h include conflicts. Including
   * winsock2.h first marks those types as defined; the subsequent
   * windows.h sees them already declared and skips the winsock.h path.
   * Required by Phase 59 network code that lives in translation units
   * which transitively include this header. */
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <windows.h>
  typedef volatile LONG iron_atomic_int;
  #define IRON_ATOMIC_INIT(v, val)          ((v) = (val))
  #define IRON_ATOMIC_LOAD(v)               InterlockedCompareExchange(&(v), 0, 0)
  #define IRON_ATOMIC_FETCH_ADD(v, n)       InterlockedExchangeAdd(&(v), (n))
  #define IRON_ATOMIC_FETCH_SUB(v, n)       InterlockedExchangeAdd(&(v), -(n))
  #define IRON_ATOMIC_CAS_WEAK(v, exp, des) iron__win_cas(&(v), (exp), (des))
  static inline bool iron__win_cas(volatile LONG *v, int *expected, int desired) {
      LONG old = InterlockedCompareExchange(v, desired, *expected);
      if (old == *expected) return true;
      *expected = (int)old;
      return false;
  }
#else
  #include <stdatomic.h>
  typedef atomic_int iron_atomic_int;
  #define IRON_ATOMIC_INIT(v, val)          atomic_init(&(v), (val))
  #define IRON_ATOMIC_LOAD(v)               atomic_load(&(v))
  #define IRON_ATOMIC_FETCH_ADD(v, n)       atomic_fetch_add(&(v), (n))
  #define IRON_ATOMIC_FETCH_SUB(v, n)       atomic_fetch_sub(&(v), (n))
  #define IRON_ATOMIC_CAS_WEAK(v, exp, des) atomic_compare_exchange_weak(&(v), (exp), (des))
#endif

/* ── Phase 19: 64-bit atomic abstraction with explicit memory ordering ──────
 * Generation counters need explicit relaxed/acquire ordering. The existing
 * IRON_ATOMIC_* macros above use sequentially-consistent ops (the C11 default
 * for *_explicit-less APIs); over-strong for monotonic counters. This parallel
 * family wraps atomic_*_explicit on POSIX and Win64 Interlocked*64 (which are
 * unconditionally seq-cst — acceptable since Iron's parallel-LSP-request
 * model is not bottlenecked on heap-tracker atomics, and Windows is excluded
 * from CI today).
 *
 * CONTEXT-locked: relaxed-fetch-add on free, acquire-load on deref-check.
 * Do NOT replace with the seq-cst IRON_ATOMIC_* macros above. */
#ifdef _WIN32
  typedef volatile LONG64 iron_atomic_u64;
  #define IRON_ATOMIC_U64_INIT(v, val) \
      ((v) = (LONG64)(val))
  #define IRON_ATOMIC_U64_LOAD_ACQUIRE(v) \
      ((uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&(v), 0, 0))
  #define IRON_ATOMIC_U64_FETCH_ADD_RELAXED(v, n) \
      ((uint64_t)InterlockedExchangeAdd64((volatile LONG64 *)&(v), (LONG64)(n)))
  /* Phase 26 POL-06: release-decrement + acquire-fence for rc final-drop.
   * Interlocked*64 on Win32 are unconditionally seq-cst (stronger than
   * release/acquire), so the "release" semantics are trivially satisfied
   * and the acquire-fence is a no-op. The macro family is added for
   * source-level parity with the POSIX path; codegen and call-site shape
   * are identical across platforms. */
  #define IRON_ATOMIC_U64_FETCH_SUB_RELEASE(v, n) \
      ((uint64_t)InterlockedExchangeAdd64((volatile LONG64 *)&(v), -(LONG64)(n)))
  /* Phase 27 GA1: relaxed fetch_sub for weak_count (and any future
   * monotonic-decrement counter that does NOT carry destructor-sync
   * semantics). Interlocked*64 on Win32 is unconditionally seq-cst,
   * which trivially satisfies relaxed; source-level parity with the
   * POSIX path preserved. */
  #define IRON_ATOMIC_U64_FETCH_SUB_RELAXED(v, n) \
      ((uint64_t)InterlockedExchangeAdd64((volatile LONG64 *)&(v), -(LONG64)(n)))
  /* Phase 27 GA2: u64 CAS for the Rust-Arc-canonical upgrade loop. Win32
   * InterlockedCompareExchange64 is seq-cst (stronger than relaxed/relaxed);
   * acceptable since Windows is excluded from CI today (CLAUDE.md tech-stack
   * Windows-excluded policy). */
  static inline bool iron__win_cas_u64_relaxed(volatile LONG64 *v,
                                                uint64_t *expected,
                                                uint64_t desired) {
      LONG64 old = InterlockedCompareExchange64(v, (LONG64)desired,
                                                (LONG64)*expected);
      bool ok = (old == (LONG64)*expected);
      if (!ok) *expected = (uint64_t)old;
      return ok;
  }
  #define IRON_ATOMIC_U64_CAS_WEAK_RELAXED(v, exp, des) \
      iron__win_cas_u64_relaxed((volatile LONG64 *)&(v), (exp), (des))
  #define IRON_ATOMIC_FENCE_ACQUIRE() \
      ((void)0)  /* Interlocked*64 on Win32 are unconditionally seq-cst */
#else
  /* <stdatomic.h> already included on POSIX path above. */
  typedef _Atomic uint64_t iron_atomic_u64;
  #define IRON_ATOMIC_U64_INIT(v, val) \
      atomic_init(&(v), (val))
  #define IRON_ATOMIC_U64_LOAD_ACQUIRE(v) \
      atomic_load_explicit(&(v), memory_order_acquire)
  #define IRON_ATOMIC_U64_FETCH_ADD_RELAXED(v, n) \
      atomic_fetch_add_explicit(&(v), (n), memory_order_relaxed)
  /* Phase 26 POL-06: release-decrement + acquire-fence for rc final-drop.
   *
   * Atomic discipline (Rust Arc canonical, cross-verified RustBelt-Relaxed +
   * mara.nl "Building Our Own Arc"):
   *   retain:      FETCH_ADD_RELAXED on refcount (monotonic increment).
   *   release:     FETCH_SUB_RELEASE on refcount (callers' prior writes
   *                synchronize-with the eventual destructor).
   *   final-drop:  when fetch_sub returns 1, FENCE_ACQUIRE before invoking
   *                drop_fn so the destructor observes writes from other
   *                holders.
   *
   * References: https://mara.nl/atomics/building-arc.html ;
   * https://github.com/rust-lang/rust/issues/62230 . */
  #define IRON_ATOMIC_U64_FETCH_SUB_RELEASE(v, n) \
      atomic_fetch_sub_explicit(&(v), (n), memory_order_release)
  /* Phase 27 GA1: relaxed fetch_sub for weak_count. weak ops are
   * non-synchronizing — they only require monotonicity. The block-free
   * guards (iron_rc_release and iron_weak_rc_release) carry the cross-
   * counter acquire-load on the OTHER counter, so the relaxed dec on
   * weak_count is sufficient. */
  #define IRON_ATOMIC_U64_FETCH_SUB_RELAXED(v, n) \
      atomic_fetch_sub_explicit(&(v), (n), memory_order_relaxed)
  /* Phase 27 GA2: u64 CAS for the Rust-Arc-canonical upgrade loop. Relaxed
   * on both success and failure paths — the acquire-load preceding the
   * loop already established the happens-before edge with the prior
   * release-dec from the strong holder; the CAS itself does not need to
   * be a synchronizing operation. */
  #define IRON_ATOMIC_U64_CAS_WEAK_RELAXED(v, exp, des) \
      atomic_compare_exchange_weak_explicit(&(v), (exp), (des), \
                                            memory_order_relaxed, \
                                            memory_order_relaxed)
  #define IRON_ATOMIC_FENCE_ACQUIRE() \
      atomic_thread_fence(memory_order_acquire)
#endif

/* ── Phase 19: Generational pointer infrastructure (heap-only this phase) ──
 * Phase 20 surfaces *T / *var T to Iron source; Phase 19 lands the runtime
 * substrate Phase 20 will codegen against.
 *
 * Public ABI commitment: Iron_FatPtr is exactly 16B (8B addr + 8B gen).
 * Documented in docs/dev/POINTER-LAYOUT.md (Plan 19-03). Future changes
 * require explicit version bump + migration plan.
 *
 * gen=0 reserved as null/freed sentinel; first valid generation is 1. */

typedef struct {
    void     *addr;   /* points to user payload; header at addr - sizeof(IronAllocHdr) */
    uint64_t  gen;    /* generation captured at pointer-creation time */
} Iron_FatPtr;

_Static_assert(sizeof(Iron_FatPtr) == 16,
               "Iron_FatPtr must be 16B — System V AMD64 / AAPCS ARM64 "
               "2-register pass-by-value lock; growing past 16B is a "
               "silent perf regression on every pointer pass. "
               "See docs/dev/POINTER-LAYOUT.md for the public ABI commitment.");

/* IronAllocHdr is the header prepended to every iron_heap_alloc'd block.
 * Release build: 16B; Debug build (IRON_DEBUG_ALLOCATOR): 32B with site
 * capture. Both sizes yield 16B-aligned user pointer (sizeof is multiple
 * of 16) given malloc's max_align_t alignment guarantee.
 *
 * Phase 31 may extend the debug section with poison/double-free fields;
 * the release layout (16B) is locked by Plan 19-01 and changing it
 * requires the public ABI bump documented in POINTER-LAYOUT.md. */
typedef struct IronAllocHdr {
    iron_atomic_u64 gen;        /* atomic generation counter (relaxed inc, acquire load) */
    uint64_t        size;       /* user payload size in bytes (arena accounting + free validation) */
#ifdef IRON_DEBUG_ALLOCATOR
    const char     *alloc_site_file;  /* string-literal __FILE__ pointer; no strdup */
    uint32_t        alloc_site_line;  /* __LINE__ */
    uint32_t        alloc_id;         /* unique id from iron_alloc_id_counter (Phase 31 leak detector reuses) */
    /* ── Phase 31 GA1 (Plan 31-01) — debug-allocator extension ──────────────
     * The debug header GROWS 32B → 64B (16-multiple preserved) to carry the
     * intrusive leak registry links + the free-site for double-free both-sites
     * reporting. Release layout (16B) is UNCHANGED. */
    struct IronAllocHdr *reg_next;    /* DBG-03: intrusive doubly-linked registry next */
    struct IronAllocHdr *reg_prev;    /* DBG-03: intrusive doubly-linked registry prev */
    const char          *free_site_file;  /* DBG-04: first free-site __FILE__; NULL until first free */
    uint32_t             free_site_line;  /* DBG-04: first free-site __LINE__ */
    uint32_t             _pad;            /* keep sizeof a 16-multiple (64B total) */
    /* Total debug-build size: 16B (gen+size) + 16B (Phase 19 site fields)
     * + 16B (reg_next/reg_prev) + 16B (free_site_file+line+pad) = 64B.
     * Phase 31 ABI bump documented in POINTER-LAYOUT.md. */
#endif
} IronAllocHdr;

#ifdef IRON_DEBUG_ALLOCATOR
  _Static_assert(sizeof(IronAllocHdr) == 64,
                 "IronAllocHdr (debug) must be 64B — Plan 31 layout re-lock "
                 "(Phase 31 ABI bump: registry links + free-site appended)");
#else
  _Static_assert(sizeof(IronAllocHdr) == 16,
                 "IronAllocHdr (release) must be 16B — Plan 19-01 layout lock");
#endif

/* ── Phase 26 POL-06 + Phase 27 GA1: rc/weak-rc policy refcount header ──────
 * Block layout: [Iron_RcHeader][IronAllocHdr][user payload].
 * User pointer points at payload start — Phase 19 ABI invariant preserved.
 * Recovery: iron_rc_header_of(user) walks back
 *   sizeof(IronAllocHdr) + sizeof(Iron_RcHeader).
 *
 * Field-layout lock (24B on 64-bit POSIX + Win32 — Phase 27 ABI re-lock):
 *   offset 0:  refcount    (8B atomic u64 — ABI-frozen Phase 26;
 *              relaxed-inc on retain, release-dec + acquire-fence
 *              on final drop)
 *   offset 8:  drop_fn     (8B function pointer — ABI-frozen Phase 26;
 *              <TypeName>_rc_drop trampoline synthesized in Plan 26-03;
 *              NULL for primitive payloads with no user destructor)
 *   offset 16: weak_count  (8B atomic u64 — Phase 27 GA1 lock; relaxed
 *              inc/dec; block free condition is
 *              weak_count == 0 AND refcount == 0)
 *
 * Lock-document: docs/dev/RC-LAYOUT.md §1 + §7 + §8 (Phase 27 closeout).
 * Non-transitivity (POL-10): outer rc policy governs only its own
 * struct memory; internal field allocations carry their own policy.
 *
 * Phase 27 GA1: weak_count appended at offset 16; relaxed/relaxed for inc
 * and dec per CONTEXT.md GA1 (Iron does not surface get_mut so Mara Bos's
 * Acquire/Release pairing is not needed — see RC-LAYOUT.md §8 for rationale). */
typedef struct Iron_RcHeader {
    iron_atomic_u64  refcount;
    void           (*drop_fn)(void *self);
    iron_atomic_u64  weak_count;   /* Phase 27 GA1 — relaxed inc/dec; CONTEXT.md GA1 */
} Iron_RcHeader;

_Static_assert(sizeof(Iron_RcHeader) == 24,
               "Iron_RcHeader ABI re-lock — 24B on 64-bit POSIX/Win32 "
               "(Phase 27 weak_count append). "
               "See docs/dev/RC-LAYOUT.md §1 for the public commitment.");
_Static_assert(offsetof(Iron_RcHeader, refcount)   == 0,
               "refcount@0  ABI-frozen (Phase 26)");
_Static_assert(offsetof(Iron_RcHeader, drop_fn)    == 8,
               "drop_fn@8   ABI-frozen (Phase 26)");
_Static_assert(offsetof(Iron_RcHeader, weak_count) == 16,
               "weak_count@16 Phase 27 ABI lock");

/* Public API — definitions in src/runtime/iron_heap_track.c. */
Iron_FatPtr iron_heap_alloc(const char *site_file, int site_line, size_t size);
void        iron_heap_free(Iron_FatPtr fp);

/* ── Phase 31 GA1 (Plan 31-01) — debug-allocator surface ───────────────────
 * All of the following are debug-build-only behaviorally; the SYMBOLS are
 * declared/defined under IRON_DEBUG_ALLOCATOR so a release build never carries
 * the registry/poison/double-free machinery (release header is 16B and has no
 * registry slots — see the #else _Static_assert above).
 *
 *   iron_heap_free_dbg  — debug-gated free with an explicit free-site; codegen
 *                         (emit_c.c) emits this under IRON_DEBUG_ALLOCATOR so a
 *                         double-free reports the SECOND/current free-site too.
 *                         iron_heap_free(fp) forwards to (fp, NULL, 0) in debug.
 *   iron_leak_dump      — atexit handler (registered in iron_runtime_init);
 *                         walks the registry and reports still-live allocations
 *                         to STDERR with their alloc-site provenance (DBG-03).
 *   iron_debug_alloc_init — idempotent registry-lock initializer (DBG-03).
 */
#ifdef IRON_DEBUG_ALLOCATOR
void iron_heap_free_dbg(Iron_FatPtr fp, const char *free_file, int free_line);
void iron_leak_dump(void);
void iron_debug_alloc_init(void);
#endif

/* Phase 31 DBG-04: double-free panic — reports BOTH the first free-site (held
 * in the header) and the second/current free-site, plus the alloc-site. noreturn
 * (abort). Definition in src/runtime/iron_panic.c (Plan 31-01 Task 3). Mirrors
 * iron_panic_stale_pointer's no-malloc, stderr-only, dual text/JSON discipline.
 * Declared unconditionally (symbol harmless in release; only CALLED in debug). */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_double_free(const char *first_free_file,
                            int first_free_line,
                            const char *second_free_file,
                            int second_free_line,
                            const struct IronAllocHdr *hdr);

/* Forward declaration — definition lands in Plan 19-02 (src/runtime/iron_panic.c).
 * Declared here so the static-inline iron_check_pointer_gen below can call it
 * without needing diagnostics.h transitively included by every iron_runtime.h
 * consumer. Plan 19-02 also adds the canonical declaration in
 * src/diagnostics/diagnostics.h next to iron_oom_abort. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_stale_pointer(const char *deref_file,
                              int deref_line,
                              const IronAllocHdr *hdr);

/* ── Phase 30 OPT-08 input: opt-in generation-check counter ──────────────────
 *
 * Behind IRON_GENCHECK_COUNT (OFF by default), three `_Atomic uint64_t`
 * counters increment (relaxed) inside the three static-inline deref guards
 * below — one per generation source (heap / stack / arena). They feed the
 * deferred OPT-08 published elision-rate report (Phase 36; docs/dev/
 * POINTER-CHECK-ELISION.md). The macro must NOT alter the hot path in normal
 * builds — when undefined, IRON_GENCHECK_COUNT_BUMP_* expand to nothing and the
 * accessor reports zeros, so production deref performance and the deterministic
 * phase-invariant test counts are unaffected.
 *
 * The counter storage + the always-declared accessors are defined out-of-line
 * in src/runtime/iron_gencheck_count.c (mirroring iron_rc.c's IRON_RC_COUNT
 * block). Relaxed ordering is correct for a pure observation counter (no
 * happens-before obligation), mirroring the iron_rc.c counter discipline. */
#ifdef IRON_GENCHECK_COUNT
extern iron_atomic_u64 iron_gencheck_heap_count;
extern iron_atomic_u64 iron_gencheck_stack_count;
extern iron_atomic_u64 iron_gencheck_arena_count;
#  define IRON_GENCHECK_COUNT_BUMP_HEAP() \
       (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(iron_gencheck_heap_count, 1)
#  define IRON_GENCHECK_COUNT_BUMP_STACK() \
       (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(iron_gencheck_stack_count, 1)
#  define IRON_GENCHECK_COUNT_BUMP_ARENA() \
       (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(iron_gencheck_arena_count, 1)
#else
#  define IRON_GENCHECK_COUNT_BUMP_HEAP()  ((void)0)
#  define IRON_GENCHECK_COUNT_BUMP_STACK() ((void)0)
#  define IRON_GENCHECK_COUNT_BUMP_ARENA() ((void)0)
#endif

/* Always-declared (stable symbols). When IRON_GENCHECK_COUNT is undefined all
 * out-params receive 0 and reset is a no-op. NULL out-params are tolerated. */
void iron_gencheck_counts(uint64_t *heap, uint64_t *stack, uint64_t *arena);
void iron_gencheck_counts_reset(void);

/* Static-inline so Phase 30 optimizer can elide redundant checks at the
 * call site. Iron's release codegen will inline this trivially.
 * CONTEXT-locked: do not change to out-of-line without coordinating with
 * Phase 30 (POINTER-LAYOUT.md API surface). */
static inline void iron_check_pointer_gen(Iron_FatPtr fp,
                                          const char *deref_file,
                                          int deref_line) {
    IRON_GENCHECK_COUNT_BUMP_HEAP();  /* Phase 30 OPT-08: no-op unless IRON_GENCHECK_COUNT */
    if (!fp.addr) {
        iron_panic_stale_pointer(deref_file, deref_line, NULL);
    }
    IronAllocHdr *hdr = ((IronAllocHdr *)fp.addr) - 1;
    uint64_t cur = IRON_ATOMIC_U64_LOAD_ACQUIRE(hdr->gen);
    if (cur != fp.gen) {
        iron_panic_stale_pointer(deref_file, deref_line, hdr);
    }
}

/* Process-global allocation-id counter; bumped per alloc in debug builds.
 * Initialized in iron_runtime_init via IRON_ATOMIC_U64_INIT.
 * Definition lives in src/runtime/iron_heap_track.c. */
extern iron_atomic_u64 iron_alloc_id_counter;

/* ── Phase 20 PTR-10: per-thread stack-frame generation counter ───────────
 * Bumped on entry/exit of each function whose body takes the address of a
 * stack-local (Iron_FuncDecl.takes_local_addr=true; mark_takes_local_addr_pass
 * flag set in Plan 20-02a). Captured by-value into Iron_FatPtr.gen at every
 * &local site; checked at deref via iron_check_stack_pointer_gen.
 *
 * Initial value 1 (defined in src/runtime/iron_heap_track.c) — gen=0 stays
 * reserved as the freed-sentinel value per Phase 19 ABI lock; uint64_t TLS
 * default-init is 0, so we set 1 explicitly to keep the first &local's gen
 * out of the freed-sentinel range.
 *
 * OQ-B Option C lock (Plan 20-02b CONTEXT.md): a SEPARATE static-inline
 * (iron_check_stack_pointer_gen, below) compares fp.gen against this TLS
 * counter directly — no IronAllocHdr recovery (stack pointers have no
 * header). iron_check_pointer_gen and Phase 19's substrate stay UNTOUCHED.
 * Pitfall 7: both check helpers are static-inline and isomorphic so Phase 30
 * elision works on each path with the same template. */
extern _Thread_local uint64_t iron_stack_gen;

/* Phase 20 PTR-10: stack-pointer panic variant (NEW Plan 20-02b).
 * Same emission channels as iron_panic_stale_pointer (text + JSON);
 * different header text "dangling stack pointer to frame" and JSON
 * "panic":"stack_pointer". Forward-declared here so the static-inline
 * iron_check_stack_pointer_gen below can call it without pulling
 * diagnostics.h into every consumer; canonical re-declaration also lives in
 * src/diagnostics/diagnostics.h next to iron_panic_stale_pointer. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_stale_stack_pointer(const char *deref_file,
                                    int deref_line,
                                    uint64_t captured_frame_gen);

/* Phase 23 VEC-03: bounded vector out-of-bounds panic.
 * Forward-declared here so generated user binaries can call it inline at
 * push and index sites without depending on diagnostics.h.  Definition in
 * src/runtime/iron_panic.c.  Canonical declaration also in iron_panic.h. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_bvec_oob(const char *deref_file,
                         int deref_line,
                         int64_t index,
                         int64_t bound);

/* Phase 24 DROP-04/05 (Plan 24-03): partial-init cleanup + panic-trap TLS state.
 * Definitions live in iron_panic.c; canonical typedef + struct body + externs
 * also in iron_panic.h. Duplicated here (with the same layout) so generated user
 * binaries include all needed types via the single iron_runtime.h preamble
 * without depending on iron_panic.h directly.
 * Guard against double-definition when iron_panic.h is also included (e.g.,
 * in iron_panic.c which includes both). */
#ifndef IRON_INIT_CLEANUP_ENTRY_DEFINED
#define IRON_INIT_CLEANUP_ENTRY_DEFINED
typedef struct IronInitCleanupEntry {
    void (*drop_fn)(void *);
    void *field_ptr;
    struct IronInitCleanupEntry *prev;
} IronInitCleanupEntry;
#endif /* IRON_INIT_CLEANUP_ENTRY_DEFINED */
extern _Thread_local IronInitCleanupEntry *iron_init_cleanup_top;
extern _Thread_local bool iron_in_destructor;
extern _Thread_local const char *iron_current_dropping_type;
void iron_init_cleanup_register(IronInitCleanupEntry *entry,
                                 void (*drop_fn)(void *), void *field_ptr);
void iron_init_cleanup_run_and_clear(void);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_destructor_aborted(const char *type_name,
                                    const char *drop_site_file,
                                    int drop_site_line);

/* Phase 20 PTR-10: stack-pointer deref check (OQ-B Option C — separate
 * static-inline; preserves Phase 19 ABI lock). Iron's release codegen
 * inlines this trivially.
 *
 * Distinct from iron_check_pointer_gen because stack pointers carry the
 * TLS counter snapshot at &-site instead of an IronAllocHdr-sourced gen;
 * compares fp.gen directly against the current iron_stack_gen.
 *
 * Pitfall 7 isomorphism: matches iron_check_pointer_gen shape (load
 * generation source, compare, panic) so Phase 30's elision pass templates
 * over both. CONTEXT-locked: do not change to out-of-line without
 * coordinating with Phase 30. */
static inline void iron_check_stack_pointer_gen(Iron_FatPtr fp,
                                                const char *deref_file,
                                                int deref_line) {
    IRON_GENCHECK_COUNT_BUMP_STACK();  /* Phase 30 OPT-08: no-op unless IRON_GENCHECK_COUNT */
    if (!fp.addr) {
        iron_panic_stale_pointer(deref_file, deref_line, NULL);
    }
    if (fp.gen != iron_stack_gen) {
        iron_panic_stale_stack_pointer(deref_file, deref_line, fp.gen);
    }
}

/* ── Phase 28 GA1: arena-pointer deref check (3rd isomorphic sibling) ───────
 * Arena fat pointers carry a SNAPSHOT of the owning Arena's live generation
 * counter; their minimal prefix header (IronArenaAllocHdr — defined in
 * runtime/iron_arena_rt.h, 16B: arena_gen@0 + size@8) holds a back-reference
 * to that counter at offset 0. Deref recovers the header via
 * `((IronArenaAllocHdr*)addr)-1`, loads *hdr->arena_gen (the arena's CURRENT
 * generation), and panics on mismatch — i.e. when reset()/restore() bumped the
 * generation since the pointer was taken (O(1) mass-invalidation; GA1).
 *
 * Distinct from iron_check_pointer_gen (heap: gen lives INSIDE the per-alloc
 * header) and iron_check_stack_pointer_gen (stack: gen is a TLS counter): the
 * arena helper reads through a POINTER to the arena's shared counter. Per the
 * CONTEXT-locked note above (iron_runtime.h:285-290), Phase 19's substrate
 * stays UNTOUCHED — this is a separate isomorphic sibling so Phase 30's
 * elision pass templates over all three with the same shape.
 *
 * IronArenaAllocHdr and iron_panic_arena_stale are forward-declared here so
 * this static-inline can be defined WITHOUT iron_runtime.h pulling in
 * runtime/iron_arena_rt.h — mirroring how the heap/stack siblings forward-
 * declare their panic functions above. The header's arena_gen back-ref is the
 * first field (offset 0, ABI-locked in iron_arena_rt.h), so the recovered
 * pointer-to-counter is read directly without the full struct definition. The
 * full IronArenaAllocHdr definition + ABI _Static_asserts live in
 * runtime/iron_arena_rt.h, included by every consumer that allocates from an
 * arena. */
struct IronArenaAllocHdr;  /* full def + ABI lock in runtime/iron_arena_rt.h */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_arena_stale(const char *deref_file,
                            int deref_line,
                            const struct IronArenaAllocHdr *hdr);

static inline void iron_check_arena_pointer_gen(Iron_FatPtr fp,
                                                const char *deref_file,
                                                int deref_line) {
    IRON_GENCHECK_COUNT_BUMP_ARENA();  /* Phase 30 OPT-08: no-op unless IRON_GENCHECK_COUNT */
    if (!fp.addr) {
        iron_panic_stale_pointer(deref_file, deref_line, NULL);
    }
    /* Recover the header: it sits 16B (sizeof IronArenaAllocHdr) before the
     * payload. Its first field (offset 0) is the iron_atomic_u64* back-ref to
     * the arena's live generation counter. */
    iron_atomic_u64 *arena_gen = ((iron_atomic_u64 **)fp.addr)[-2];
    uint64_t cur = IRON_ATOMIC_U64_LOAD_ACQUIRE(*arena_gen);
    /* Arena allocations carry a MONOTONIC per-allocation snapshot (the live
     * counter's value at alloc time). The pointer is valid iff its snapshot is
     * still below the live counter: snapshot < cur. reset()/restore() lower the
     * live counter (to the floor 1, or to the save's gen_snapshot), so every
     * pointer whose snapshot is now >= cur was reclaimed and must panic. This
     * lets restore() selectively invalidate post-save allocations while pre-save
     * pointers (snapshot < gen_snapshot == cur) survive. */
    if (fp.gen >= cur) {
        iron_panic_arena_stale(deref_file, deref_line,
                               (const struct IronArenaAllocHdr *)
                               ((const char *)fp.addr - 16));
    }
}

/* ── Platform threading abstraction ──────────────────────────────────────── */
#ifdef _WIN32

  typedef HANDLE               iron_thread_t;
  typedef CRITICAL_SECTION     iron_mutex_t;
  typedef CONDITION_VARIABLE   iron_cond_t;

  /* Thread wrapper: Win32 thread proc signature differs from pthreads */
  typedef struct { void *(*fn)(void*); void *arg; } iron__win_trampoline_t;
  static DWORD WINAPI iron__win_thread_proc(void *p) {
      iron__win_trampoline_t *t = (iron__win_trampoline_t *)p;
      t->fn(t->arg);
      free(p);
      return 0;
  }
  static inline int iron__win_thread_create(iron_thread_t *t,
                                            void *(*fn)(void*), void *arg) {
      iron__win_trampoline_t *tramp = (iron__win_trampoline_t *)malloc(sizeof(*tramp));
      if (!tramp) return -1;
      tramp->fn = fn; tramp->arg = arg;
      *t = CreateThread(NULL, 0, iron__win_thread_proc, tramp, 0, NULL);
      return *t ? 0 : -1;
  }

  #define IRON_THREAD_CREATE(t,fn,arg)   iron__win_thread_create(&(t),(fn),(arg))
  #define IRON_THREAD_JOIN(t)            (WaitForSingleObject((t), INFINITE), CloseHandle((t)))
  #define IRON_MUTEX_INIT(m)             InitializeCriticalSection(&(m))
  #define IRON_MUTEX_LOCK(m)             EnterCriticalSection(&(m))
  #define IRON_MUTEX_UNLOCK(m)           LeaveCriticalSection(&(m))
  #define IRON_MUTEX_DESTROY(m)          DeleteCriticalSection(&(m))
  #define IRON_COND_INIT(c)              InitializeConditionVariable(&(c))
  #define IRON_COND_WAIT(c,m)            SleepConditionVariableCS(&(c), &(m), INFINITE)
  #define IRON_COND_SIGNAL(c)            WakeConditionVariable(&(c))
  #define IRON_COND_BROADCAST(c)         WakeAllConditionVariable(&(c))
  #define IRON_COND_DESTROY(c)           ((void)(c))  /* Win32 CV needs no destroy */
#else
  #include <pthread.h>
  typedef pthread_t          iron_thread_t;
  typedef pthread_mutex_t    iron_mutex_t;
  typedef pthread_cond_t     iron_cond_t;

  #define IRON_THREAD_CREATE(t,fn,arg)   pthread_create(&(t),NULL,(fn),(arg))
  #define IRON_THREAD_JOIN(t)            pthread_join((t), NULL)
  #define IRON_MUTEX_INIT(m)             pthread_mutex_init(&(m), NULL)
  #define IRON_MUTEX_LOCK(m)             pthread_mutex_lock(&(m))
  #define IRON_MUTEX_UNLOCK(m)           pthread_mutex_unlock(&(m))
  #define IRON_MUTEX_DESTROY(m)          pthread_mutex_destroy(&(m))
  #define IRON_COND_INIT(c)              pthread_cond_init(&(c), NULL)
  #define IRON_COND_WAIT(c,m)            pthread_cond_wait(&(c), &(m))
  #define IRON_COND_SIGNAL(c)            pthread_cond_signal(&(c))
  #define IRON_COND_BROADCAST(c)         pthread_cond_broadcast(&(c))
  #define IRON_COND_DESTROY(c)           pthread_cond_destroy(&(c))
#endif

/* ── Iron_String ────────────────────────────────────────────────────────────
 * 24-byte string type with Small String Optimisation (SSO).
 * Strings <= IRON_STRING_SSO_MAX bytes are stored inline without heap
 * allocation. Longer strings are heap-allocated. The intern table
 * deduplicates identical string content for literal strings.
 */
#define IRON_STRING_SSO_MAX 23

typedef struct {
    union {
        /* Heap variant (is_heap flag set) */
        struct {
            char    *data;
            uint32_t byte_length;
            uint32_t codepoint_count;
            uint8_t  _padding[7];
            uint8_t  flags; /* bit 0 = is_heap, bit 1 = is_interned */
        } heap;

        /* SSO variant (is_heap flag clear).
         * data[0..len-1] holds the string bytes; data[len] is always '\0'.
         * data has SSO_MAX+1 slots so a 23-byte string fits with terminator.
         * The union is padded to 25 bytes by the compiler to accommodate this.
         */
        struct {
            char    data[IRON_STRING_SSO_MAX + 1]; /* +1 for null terminator */
            uint8_t len; /* byte length (0..IRON_STRING_SSO_MAX) */
        } sso;
    };
} Iron_String;

/* Iron_String API */
Iron_String  iron_string_from_cstr(const char *cstr, size_t byte_len);
Iron_String  iron_string_from_literal(const char *lit, size_t byte_len);
const char  *iron_string_cstr(const Iron_String *s);
size_t       iron_string_byte_len(const Iron_String *s);
size_t       iron_string_codepoint_count(const Iron_String *s);
bool         iron_string_equals(const Iron_String *a, const Iron_String *b);
Iron_String  iron_string_concat(const Iron_String *a, const Iron_String *b);
Iron_String  iron_string_intern(Iron_String s);

/* ── Phase 26 POL-06: rc policy runtime API ──────────────────────────────────
 *
 * Replaces the pre-v4 control-block-plus-value shape entirely.
 * The legacy v1.x design (separate control block + value pointer) is gone
 * because it predated the Phase 19 atomic-ordering convention and had zero
 * codegen call sites (verified via `grep iron_rc_retain\|iron_rc_release
 * src/lir/ src/hir/ src/cli/` → zero hits).
 *
 * Allocation: iron_rc_alloc(size, drop_fn) returns the user pointer; the
 * Iron_RcHeader is the prefix of the block, recoverable in O(1) via
 * iron_rc_header_of(user_ptr).
 *
 * Atomic discipline (mirrors Phase 19 macros + new FETCH_SUB_RELEASE /
 * FENCE_ACQUIRE pair above): retain = relaxed-inc; release = release-dec;
 * on prev == 1, acquire-fence then drop_fn(user_ptr) then free(block).
 *
 * Underflow detection: assert prev > 0 in debug builds; silent wrap in
 * release. Overflow detection: saturate at UINT64_MAX-1 (iron_oom_abort
 * deterministically — UINT64_MAX retains is physically unreachable).
 *
 * Phase 27 will add weak rc + upgrade; Iron_RcHeader will grow a
 * weak_count field but the refcount@0 + drop_fn@8 layout is ABI-frozen. */
void          *iron_rc_alloc(size_t size, void (*drop_fn)(void *));
void           iron_rc_retain(void *user_ptr);
void           iron_rc_release(void *user_ptr);
Iron_RcHeader *iron_rc_header_of(void *user_ptr);

/* ── Phase 27 weak rc API ────────────────────────────────────────────────────
 *
 * `weak rc T` is a non-owning reference (POL-08). Block layout is unchanged
 * — weak handles point at the same user pointer the strong rc carried, just
 * tagged by the static type IRON_TYPE_WEAK_RC at the compiler level.
 *
 * Lifecycle (CONTEXT.md GA1):
 *   strong → 0   : drop_fn fires (Phase 26); block freed ONLY when
 *                  weak_count == 0 (acquire-load). Else block retained.
 *   weak   → 0   : block freed ONLY when refcount == 0 (acquire-load).
 *                  Else strong's eventual final-drop frees.
 *
 * Atomic discipline (CONTEXT.md GA1 + GA2):
 *   weak_count inc/dec : memory_order_relaxed (Iron does not surface
 *                        get_mut so Mara Bos's Acquire/Release is N/A;
 *                        see RC-LAYOUT.md §8 for the rationale).
 *   upgrade()          : Rust Arc canonical — acquire-load on refcount +
 *                        relaxed/relaxed CAS loop; returns NULL on
 *                        observed refcount==0 (covers mid-destructor race).
 *
 * See docs/dev/RC-LAYOUT.md §8 for the full state machine, upgrade race
 * diagram, and Mara-vs-Iron memory-ordering divergence rationale. */
void  iron_weak_rc_retain(void *user_ptr);
void  iron_weak_rc_release(void *user_ptr);
void *iron_rc_downgrade(void *strong_user_ptr);   /* rc T -> weak rc T */
void *iron_rc_upgrade(void *weak_user_ptr);       /* weak rc T -> T? (NULL on dead) */

/* ── Phase 29 OPT-08 input: opt-in rc-op counter ─────────────────────────────
 *
 * `iron_rc_retain` / `iron_rc_release` carry a pair of `_Atomic uint64_t`
 * counters guarded by the IRON_RC_COUNT compile-time macro. The macro is OFF
 * by default — normal builds add ZERO instructions to the hot path and the
 * deterministic phase-invariant test counts are unaffected.
 *
 * Defining IRON_RC_COUNT (e.g. an instrumented OPT-08 benchmark build) turns
 * the counters ON. They feed the deferred OPT-08 measurement that informs the
 * deferred OQ-07 `arc`-policy decision (see docs/dev/RC-ELISION.md).
 *
 * The accessor + reset are ALWAYS declared (stable symbol regardless of the
 * macro). When IRON_RC_COUNT is undefined they report zeros and reset is a
 * no-op, so callers compile and link identically in both configurations. */
void iron_rc_op_counts(uint64_t *retains, uint64_t *releases);
void iron_rc_op_counts_reset(void);

/* ── Iron_Error ──────────────────────────────────────────────────────────────
 * Lightweight error type (no heap allocation).
 */
typedef struct {
    int         code;
    const char *message;
} Iron_Error;

static inline Iron_Error iron_error_none(void)              { return (Iron_Error){0, NULL}; }
static inline Iron_Error iron_error_new(int c, const char *m) { return (Iron_Error){c, m}; }
static inline bool       iron_error_is_ok(Iron_Error e)     { return e.code == 0; }

/* ── Iron_Deadline — monotonic-clock budget helper (INFRA-09) ────────────────
 * Single-budget timeout accounting: a deadline is an absolute monotonic
 * timestamp (milliseconds). Operations subtract Iron_monotonic_now_ms() from
 * it to get the remaining budget. The sentinel value 0 means "already
 * expired / poll-once" so callers can short-circuit without a clock read.
 *
 * All accessors are static-inline so they inline into hot I/O paths with
 * zero function-call overhead.
 */
typedef struct {
    uint64_t deadline_mono_ms;  /* 0 sentinel => "poll-once / already expired" */
} Iron_Deadline;

uint64_t Iron_monotonic_now_ms(void);

static inline Iron_Deadline Iron_deadline_from_timeout_ms(int64_t timeout_ms) {
    if (timeout_ms <= 0) {
        Iron_Deadline d;
        d.deadline_mono_ms = 0;
        return d;
    }
    Iron_Deadline d;
    d.deadline_mono_ms = Iron_monotonic_now_ms() + (uint64_t)timeout_ms;
    return d;
}

static inline int Iron_deadline_remaining_ms(Iron_Deadline d) {
    if (d.deadline_mono_ms == 0) return 0;
    uint64_t now = Iron_monotonic_now_ms();
    if (now >= d.deadline_mono_ms) return 0;
    uint64_t rem = d.deadline_mono_ms - now;
    return (rem > (uint64_t)0x7FFFFFFF) ? 0x7FFFFFFF : (int)rem;
}

static inline bool Iron_deadline_expired(Iron_Deadline d) {
    if (d.deadline_mono_ms == 0) return true;
    return Iron_monotonic_now_ms() >= d.deadline_mono_ms;
}

/* ── Timed condvar wait ──────────────────────────────────────────────────────
 * Cross-platform bounded wait on an iron_cond_t. POSIX uses
 * pthread_cond_timedwait with CLOCK_REALTIME (documented caveat: may jump
 * during NTP slew — acceptable for Phase 59 foundation; TLS phase can
 * upgrade via pthread_condattr_setclock to CLOCK_MONOTONIC).
 * Windows uses SleepConditionVariableCS which is already monotonic.
 */
#define IRON_TIMEDWAIT_OK        0
#define IRON_TIMEDWAIT_EXPIRED   1
#define IRON_TIMEDWAIT_ERROR    -1
int iron_cond_timedwait_ms(iron_cond_t *cv, iron_mutex_t *lock, int timeout_ms);

/* ── Network init hooks (Phase 59 P01c) ──────────────────────────────────────
 * Iron_net_wsa_startup_once wraps WSAStartup(MAKEWORD(2,2)) with a refcounted
 * mutex so repeated iron_runtime_init calls (unit-test harness pattern) don't
 * re-enter WSAStartup. On POSIX these are no-ops and always return 0.
 *
 * iron_net_install_sigpipe_ignore installs SIG_IGN for SIGPIPE on POSIX so
 * writes to closed sockets/pipes return -1 / errno==EPIPE instead of killing
 * the process. Windows is a no-op (no SIGPIPE).
 *
 * Both hooks are called from iron_runtime_init; iron_runtime_shutdown calls
 * Iron_net_wsa_cleanup_once which decrements the refcount and only invokes
 * WSACleanup when the refcount hits zero.
 */
int  Iron_net_wsa_startup_once(void);
void Iron_net_wsa_cleanup_once(void);
void iron_net_install_sigpipe_ignore(void);

/* ── Built-in function declarations ──────────────────────────────────────────
 * These are called by code generated by the Iron compiler.
 */
void    Iron_print(Iron_String s);
void    Iron_println(Iron_String s);
int64_t Iron_len(Iron_String s);
int64_t Iron_min(int64_t a, int64_t b);
int64_t Iron_max(int64_t a, int64_t b);
int64_t Iron_clamp(int64_t val, int64_t lo, int64_t hi);
int64_t Iron_abs(int64_t val);
void    Iron_assert(bool cond, Iron_String msg);

/* ── Phase 78 FMT — Int/Int32/Float → String conversion ─────────────────
 * Defined in src/runtime/iron_fmt.c. Consumed by the Iron-side stubs in
 * src/stdlib/int.iron and src/stdlib/float.iron (landed in Plan 78-02).
 *
 * Iron_int_to_string   — signed 64-bit decimal (INT64_MIN safe).
 * Iron_int32_to_string — signed 32-bit decimal (INT32_MIN safe).
 * Iron_float_to_string — libc %.6g (6 sig digits, trailing zeros trimmed);
 *                        NaN/±Inf/-0.0 normalize to "NaN"/"inf"/"-inf"/"0".
 */
Iron_String Iron_int_to_string(int64_t n);
Iron_String Iron_int32_to_string(int32_t n);
Iron_String Iron_float_to_string(double f);

static inline int64_t Iron_range(int64_t n) { return n; }

/* ── Iron_Pool (fixed-size thread pool) ──────────────────────────────────────
 * Iron_Pool manages a set of worker threads and a FIFO work queue.
 * Iron_pool_barrier() blocks until all submitted work completes.
 * The global pool is initialized in iron_runtime_init().
 */
typedef struct Iron_Pool Iron_Pool;

/* Global pool — initialized in iron_runtime_init() */
extern Iron_Pool *Iron_global_pool;

/* Pool API */
Iron_Pool *Iron_pool_create(const char *name, int thread_count);
void       Iron_pool_destroy(Iron_Pool *pool);
void       Iron_pool_submit(Iron_Pool *pool, void (*fn)(void *), void *arg);
void       Iron_pool_barrier(Iron_Pool *pool);
int        Iron_pool_thread_count(const Iron_Pool *pool);

/* ── Elastic pool (Phase 59 P01b) ─────────────────────────────────────────────
 * Iron_elastic_pool_create returns an Iron_Pool that grows workers on demand
 * (0..max_threads) and retires idle workers after idle_timeout_ms of no work.
 * Elastic pools support the same submit/barrier API as fixed-size pools plus
 * Iron_pool_submit_wait for signalling completion through an Iron_PoolWait.
 */
Iron_Pool *Iron_elastic_pool_create(const char *name,
                                    int max_threads,
                                    int idle_timeout_ms);

/* Read accessors — expose elastic-mode state without leaking struct layout. */
bool Iron_pool_is_elastic(const Iron_Pool *p);
int  Iron_pool_max_threads(const Iron_Pool *p);
int  Iron_pool_live_thread_count(const Iron_Pool *p);
int  Iron_pool_leaked_count(const Iron_Pool *p);

/* Bump leaked-worker bookkeeping and logically free the slot so elastic math
 * allows spawning a replacement on the next submit. See RESEARCH.md Pitfall 13
 * for the pending-decrement invariant. */
void Iron_pool_mark_one_leaked(Iron_Pool *pool);

/* Global elastic I/O pool — initialized in iron_threads_init(). Used by
 * blocking-DNS, blocking-syscall, and other I/O-bound work paths that would
 * otherwise starve Iron_global_pool's CPU workers. */
extern Iron_Pool *Iron_io_pool;

/* ── Iron_PoolWait — abandoned-flag completion primitive (Phase 59 P01b) ────
 * Coordination between a caller that submits I/O work with a deadline and a
 * worker that may outlive the caller's patience. The caller calls wait_ms;
 * on timeout it calls set_abandoned and returns an error. The worker calls
 * worker_finish when its syscall returns — if abandoned, the worker owns the
 * result and must destroy it; otherwise the result is stored on the wait
 * struct and the caller is signalled.
 *
 * The wait struct is allocated by the caller via Iron_poolwait_create and
 * always freed by the caller via Iron_poolwait_destroy. The abandoned-flag
 * variant avoids a refcount at the cost of the caller owning the struct's
 * lifetime even in the leaked-worker case.
 */
typedef struct Iron_PoolWait Iron_PoolWait;

Iron_PoolWait *Iron_poolwait_create(void);
void           Iron_poolwait_destroy(Iron_PoolWait *w);
bool           Iron_poolwait_completed(Iron_PoolWait *w);
/* Block until the worker signals completion, the timeout expires, or an
 * error occurs. Returns 1 on completed, 0 on timeout, -1 on error. */
int            Iron_poolwait_wait_ms(Iron_PoolWait *w, int timeout_ms);
void           Iron_poolwait_set_abandoned(Iron_PoolWait *w);
void           Iron_poolwait_worker_finish(Iron_PoolWait *w,
                                           void *result,
                                           void (*result_destructor)(void*));

/* Submit work to an elastic pool AND bind an Iron_PoolWait the worker will
 * signal on completion. Intended for elastic pools (I/O paths); calling this
 * on a fixed-size pool is undefined. */
void Iron_pool_submit_wait(Iron_Pool *pool,
                           void (*fn)(void *),
                           void *arg,
                           Iron_PoolWait *wait);

/* ── Iron_Handle (future for spawn/await) ────────────────────────────────────
 * Created by spawn; awaited with Iron_handle_wait().
 * Panic in the spawned task is stored and re-raised on wait.
 */
typedef struct {
    iron_thread_t   thread;
    bool            done;
    void           *result;
    iron_mutex_t    lock;
    iron_cond_t     cond;
    char           *panic_msg;
} Iron_Handle;

/* Handle API */
Iron_Handle *Iron_handle_create(void (*fn)(void *), void *arg);
void         Iron_handle_wait(Iron_Handle *handle);
void         Iron_handle_destroy(Iron_Handle *handle);
void        *iron_future_await(Iron_Handle *handle);
Iron_Handle *iron_handle_create_self_ref(void (*fn)(void *));

/* ── Iron_Channel (bounded ring buffer) ──────────────────────────────────────
 * send blocks when the buffer is full; recv blocks when it is empty.
 * try_recv returns immediately with true/false.
 * capacity 0 or 1 is treated as unbuffered (capacity = 1).
 */
typedef struct Iron_Channel Iron_Channel;

/* Channel API */
Iron_Channel *Iron_channel_create(int capacity);
void          Iron_channel_send(Iron_Channel *ch, void *item);
void         *Iron_channel_recv(Iron_Channel *ch);
bool          Iron_channel_try_recv(Iron_Channel *ch, void **out);
void          Iron_channel_close(Iron_Channel *ch);
void          Iron_channel_destroy(Iron_Channel *ch);

/* ── Iron_Mutex (value-wrapping mutex) ───────────────────────────────────────
 * Wraps a value so that all access must go through lock/unlock.
 * Iron_mutex_lock() returns a pointer to the wrapped value.
 */
typedef struct {
    iron_mutex_t    lock;
    void           *value;
    size_t          value_size;
} Iron_Mutex;

/* Mutex API */
Iron_Mutex *Iron_mutex_create(void *initial_value, size_t size);
void       *Iron_mutex_lock(Iron_Mutex *m);   /* returns pointer to value */
void        Iron_mutex_unlock(Iron_Mutex *m);
void        Iron_mutex_destroy(Iron_Mutex *m);

/* ── Lock / CondVar raw primitives ───────────────────────────────────────────
 * Thin wrappers around pthread_mutex_t and pthread_cond_t for use in
 * Iron programs that need lower-level synchronisation.
 */
typedef iron_mutex_t    Iron_Lock;
typedef iron_cond_t     Iron_CondVar;

void Iron_lock_init(Iron_Lock *l);
void Iron_lock_acquire(Iron_Lock *l);
void Iron_lock_release(Iron_Lock *l);
void Iron_condvar_init(Iron_CondVar *cv);
void Iron_condvar_wait(Iron_CondVar *cv, Iron_Lock *l);
void Iron_condvar_signal(Iron_CondVar *cv);
void Iron_condvar_broadcast(Iron_CondVar *cv);

/* ── Runtime lifecycle ───────────────────────────────────────────────────────
 * iron_runtime_init(argc, argv) must be called before any Iron_String or Iron_Rc use.
 * It stores argc/argv in file-scope globals for os.args() access and creates
 * Iron_global_pool with (cpu_count - 1) worker threads.
 * Pass (0, NULL) when no args are needed (e.g. in unit tests).
 * iron_runtime_shutdown() releases all runtime resources.
 */
void iron_runtime_init(int argc, char **argv);
void iron_runtime_shutdown(void);

/* ── Closure fat pointer ─────────────────────────────────────────────────── */
/* All closure values — capturing and non-capturing — use this struct.
 * Non-capturing closures have env = NULL. */
typedef struct {
    void *env;
    void (*fn)(void *);
} Iron_Closure;

/* Call a closure. Casts fn to the actual signature and passes env as first arg.
 * For void closures with no extra args: IRON_CALL_CLOSURE(c)
 * For closures with args, the emitter writes explicit casts instead. */
#define IRON_CALL_CLOSURE(c) ((c).fn((c).env))

/* ── Collection macros ───────────────────────────────────────────────────────
 * Macro-generated List[T], Map[K,V], and Set[T] collection types.
 *
 * The Iron compiler's monomorphization pass (gen_types.c ensure_monomorphized_type)
 * emits stub struct typedefs with the naming convention Iron_<base>_<csuffix>.
 * These macros generate the matching function implementations.
 *
 * Naming example (from gen_types.c mangle_generic):
 *   List[Int]        -> Iron_List_int64_t   (struct + functions)
 *   Map[String, Int] -> Iron_Map_Iron_String_int64_t
 *   Set[Int]         -> Iron_Set_int64_t
 *
 * Usage:
 *   IRON_LIST_DECL(int64_t, int64_t)   -- declares function prototypes
 *   IRON_LIST_IMPL(int64_t, int64_t)   -- defines function bodies (in .c file)
 *
 * The codegen-emitted struct typedef must already be visible (the codegen
 * output includes it before calling any runtime function).  For the runtime's
 * own test/collection file, use the pre-instantiated common types defined
 * with IRON_CODEGEN_PROVIDES_STRUCTS unset (see iron_collections.c).
 */

/* ── List[T] macros ──────────────────────────────────────────────────────────
 * Expected struct layout (emitted by ensure_monomorphized_type):
 *   typedef struct Iron_List_##suffix {
 *       T       *items;
 *       int64_t  count;
 *       int64_t  capacity;
 *   } Iron_List_##suffix;
 */
#define IRON_LIST_DECL(T, suffix) \
    Iron_List_##suffix Iron_List_##suffix##_create(void); \
    Iron_List_##suffix Iron_List_##suffix##_create_with_capacity(int64_t cap); \
    Iron_List_##suffix Iron_List_##suffix##_clone(const Iron_List_##suffix *src); \
    void               Iron_List_##suffix##_push(Iron_List_##suffix *self, T item); \
    T                  Iron_List_##suffix##_get(const Iron_List_##suffix *self, int64_t index); \
    void               Iron_List_##suffix##_set(Iron_List_##suffix *self, int64_t index, T item); \
    T                  Iron_List_##suffix##_pop(Iron_List_##suffix *self); \
    int64_t            Iron_List_##suffix##_len(const Iron_List_##suffix *self); \
    void               Iron_List_##suffix##_free(Iron_List_##suffix *self);

#define IRON_LIST_IMPL(T, suffix) \
    Iron_List_##suffix Iron_List_##suffix##_create(void) { \
        Iron_List_##suffix l; \
        l.items = NULL; l.count = 0; l.capacity = 0; \
        return l; \
    } \
    Iron_List_##suffix Iron_List_##suffix##_create_with_capacity(int64_t cap) { \
        Iron_List_##suffix l; \
        l.count = 0; \
        l.capacity = cap; \
        l.items = NULL; \
        if (cap > 0) { \
            l.items = (T *)malloc((size_t)cap * sizeof(T)); \
            if (!l.items) iron_oom_abort("Iron_List_" #suffix "_create_with_capacity"); \
        } \
        return l; \
    } \
    Iron_List_##suffix Iron_List_##suffix##_clone(const Iron_List_##suffix *src) { \
        Iron_List_##suffix dst; \
        dst.count = src->count; \
        dst.capacity = src->count; \
        if (src->count > 0) { \
            dst.items = (T *)malloc((size_t)src->count * sizeof(T)); \
            if (!dst.items) iron_oom_abort("Iron_List_" #suffix "_clone"); \
            memcpy(dst.items, src->items, (size_t)src->count * sizeof(T)); \
        } else { \
            dst.items = NULL; \
        } \
        return dst; \
    } \
    void Iron_List_##suffix##_push(Iron_List_##suffix *self, T item) { \
        if (self->count >= self->capacity) { \
            int64_t new_cap = self->capacity ? self->capacity * 2 : 8; \
            /* FIX-01/FIX-02: capacity doubling must not wrap int64_t (audit row 18) */ \
            if (new_cap < self->capacity) { \
                iron_oom_abort("Iron_List_" #suffix "_push: capacity overflow"); \
            } \
            T *new_items = (T *)realloc(self->items, (size_t)new_cap * sizeof(T)); \
            if (!new_items) iron_oom_abort("Iron_List_" #suffix "_push"); \
            self->items = new_items; \
            self->capacity = new_cap; \
        } \
        self->items[self->count++] = item; \
    } \
    T Iron_List_##suffix##_get(const Iron_List_##suffix *self, int64_t index) { \
        return self->items[index]; \
    } \
    void Iron_List_##suffix##_set(Iron_List_##suffix *self, int64_t index, T item) { \
        self->items[index] = item; \
    } \
    T Iron_List_##suffix##_pop(Iron_List_##suffix *self) { \
        return self->items[--self->count]; \
    } \
    int64_t Iron_List_##suffix##_len(const Iron_List_##suffix *self) { \
        return self->count; \
    } \
    void Iron_List_##suffix##_free(Iron_List_##suffix *self) { \
        free(self->items); \
        self->items = NULL; self->count = 0; self->capacity = 0; \
    }

/* ── Collection method macros (map, filter, reduce, forEach, sum) ────────────
 * These macros generate the higher-order collection operations for List[T].
 * The closures follow Iron's uniform calling convention:
 *   fn(void *env, arg0, arg1, ...) — env is always the first parameter.
 */
#define IRON_LIST_COLL_DECL(T, suffix) \
    Iron_List_##suffix Iron_List_##suffix##_map(const Iron_List_##suffix *self, Iron_Closure f); \
    Iron_List_##suffix Iron_List_##suffix##_filter(const Iron_List_##suffix *self, Iron_Closure f); \
    T                  Iron_List_##suffix##_reduce(const Iron_List_##suffix *self, T init, Iron_Closure f); \
    void               Iron_List_##suffix##_forEach(const Iron_List_##suffix *self, Iron_Closure f); \
    T                  Iron_List_##suffix##_sum(const Iron_List_##suffix *self);

/* Implementation uses memcpy for closure fn casts to avoid
 * -Wcast-function-type-mismatch.  Callers must include <string.h>. */
#define IRON_LIST_COLL_IMPL(T, suffix, zero_val) \
    Iron_List_##suffix Iron_List_##suffix##_map(const Iron_List_##suffix *self, Iron_Closure f) { \
        typedef T (*MapFn)(void *, T); \
        MapFn map_fn; \
        memcpy(&map_fn, &f.fn, sizeof(map_fn)); \
        Iron_List_##suffix result = Iron_List_##suffix##_create(); \
        for (int64_t i = 0; i < self->count; i++) { \
            T val = map_fn(f.env, self->items[i]); \
            Iron_List_##suffix##_push(&result, val); \
        } \
        return result; \
    } \
    Iron_List_##suffix Iron_List_##suffix##_filter(const Iron_List_##suffix *self, Iron_Closure f) { \
        typedef bool (*FilterFn)(void *, T); \
        FilterFn filter_fn; \
        memcpy(&filter_fn, &f.fn, sizeof(filter_fn)); \
        Iron_List_##suffix result = Iron_List_##suffix##_create(); \
        for (int64_t i = 0; i < self->count; i++) { \
            if (filter_fn(f.env, self->items[i])) { \
                Iron_List_##suffix##_push(&result, self->items[i]); \
            } \
        } \
        return result; \
    } \
    T Iron_List_##suffix##_reduce(const Iron_List_##suffix *self, T init, Iron_Closure f) { \
        typedef T (*ReduceFn)(void *, T, T); \
        ReduceFn reduce_fn; \
        memcpy(&reduce_fn, &f.fn, sizeof(reduce_fn)); \
        T acc = init; \
        for (int64_t i = 0; i < self->count; i++) { \
            acc = reduce_fn(f.env, acc, self->items[i]); \
        } \
        return acc; \
    } \
    void Iron_List_##suffix##_forEach(const Iron_List_##suffix *self, Iron_Closure f) { \
        typedef void (*ForEachFn)(void *, T); \
        ForEachFn each_fn; \
        memcpy(&each_fn, &f.fn, sizeof(each_fn)); \
        for (int64_t i = 0; i < self->count; i++) { \
            each_fn(f.env, self->items[i]); \
        } \
    } \
    T Iron_List_##suffix##_sum(const Iron_List_##suffix *self) { \
        T total = zero_val; \
        for (int64_t i = 0; i < self->count; i++) { \
            total = total + self->items[i]; \
        } \
        return total; \
    }

/* ── Map[K,V] macros ─────────────────────────────────────────────────────────
 * Simple array-based map with linear-scan lookup (O(n), sufficient for v1).
 * eq_fn has signature: bool (*)(const K *a, const K *b)
 *
 * Expected struct layout:
 *   typedef struct Iron_Map_##ksuffix##_##vsuffix {
 *       K       *keys;
 *       V       *values;
 *       int64_t  count;
 *       int64_t  capacity;
 *   } Iron_Map_##ksuffix##_##vsuffix;
 */
#define IRON_MAP_DECL(K, V, ksuffix, vsuffix) \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_create(void); \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_create_with_capacity(int64_t cap); \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_clone(const Iron_Map_##ksuffix##_##vsuffix *src); \
    void  Iron_Map_##ksuffix##_##vsuffix##_put(Iron_Map_##ksuffix##_##vsuffix *self, K key, V value); \
    V     Iron_Map_##ksuffix##_##vsuffix##_get(const Iron_Map_##ksuffix##_##vsuffix *self, K key); \
    bool  Iron_Map_##ksuffix##_##vsuffix##_has(const Iron_Map_##ksuffix##_##vsuffix *self, K key); \
    void  Iron_Map_##ksuffix##_##vsuffix##_remove(Iron_Map_##ksuffix##_##vsuffix *self, K key); \
    int64_t Iron_Map_##ksuffix##_##vsuffix##_len(const Iron_Map_##ksuffix##_##vsuffix *self); \
    void  Iron_Map_##ksuffix##_##vsuffix##_free(Iron_Map_##ksuffix##_##vsuffix *self);

#define IRON_MAP_IMPL(K, V, ksuffix, vsuffix, eq_fn) \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_create(void) { \
        Iron_Map_##ksuffix##_##vsuffix m; \
        m.keys = NULL; m.values = NULL; m.count = 0; m.capacity = 0; \
        return m; \
    } \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_create_with_capacity(int64_t cap) { \
        Iron_Map_##ksuffix##_##vsuffix m; \
        m.count = 0; \
        m.capacity = cap; \
        m.keys = NULL; m.values = NULL; \
        if (cap > 0) { \
            m.keys = (K *)malloc((size_t)cap * sizeof(K)); \
            if (!m.keys) iron_oom_abort("Iron_Map_" #ksuffix "_" #vsuffix "_create_with_capacity: keys"); \
            m.values = (V *)malloc((size_t)cap * sizeof(V)); \
            if (!m.values) iron_oom_abort("Iron_Map_" #ksuffix "_" #vsuffix "_create_with_capacity: values"); \
        } \
        return m; \
    } \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_clone(const Iron_Map_##ksuffix##_##vsuffix *src) { \
        Iron_Map_##ksuffix##_##vsuffix dst; \
        dst.count = src->count; \
        dst.capacity = src->count; \
        if (src->count > 0) { \
            dst.keys   = (K *)malloc((size_t)src->count * sizeof(K)); \
            if (!dst.keys) iron_oom_abort("Iron_Map_" #ksuffix "_" #vsuffix "_clone: keys"); \
            dst.values = (V *)malloc((size_t)src->count * sizeof(V)); \
            if (!dst.values) iron_oom_abort("Iron_Map_" #ksuffix "_" #vsuffix "_clone: values"); \
            memcpy(dst.keys,   src->keys,   (size_t)src->count * sizeof(K)); \
            memcpy(dst.values, src->values, (size_t)src->count * sizeof(V)); \
        } else { \
            dst.keys = NULL; \
            dst.values = NULL; \
        } \
        return dst; \
    } \
    void Iron_Map_##ksuffix##_##vsuffix##_put(Iron_Map_##ksuffix##_##vsuffix *self, K key, V value) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->keys[i], &key)) { self->values[i] = value; return; } \
        } \
        if (self->count >= self->capacity) { \
            int64_t new_cap = self->capacity ? self->capacity * 2 : 8; \
            /* FIX-01/FIX-02: capacity doubling must not wrap int64_t (audit row 18) */ \
            if (new_cap < self->capacity) { \
                iron_oom_abort("Iron_Map_" #ksuffix "_" #vsuffix "_put: capacity overflow"); \
            } \
            K *new_keys = (K *)realloc(self->keys, (size_t)new_cap * sizeof(K)); \
            if (!new_keys) iron_oom_abort("Iron_Map_" #ksuffix "_" #vsuffix "_put: keys"); \
            self->keys = new_keys; \
            V *new_values = (V *)realloc(self->values, (size_t)new_cap * sizeof(V)); \
            if (!new_values) iron_oom_abort("Iron_Map_" #ksuffix "_" #vsuffix "_put: values"); \
            self->values = new_values; \
            self->capacity = new_cap; \
        } \
        self->keys[self->count]   = key; \
        self->values[self->count] = value; \
        self->count++; \
    } \
    V Iron_Map_##ksuffix##_##vsuffix##_get(const Iron_Map_##ksuffix##_##vsuffix *self, K key) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->keys[i], &key)) { return self->values[i]; } \
        } \
        /* Caller must use has() first — undefined if key absent */ \
        V _zero; \
        memset(&_zero, 0, sizeof(V)); \
        return _zero; \
    } \
    bool Iron_Map_##ksuffix##_##vsuffix##_has(const Iron_Map_##ksuffix##_##vsuffix *self, K key) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->keys[i], &key)) { return true; } \
        } \
        return false; \
    } \
    void Iron_Map_##ksuffix##_##vsuffix##_remove(Iron_Map_##ksuffix##_##vsuffix *self, K key) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->keys[i], &key)) { \
                self->keys[i]   = self->keys[self->count - 1]; \
                self->values[i] = self->values[self->count - 1]; \
                self->count--; \
                return; \
            } \
        } \
    } \
    int64_t Iron_Map_##ksuffix##_##vsuffix##_len(const Iron_Map_##ksuffix##_##vsuffix *self) { \
        return self->count; \
    } \
    void Iron_Map_##ksuffix##_##vsuffix##_free(Iron_Map_##ksuffix##_##vsuffix *self) { \
        free(self->keys);   self->keys   = NULL; \
        free(self->values); self->values = NULL; \
        self->count = 0; self->capacity = 0; \
    }

/* ── Set[T] macros ───────────────────────────────────────────────────────────
 * Simple array-based set with linear-scan deduplication (O(n), sufficient v1).
 * eq_fn has signature: bool (*)(const T *a, const T *b)
 *
 * Expected struct layout:
 *   typedef struct Iron_Set_##suffix {
 *       T       *items;
 *       int64_t  count;
 *       int64_t  capacity;
 *   } Iron_Set_##suffix;
 */
#define IRON_SET_DECL(T, suffix) \
    Iron_Set_##suffix Iron_Set_##suffix##_create(void); \
    Iron_Set_##suffix Iron_Set_##suffix##_create_with_capacity(int64_t cap); \
    Iron_Set_##suffix Iron_Set_##suffix##_clone(const Iron_Set_##suffix *src); \
    void    Iron_Set_##suffix##_add(Iron_Set_##suffix *self, T item); \
    bool    Iron_Set_##suffix##_contains(const Iron_Set_##suffix *self, T item); \
    void    Iron_Set_##suffix##_remove(Iron_Set_##suffix *self, T item); \
    int64_t Iron_Set_##suffix##_len(const Iron_Set_##suffix *self); \
    void    Iron_Set_##suffix##_free(Iron_Set_##suffix *self);

#define IRON_SET_IMPL(T, suffix, eq_fn) \
    Iron_Set_##suffix Iron_Set_##suffix##_create(void) { \
        Iron_Set_##suffix s; \
        s.items = NULL; s.count = 0; s.capacity = 0; \
        return s; \
    } \
    Iron_Set_##suffix Iron_Set_##suffix##_create_with_capacity(int64_t cap) { \
        Iron_Set_##suffix s; \
        s.count = 0; \
        s.capacity = cap; \
        s.items = NULL; \
        if (cap > 0) { \
            s.items = (T *)malloc((size_t)cap * sizeof(T)); \
            if (!s.items) iron_oom_abort("Iron_Set_" #suffix "_create_with_capacity"); \
        } \
        return s; \
    } \
    Iron_Set_##suffix Iron_Set_##suffix##_clone(const Iron_Set_##suffix *src) { \
        Iron_Set_##suffix dst; \
        dst.count = src->count; \
        dst.capacity = src->count; \
        if (src->count > 0) { \
            dst.items = (T *)malloc((size_t)src->count * sizeof(T)); \
            if (!dst.items) iron_oom_abort("Iron_Set_" #suffix "_clone"); \
            memcpy(dst.items, src->items, (size_t)src->count * sizeof(T)); \
        } else { \
            dst.items = NULL; \
        } \
        return dst; \
    } \
    void Iron_Set_##suffix##_add(Iron_Set_##suffix *self, T item) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->items[i], &item)) { return; } \
        } \
        if (self->count >= self->capacity) { \
            int64_t new_cap = self->capacity ? self->capacity * 2 : 8; \
            /* FIX-01/FIX-02: capacity doubling must not wrap int64_t (audit row 18) */ \
            if (new_cap < self->capacity) { \
                iron_oom_abort("Iron_Set_" #suffix "_add: capacity overflow"); \
            } \
            T *new_items = (T *)realloc(self->items, (size_t)new_cap * sizeof(T)); \
            if (!new_items) iron_oom_abort("Iron_Set_" #suffix "_add"); \
            self->items = new_items; \
            self->capacity = new_cap; \
        } \
        self->items[self->count++] = item; \
    } \
    bool Iron_Set_##suffix##_contains(const Iron_Set_##suffix *self, T item) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->items[i], &item)) { return true; } \
        } \
        return false; \
    } \
    void Iron_Set_##suffix##_remove(Iron_Set_##suffix *self, T item) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->items[i], &item)) { \
                self->items[i] = self->items[self->count - 1]; \
                self->count--; \
                return; \
            } \
        } \
    } \
    int64_t Iron_Set_##suffix##_len(const Iron_Set_##suffix *self) { \
        return self->count; \
    } \
    void Iron_Set_##suffix##_free(Iron_Set_##suffix *self) { \
        free(self->items); \
        self->items = NULL; self->count = 0; self->capacity = 0; \
    }

/* ── Pre-instantiated common collection struct typedefs ──────────────────────
 * The codegen emits its own struct typedefs in the generated C output.
 * Here we define the most common types so that iron_collections.c and
 * test files compile without needing a full codegen pass.
 * C11 permits duplicate compatible typedefs, so codegen output can repeat them.
 */
#ifndef IRON_CODEGEN_PROVIDES_STRUCTS

typedef struct Iron_List_int64_t    { int64_t     *items; int64_t count; int64_t capacity; } Iron_List_int64_t;
typedef struct Iron_List_int32_t    { int32_t     *items; int64_t count; int64_t capacity; } Iron_List_int32_t;
typedef struct Iron_List_double     { double      *items; int64_t count; int64_t capacity; } Iron_List_double;
typedef struct Iron_List_bool       { bool        *items; int64_t count; int64_t capacity; } Iron_List_bool;
typedef struct Iron_List_Iron_String { Iron_String *items; int64_t count; int64_t capacity; } Iron_List_Iron_String;
typedef struct Iron_List_Iron_Closure { Iron_Closure *items; int64_t count; int64_t capacity; } Iron_List_Iron_Closure;
/* Phase 68 (Plan 68-01): ABI-FLOAT32 + ABI-UINT8 pre-instantiated struct
 * typedefs.
 *
 * Suffix convention matches ironc's emit_type_to_c (src/lir/emit_helpers.c:151):
 *   Float32 → "float"   →  Iron_List_float
 *   UInt8   → "uint8_t" →  Iron_List_uint8_t
 *
 * RESEARCH.md Pitfall #1 flagged the risk that mangle_generic might emit a
 * different suffix for Float32; the probe tests/manual/abi_float32_probe.iron
 * confirmed the suffix is plain `float`.  These typedefs are visible to
 * iron_collections.c so that IRON_LIST_IMPL(float, float) and
 * IRON_LIST_IMPL(uint8_t, uint8_t) expand into well-formed bodies. */
typedef struct Iron_List_float   { float   *items; int64_t count; int64_t capacity; } Iron_List_float;
typedef struct Iron_List_uint8_t { uint8_t *items; int64_t count; int64_t capacity; } Iron_List_uint8_t;

typedef struct Iron_Map_Iron_String_int64_t    { Iron_String *keys; int64_t     *values; int64_t count; int64_t capacity; } Iron_Map_Iron_String_int64_t;
typedef struct Iron_Map_Iron_String_Iron_String { Iron_String *keys; Iron_String *values; int64_t count; int64_t capacity; } Iron_Map_Iron_String_Iron_String;

typedef struct Iron_Set_int64_t    { int64_t     *items; int64_t count; int64_t capacity; } Iron_Set_int64_t;
typedef struct Iron_Set_Iron_String { Iron_String *items; int64_t count; int64_t capacity; } Iron_Set_Iron_String;

#endif /* IRON_CODEGEN_PROVIDES_STRUCTS */

/* Declarations for the pre-instantiated types in iron_collections.c */
IRON_LIST_DECL(int64_t,     int64_t)
IRON_LIST_DECL(int32_t,     int32_t)
IRON_LIST_DECL(double,      double)
IRON_LIST_DECL(bool,        bool)
IRON_LIST_DECL(Iron_String, Iron_String)
IRON_LIST_DECL(Iron_Closure, Iron_Closure)
/* Phase 68 (Plan 68-01): ABI-FLOAT32 + ABI-UINT8 primitive list types.
 * Suffix matches ironc's emit_type_to_c output (emit_helpers.c:151):
 * Float32 → "float", UInt8 → "uint8_t".  See probe
 * tests/manual/abi_float32_probe.iron for end-to-end validation. */
IRON_LIST_DECL(float,       float)
IRON_LIST_DECL(uint8_t,     uint8_t)

/* Collection method declarations for common numeric types */
IRON_LIST_COLL_DECL(int64_t, int64_t)
IRON_LIST_COLL_DECL(int32_t, int32_t)
IRON_LIST_COLL_DECL(double,  double)
/* Phase 68 (Plan 68-01): map/filter/reduce/forEach/sum for float +
 * uint8_t — keeps parity with int32_t/int64_t/double. */
IRON_LIST_COLL_DECL(float,   float)
IRON_LIST_COLL_DECL(uint8_t, uint8_t)

IRON_MAP_DECL(Iron_String, int64_t,     Iron_String, int64_t)
IRON_MAP_DECL(Iron_String, Iron_String, Iron_String, Iron_String)

IRON_SET_DECL(int64_t,     int64_t)
IRON_SET_DECL(Iron_String, Iron_String)

/* ── String built-in method declarations (Phase 38) ─────────────────────────
 * Called by code generated by the Iron compiler for s.method() syntax.
 */
Iron_String           Iron_string_upper(Iron_String self);
Iron_String           Iron_string_lower(Iron_String self);
Iron_String           Iron_string_trim(Iron_String self);
bool                  Iron_string_contains(Iron_String self, Iron_String sub);
bool                  Iron_string_starts_with(Iron_String self, Iron_String prefix);
bool                  Iron_string_ends_with(Iron_String self, Iron_String suffix);
Iron_List_Iron_String Iron_string_split(Iron_String self, Iron_String sep);
Iron_String           Iron_string_replace(Iron_String self, Iron_String old_s, Iron_String new_s);
Iron_String           Iron_string_substring(Iron_String self, int64_t start, int64_t end_idx);
int64_t               Iron_string_index_of(Iron_String self, Iron_String sub);
Iron_String           Iron_string_char_at(Iron_String self, int64_t i);
int64_t               Iron_string_to_int(Iron_String self);
double                Iron_string_to_float(Iron_String self);
Iron_String           Iron_string_join(Iron_String self, Iron_List_Iron_String parts);
int64_t               Iron_string_len(Iron_String self);
Iron_String           Iron_string_repeat(Iron_String self, int64_t n);
Iron_String           Iron_string_pad_left(Iron_String self, int64_t width, Iron_String ch);
Iron_String           Iron_string_pad_right(Iron_String self, int64_t width, Iron_String ch);
int64_t               Iron_string_count(Iron_String self, Iron_String sub);

/* ── String primitives added in Phase 59 P01c ────────────────────────────────
 * rindex_of / byte_at / from_byte — the trio Phase 59 needs for URL parsing
 * and generic byte-level access. from_byte is intentionally batched here
 * (instead of landing in P05) so all three primitives arrive in one coherent
 * edit to string.iron.
 */
int64_t               Iron_string_rindex_of(Iron_String self, Iron_String sub);
int64_t               Iron_string_byte_at(Iron_String self, int64_t i);
Iron_String           Iron_string_from_byte(int64_t b);

#endif /* IRON_RUNTIME_H */
