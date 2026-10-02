#ifndef IRON_RUNTIME_H
#define IRON_RUNTIME_H

/* The runtime header is freestanding (#235): it includes only the headers
 * clang ships with the compiler and declares everything else itself, so
 * generated C compiles without any platform SDK. Memory, threads and
 * formatting come from the runtime library behind the iron_* functions
 * below; the runtime's own .c files include the real platform headers. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdatomic.h>

#include "runtime/iron_errors.h"

#if defined(__GNUC__) || defined(__clang__)
#define IRON_NORETURN __attribute__((noreturn))
#else
#define IRON_NORETURN
#endif

/* ── Memory primitives (iron_os.c) ─────────────────────────────────────────
 * Every allocation and byte operation of the runtime header's inline
 * helpers and of generated code goes through these; the runtime library
 * forwards them to the platform's libc. */
void  *iron_mem_alloc(size_t size);
void  *iron_mem_calloc(size_t count, size_t size);
void  *iron_mem_realloc(void *ptr, size_t size);
void   iron_mem_free(void *ptr);
void  *iron_mem_copy(void *dst, const void *src, size_t n);
void  *iron_mem_move(void *dst, const void *src, size_t n);
void  *iron_mem_set(void *dst, int byte, size_t n);
int    iron_mem_cmp(const void *a, const void *b, size_t n);
size_t iron_cstr_len(const char *s);
int    iron_cstr_cmp(const char *a, const char *b);
void   iron_sort(void *base, size_t count, size_t size,
                 int (*cmp)(const void *, const void *));
/* Allocation failure in a runtime container (iron_oom.c): reports `where`
 * and aborts. */
IRON_NORETURN void iron_oom_abort(const char *where);
/* Standard streams and process exit, for generated code (iron_os.c). */
void   iron_out_write(const char *bytes, size_t n);
void   iron_err_write(const char *bytes, size_t n);
void   iron_out_flush(void);
IRON_NORETURN void iron_exit(int code);
IRON_NORETURN void iron_abort(void);
/* printf-style formatting into a freshly allocated C string (iron_fmt.c);
 * the caller frees it with iron_mem_free. */
char  *iron_cstr_format(const char *fmt, ...);
/* Box.unwrap() on a null box. */
IRON_NORETURN void iron_panic_null_box(void);
/* FileHandle.open(path) (write mode) / close: the descriptor, or -1. */
int    iron_filehandle_open(const char *path);
void   iron_filehandle_close(int fd);

/* ── Atomics: C11 on every platform ──────────────────────────────────────── */
typedef atomic_int iron_atomic_int;
#define IRON_ATOMIC_INIT(v, val)          atomic_init(&(v), (val))
#define IRON_ATOMIC_LOAD(v)               atomic_load(&(v))
#define IRON_ATOMIC_FETCH_ADD(v, n)       atomic_fetch_add(&(v), (n))
#define IRON_ATOMIC_FETCH_SUB(v, n)       atomic_fetch_sub(&(v), (n))
#define IRON_ATOMIC_CAS_WEAK(v, exp, des) atomic_compare_exchange_weak(&(v), (exp), (des))

/* 64-bit counters with explicit ordering. Generation counters need
 * relaxed/acquire ordering; refcounts follow the Rust Arc discipline:
 * retain is a relaxed increment, release a release-decrement, and the
 * final drop fences for acquire before running the destructor. */
typedef _Atomic uint64_t iron_atomic_u64;
#define IRON_ATOMIC_U64_INIT(v, val) \
    atomic_init(&(v), (val))
/* Store on a LIVE atomic (post-init); atomic_init on an atomic other
 * threads use is C11 UB (arena reset/restore paths). RELEASE pairs with
 * the ACQUIRE loads in the gen-check / save paths. */
#define IRON_ATOMIC_U64_STORE_RELEASE(v, val) \
    atomic_store_explicit(&(v), (val), memory_order_release)
#define IRON_ATOMIC_U64_LOAD_ACQUIRE(v) \
    atomic_load_explicit(&(v), memory_order_acquire)
#define IRON_ATOMIC_U64_FETCH_ADD_RELAXED(v, n) \
    atomic_fetch_add_explicit(&(v), (n), memory_order_relaxed)
#define IRON_ATOMIC_U64_FETCH_SUB_RELEASE(v, n) \
    atomic_fetch_sub_explicit(&(v), (n), memory_order_release)
#define IRON_ATOMIC_U64_FETCH_SUB_RELAXED(v, n) \
    atomic_fetch_sub_explicit(&(v), (n), memory_order_relaxed)
#define IRON_ATOMIC_U64_CAS_WEAK_RELAXED(v, exp, des) \
    atomic_compare_exchange_weak_explicit(&(v), (exp), (des), \
                                          memory_order_relaxed, \
                                          memory_order_relaxed)
#define IRON_ATOMIC_FENCE_ACQUIRE() \
    atomic_thread_fence(memory_order_acquire)

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
 *              inc / RELEASE dec; starts at 1: the strong cohort owns a
 *              collective weak (Rust Arc scheme). Block free condition is
 *              the weak_count 1→0 edge — the final strong release runs
 *              drop_fn then releases the collective weak, so weak==0
 *              implies refcount==0. Single-counter free linearization;
 *              see iron_rc.c)
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
 *   iron_heap_free_dbg  — free with an explicit free-site. Codegen (emit_c.c)
 *                         ALWAYS emits this so the call site is stable across
 *                         build modes. In a debug build it records the free-
 *                         site, unlinks the registry, poisons, and reports the
 *                         SECOND/current free-site on a double-free. In a
 *                         release build it is a thin wrapper that ignores the
 *                         site and runs the plain generation-checked free.
 *                         Declared UNCONDITIONALLY so generated C links in
 *                         both modes.
 *   iron_leak_dump      — atexit handler (registered in iron_runtime_init);
 *                         walks the registry and reports still-live allocations
 *                         to STDERR with their alloc-site provenance (DBG-03).
 *   iron_debug_alloc_init — idempotent registry-lock initializer (DBG-03).
 *   iron_debug_quarantine_drain — release the freed blocks the debug allocator
 *                         retains so a UAF read still hits the 0xDD poison and a
 *                         double-free can still name the first free-site
 *                         (DBG-01/04). Called from iron_runtime_shutdown.
 */
void iron_heap_free_dbg(Iron_FatPtr fp, const char *free_file, int free_line);
#ifdef IRON_DEBUG_ALLOCATOR
void iron_leak_dump(void);
void iron_debug_alloc_init(void);
void iron_debug_quarantine_drain(void);
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

/* ── Phase 31 GA3 (Plan 31-03) — DBG-07 release opt-in leak check ───────────
 * The release 16B IronAllocHdr has no registry slots, so the release leak check
 * tracks live allocations in a SEPARATE side-table (src/runtime/iron_leakcheck.c)
 * keyed by the user pointer, armed only when IRON_LEAK_CHECK=1 (read once at
 * init). All four symbols are declared UNCONDITIONALLY and always linked: the
 * release alloc/free path calls _register/_unregister, which early-return at
 * zero cost when the env flag is unset (the common case). No poison in release.
 * In a debug build these are never CALLED (the in-header registry from Plan
 * 31-01 is the active mechanism), so the two trackers never double-count. */
void iron_leakcheck_init_from_env(void);
void iron_leakcheck_register(void *user_ptr, const char *site_file,
                             int site_line, uint64_t size);
void iron_leakcheck_unregister(void *user_ptr);
void iron_leakcheck_dump(void);

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
/* Process-wide monotonic generation source for heap and rc headers. */
extern iron_atomic_u64 iron_heap_gen_counter;
uint64_t iron_heap_next_gen(void);

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

/* Interface `var` parameter boundary: the callee rebound a wrapped concrete
 * binding to another implementor, so the write-back cannot proceed.
 * Forward-declared here for generated code; definition in
 * src/runtime/iron_panic.c, canonical declaration in iron_panic.h. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_iface_rebound(const char *site_file, int site_line,
                              const char *iface_name, const char *expected_impl);

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

/* Integer division/modulo by zero (DIV-01). Definition in iron_panic.c. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_div_by_zero(const char *site_file, int site_line);

/* Generic index out of bounds (LIST-01). Definition in iron_panic.c. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_index_oob(const char *site_file, int site_line,
                          int64_t index, int64_t bound);

/* Checked integer division/modulo (DIV-01). The emitter routes every integer
 * `/` and `%` through these so b == 0 becomes an Iron panic instead of a
 * SIGFPE/UB, and INT64_MIN / -1 wraps (matching the -fwrapv Int semantics)
 * instead of trapping. Narrower signed/unsigned operands promote in and
 * truncate back on assignment, which under wrap semantics is exact. */
static inline int64_t iron_idiv64(int64_t a, int64_t b,
                                  const char *site_file, int site_line) {
    if (b == 0) iron_panic_div_by_zero(site_file, site_line);
    if (b == -1) return (int64_t)(0u - (uint64_t)a); /* INT64_MIN-safe negate */
    return a / b;
}
static inline int64_t iron_imod64(int64_t a, int64_t b,
                                  const char *site_file, int site_line) {
    if (b == 0) iron_panic_div_by_zero(site_file, site_line);
    if (b == -1) return 0; /* INT64_MIN % -1 traps in hardware; result is 0 */
    return a % b;
}
static inline uint64_t iron_udiv64(uint64_t a, uint64_t b,
                                   const char *site_file, int site_line) {
    if (b == 0) iron_panic_div_by_zero(site_file, site_line);
    return a / b;
}
static inline uint64_t iron_umod64(uint64_t a, uint64_t b,
                                   const char *site_file, int site_line) {
    if (b == 0) iron_panic_div_by_zero(site_file, site_line);
    return a % b;
}

/* LIST-01: expression-form bounds check. Panics on i < 0 || i >= n (unsigned
 * compare covers both), otherwise returns i — so an inlined `arr[i]` in a
 * consumer expression becomes `arr[iron_bounds_idx(i, n, ...)]` without
 * needing a preceding statement. */
static inline int64_t iron_bounds_idx(int64_t i, int64_t n,
                                      const char *site_file, int site_line) {
    if ((uint64_t)i >= (uint64_t)n) iron_panic_index_oob(site_file, site_line, i, n);
    return i;
}

/* 2026-07 UNCHK-IDX: per-site unchecked indexing (`xs.get_unchecked(i)` /
 * `xs.set_unchecked(i, v)`). The emitter wraps the index of an author-declared
 * unchecked access in IRON_UNCHECKED_IDX instead of iron_bounds_idx:
 *   - normal build: expands to the bare index — raw access, zero overhead,
 *     no guard for clang's loop vectorizer to trip over. Out-of-bounds here
 *     is undefined behavior, exactly like C.
 *   - --debug-build (-DIRON_DEBUG_ALLOCATOR): keeps the full LIST-01 guard,
 *     panicking through iron_panic_index_oob_unchecked whose headline names
 *     the site as declared-unchecked ("index out of bounds (unchecked site)").
 * Keeping the build-mode split in the C preprocessor means one generated TU
 * serves both modes and the LIR/optimizer never need to know the build mode.
 * The macro is expression-form (composes as an array subscript) and its
 * arguments are emitter-materialized values with no side effects. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_index_oob_unchecked(const char *site_file, int site_line,
                                    int64_t index, int64_t bound);
#ifdef IRON_DEBUG_ALLOCATOR
static inline int64_t iron_bounds_idx_unchecked(int64_t i, int64_t n,
                                                const char *site_file,
                                                int site_line) {
    if ((uint64_t)i >= (uint64_t)n)
        iron_panic_index_oob_unchecked(site_file, site_line, i, n);
    return i;
}
#define IRON_UNCHECKED_IDX(i, n, site_file, site_line) \
    iron_bounds_idx_unchecked((i), (n), (site_file), (site_line))
#else
#define IRON_UNCHECKED_IDX(i, n, site_file, site_line) (i)
#endif

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

/* ── Threads, mutexes, condition variables, read-write locks (iron_os.c) ──
 * The handles are opaque storage large enough for the platform primitive
 * (pthread or Win32); iron_os.c checks the sizes with static assertions.
 * Every runtime thread (spawned tasks, the language server's workers) gets
 * the main thread's usual 8 MB stack: secondary threads otherwise get the
 * platform default (512 KB on macOS), where deep recursion overflowed. */
#ifndef IRON_THREAD_STACK_SIZE
#define IRON_THREAD_STACK_SIZE ((size_t)8 * 1024 * 1024)
#endif
typedef struct { _Alignas(16) unsigned char opaque[64];  } iron_mutex_t;
typedef struct { _Alignas(16) unsigned char opaque[64];  } iron_cond_t;
typedef struct { _Alignas(16) unsigned char opaque[256]; } iron_rwlock_t;
typedef struct { uintptr_t handle; } iron_thread_t;

/* One-time initialization shared by every thread: a C11 atomic state
 * machine (0 untouched, 1 running, 2 done), so a zero initializer is
 * valid on every platform (Apple's PTHREAD_ONCE_INIT is not zero). */
typedef struct { atomic_int state; } iron_once_t;
#define IRON_ONCE_INIT { 0 }
void iron_once(iron_once_t *once, void (*fn)(void));

int  iron_thread_create(iron_thread_t *t, void *(*fn)(void *), void *arg);
int  iron_thread_join(iron_thread_t t);
int  iron_thread_detach(iron_thread_t t);
iron_thread_t iron_thread_self(void);
bool iron_thread_equal(iron_thread_t a, iron_thread_t b);
void iron_mutex_init(iron_mutex_t *m);
void iron_mutex_lock(iron_mutex_t *m);
void iron_mutex_unlock(iron_mutex_t *m);
void iron_mutex_destroy(iron_mutex_t *m);
void iron_cond_init(iron_cond_t *c);
void iron_cond_wait(iron_cond_t *c, iron_mutex_t *m);
void iron_cond_signal(iron_cond_t *c);
void iron_cond_broadcast(iron_cond_t *c);
void iron_cond_destroy(iron_cond_t *c);
void iron_rwlock_init(iron_rwlock_t *l);
void iron_rwlock_rdlock(iron_rwlock_t *l);
void iron_rwlock_wrlock(iron_rwlock_t *l);
void iron_rwlock_rdunlock(iron_rwlock_t *l);
void iron_rwlock_wrunlock(iron_rwlock_t *l);
void iron_rwlock_destroy(iron_rwlock_t *l);

#define IRON_THREAD_CREATE(t,fn,arg)   iron_thread_create(&(t),(fn),(arg))
#define IRON_THREAD_JOIN(t)            iron_thread_join((t))
#define IRON_MUTEX_INIT(m)             iron_mutex_init(&(m))
#define IRON_MUTEX_LOCK(m)             iron_mutex_lock(&(m))
#define IRON_MUTEX_UNLOCK(m)           iron_mutex_unlock(&(m))
#define IRON_MUTEX_DESTROY(m)          iron_mutex_destroy(&(m))
#define IRON_COND_INIT(c)              iron_cond_init(&(c))
#define IRON_COND_WAIT(c,m)            iron_cond_wait(&(c), &(m))
#define IRON_COND_SIGNAL(c)            iron_cond_signal(&(c))
#define IRON_COND_BROADCAST(c)         iron_cond_broadcast(&(c))
#define IRON_COND_DESTROY(c)           iron_cond_destroy(&(c))
#define IRON_RWLOCK_INIT(l)            iron_rwlock_init(&(l))
#define IRON_RWLOCK_RDLOCK(l)          iron_rwlock_rdlock(&(l))
#define IRON_RWLOCK_WRLOCK(l)          iron_rwlock_wrlock(&(l))
#define IRON_RWLOCK_RDUNLOCK(l)        iron_rwlock_rdunlock(&(l))
#define IRON_RWLOCK_WRUNLOCK(l)        iron_rwlock_wrunlock(&(l))
#define IRON_RWLOCK_DESTROY(l)         iron_rwlock_destroy(&(l))

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
/* printf-style formatting of interpolation parts (iron_fmt.c). */
Iron_String  iron_string_format(const char *fmt, ...);
Iron_String  iron_string_from_literal(const char *lit, size_t byte_len);
const char  *iron_string_cstr(const Iron_String *s);
size_t       iron_string_byte_len(const Iron_String *s);
size_t       iron_string_codepoint_count(const Iron_String *s);
bool         iron_string_equals(const Iron_String *a, const Iron_String *b);
int          iron_string_compare(const Iron_String *a, const Iron_String *b);
Iron_String  iron_string_concat(const Iron_String *a, const Iron_String *b);
Iron_String  iron_string_intern(Iron_String s);
/* Consumes one owned string value. Heap-backed interned literals remain owned
 * by the runtime and are ignored; non-interned heap storage is freed. Any
 * by-value aliases of a released string become invalid and must not be used. */
void         iron_string_release(Iron_String *s);
/* Registers one more owner of the string's characters (a copy of the value
 * that will be released on its own). No-op for inline and interned strings. */
void         iron_string_retain(const Iron_String *s);

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
 * on prev == 1, acquire-fence then drop_fn(user_ptr) then iron_mem_free(block).
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
Iron_String Iron_read_file(Iron_String path);

/* ── Phase 78 FMT — Int/Int32/Float → String conversion ─────────────────
 * Defined in src/runtime/iron_fmt.c. Consumed by the Iron-side stubs in
 * src/stdlib/int.iron and src/stdlib/float.iron (landed in Plan 78-02).
 *
 * Iron_int_to_string   — signed 64-bit decimal (INT64_MIN safe).
 * Iron_int32_to_string — signed 32-bit decimal (INT32_MIN safe).
 * Iron_float_to_string — shortest round-trip digits (iron_fmt_float);
 *                        NaN/±Inf/-0.0 normalize to "NaN"/"inf"/"-inf"/"0".
 */
Iron_String Iron_int_to_string(int64_t n);
Iron_String Iron_int32_to_string(int32_t n);
Iron_String Iron_float_to_string(double f);
/* Shortest round-trip float formatting shared by to_string and string
 * interpolation; is_f32 picks the shortest form that round-trips as a
 * Float32.  out must hold IRON_FMT_FLOAT_BUF bytes; returns out. */
#define IRON_FMT_FLOAT_BUF 40
const char *iron_fmt_float(double v, bool is_f32, char *out);

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
typedef struct Iron_Handle {
    iron_thread_t   thread;
    bool            done;
    void           *result;
    iron_mutex_t    lock;
    iron_cond_t     cond;
    char           *panic_msg;
} Iron_Handle;

/* Handle API */
Iron_Handle *Iron_handle_create(void (*fn)(void *), void *arg);
Iron_Handle *Iron_handle_create_result(
    void (*fn)(void *, Iron_Handle *), void *arg);
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
/* Phase 37 rc-balance (M5): element-drop-aware destroy. Runs `elem_drop` on
 * each still-queued box payload before freeing the box (NULL = free only —
 * the historical behavior Iron_channel_destroy preserves). The per-T glue
 * (emit_helpers.c) passes the element type's destructor trampoline when T
 * has a drop/close obligation. */
void          Iron_channel_destroy_with(Iron_Channel *ch,
                                        void (*elem_drop)(void *));

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
/* Phase 37 rc-balance (M5): element-drop-aware destroy — runs `elem_drop`
 * on the wrapped value before freeing its storage (NULL = free only). */
void        Iron_mutex_destroy_with(Iron_Mutex *m,
                                    void (*elem_drop)(void *));

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

/* A capturing closure's env block is shared by every copy of the value
 * (#190): it sits behind a header holding an atomic reference count and
 * the env's own drop function (which releases the captures and frees the
 * block). A copy retains, a drop releases, the last release runs the
 * drop. A NULL env (no captures) is never counted. */
void *iron_closure_env_alloc(size_t env_size, void (*drop)(void *env));
void  iron_closure_env_free(void *env);          /* called by the env drop */

/* A `var` that a closure writes lives in a counted cell (#210): the frame
 * and every closure env that captured it share the cell, so the closure
 * may outlive the frame. The value sits behind the header; `drop`, when
 * set, destroys the value before the cell is freed. */
void *iron_cell_alloc(size_t size, void (*drop)(void *value));
void  iron_cell_retain(void *value);
void  iron_cell_release(void *value);
void  iron_closure_retain(Iron_Closure c);
void  iron_closure_release(Iron_Closure c);

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
 *   Map[String, Int] -> Iron_Map_Iron_String_int64_t (hash table, see IRON_HMAP_DEFINE)
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
/* qsort comparators for list.sort() on numeric and String elements.
 * Floats order NaN after every number so the order is total. */
#define IRON_SORT_CMP_NUM(name, T) \
    static inline int name(const void *pa, const void *pb) { \
        T a = *(const T *)pa, b = *(const T *)pb; \
        return (a > b) - (a < b); \
    }
IRON_SORT_CMP_NUM(iron_sort_cmp_int8_t,   int8_t)
IRON_SORT_CMP_NUM(iron_sort_cmp_int16_t,  int16_t)
IRON_SORT_CMP_NUM(iron_sort_cmp_int32_t,  int32_t)
IRON_SORT_CMP_NUM(iron_sort_cmp_int64_t,  int64_t)
IRON_SORT_CMP_NUM(iron_sort_cmp_uint8_t,  uint8_t)
IRON_SORT_CMP_NUM(iron_sort_cmp_uint16_t, uint16_t)
IRON_SORT_CMP_NUM(iron_sort_cmp_uint32_t, uint32_t)
IRON_SORT_CMP_NUM(iron_sort_cmp_uint64_t, uint64_t)
#define IRON_SORT_CMP_FLOAT(name, T) \
    static inline int name(const void *pa, const void *pb) { \
        T a = *(const T *)pa, b = *(const T *)pb; \
        if (a != a) return (b != b) ? 0 : 1; \
        if (b != b) return -1; \
        return (a > b) - (a < b); \
    }
IRON_SORT_CMP_FLOAT(iron_sort_cmp_float,  float)
IRON_SORT_CMP_FLOAT(iron_sort_cmp_double, double)
static inline int iron_sort_cmp_Iron_String(const void *pa, const void *pb) {
    return iron_string_compare((const Iron_String *)pa, (const Iron_String *)pb);
}

#define IRON_LIST_DECL(T, suffix) \
    Iron_List_##suffix Iron_List_##suffix##_create(void); \
    Iron_List_##suffix Iron_List_##suffix##_create_with_capacity(int64_t cap); \
    Iron_List_##suffix Iron_List_##suffix##_clone(const Iron_List_##suffix *src); \
    Iron_List_##suffix Iron_List_##suffix##_take(Iron_List_##suffix *self); \
    void               Iron_List_##suffix##_push(Iron_List_##suffix *self, T item); \
    T                  Iron_List_##suffix##_get(const Iron_List_##suffix *self, int64_t index); \
    void               Iron_List_##suffix##_set(Iron_List_##suffix *self, int64_t index, T item); \
    T                  Iron_List_##suffix##_pop(Iron_List_##suffix *self); \
    int64_t            Iron_List_##suffix##_len(const Iron_List_##suffix *self); \
    T                  Iron_List_##suffix##_remove(Iron_List_##suffix *self, int64_t index); \
    void               Iron_List_##suffix##_insert(Iron_List_##suffix *self, int64_t index, T item); \
    void               Iron_List_##suffix##_reverse(Iron_List_##suffix *self); \
    void               Iron_List_##suffix##_clear(Iron_List_##suffix *self); \
    void               Iron_List_##suffix##_free(Iron_List_##suffix *self);

/* Phase 33 STDLIB-02 (Plan 33-04): IRON_LIST_IMPL is split into
 *   - IRON_LIST_IMPL_CORE  : create / create_with_capacity / push / get / set /
 *                            pop / len  (the lifecycle-agnostic surface)
 *   - the trivial _clone (memcpy) + _free (iron_mem_free(items)) bodies
 * IRON_LIST_IMPL composes CORE + trivial lifecycle (the fast path, used for
 * primitive / trivial-struct element types — Pitfall 5). For element types
 * whose destructor/copy must run per element, the emitter (emit_structs.c
 * emit_mono_list_decls) instead emits IRON_LIST_IMPL_CORE plus a custom
 * element-destructor-aware _free / _clone — see the per-element drop/copy loop
 * gated on od_has_drop_lir / the FileHandle nocopy surface. */
#define IRON_LIST_IMPL_CORE(T, suffix) \
    Iron_List_##suffix Iron_List_##suffix##_take(Iron_List_##suffix *self) { \
        Iron_List_##suffix out = *self; \
        self->items = NULL; self->count = 0; self->capacity = 0; \
        return out; \
    } \
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
            l.items = (T *)iron_mem_alloc((size_t)cap * sizeof(T)); \
            if (!l.items) iron_oom_abort("Iron_List_" #suffix "_create_with_capacity"); \
        } \
        return l; \
    } \
    void Iron_List_##suffix##_push(Iron_List_##suffix *self, T item) { \
        if (self->count >= self->capacity) { \
            int64_t new_cap = self->capacity ? self->capacity * 2 : 8; \
            /* FIX-01/FIX-02: capacity doubling must not wrap int64_t (audit row 18) */ \
            if (new_cap < self->capacity) { \
                iron_oom_abort("Iron_List_" #suffix "_push: capacity overflow"); \
            } \
            T *new_items = (T *)iron_mem_realloc(self->items, (size_t)new_cap * sizeof(T)); \
            if (!new_items) iron_oom_abort("Iron_List_" #suffix "_push"); \
            self->items = new_items; \
            self->capacity = new_cap; \
        } \
        self->items[self->count++] = item; \
    } \
    T Iron_List_##suffix##_get(const Iron_List_##suffix *self, int64_t index) { \
        /* LIST-01: unsigned compare rejects negative and >= count in one test */ \
        if ((uint64_t)index >= (uint64_t)self->count) \
            iron_panic_index_oob("Iron_List_" #suffix "_get", 0, index, self->count); \
        return self->items[index]; \
    } \
    void Iron_List_##suffix##_set(Iron_List_##suffix *self, int64_t index, T item) { \
        if ((uint64_t)index >= (uint64_t)self->count) \
            iron_panic_index_oob("Iron_List_" #suffix "_set", 0, index, self->count); \
        self->items[index] = item; \
    } \
    T Iron_List_##suffix##_pop(Iron_List_##suffix *self) { \
        /* LIST-01: pop on empty read items[-1] and corrupted count to -1 */ \
        if (self->count <= 0) \
            iron_panic_index_oob("Iron_List_" #suffix "_pop", 0, -1, self->count); \
        return self->items[--self->count]; \
    } \
    T Iron_List_##suffix##_remove(Iron_List_##suffix *self, int64_t index) { \
        /* Removes and returns items[index], shifting the tail down. */ \
        if ((uint64_t)index >= (uint64_t)self->count) \
            iron_panic_index_oob("Iron_List_" #suffix "_remove", 0, index, self->count); \
        T removed = self->items[index]; \
        iron_mem_move(&self->items[index], &self->items[index + 1], \
                (size_t)(self->count - index - 1) * sizeof(T)); \
        self->count--; \
        return removed; \
    } \
    void Iron_List_##suffix##_insert(Iron_List_##suffix *self, int64_t index, T item) { \
        /* index == count appends. */ \
        if ((uint64_t)index > (uint64_t)self->count) \
            iron_panic_index_oob("Iron_List_" #suffix "_insert", 0, index, self->count + 1); \
        Iron_List_##suffix##_push(self, item); \
        iron_mem_move(&self->items[index + 1], &self->items[index], \
                (size_t)(self->count - 1 - index) * sizeof(T)); \
        self->items[index] = item; \
    } \
    void Iron_List_##suffix##_reverse(Iron_List_##suffix *self) { \
        for (int64_t i = 0, j = self->count - 1; i < j; i++, j--) { \
            T tmp = self->items[i]; \
            self->items[i] = self->items[j]; \
            self->items[j] = tmp; \
        } \
    } \
    int64_t Iron_List_##suffix##_len(const Iron_List_##suffix *self) { \
        return self->count; \
    }

/* Trivial (fast-path) _clone + _free: memcpy / iron_mem_free(items). Used for primitive
 * and trivial-struct element types where no per-element destructor/copy runs. */
#define IRON_LIST_IMPL_TRIVIAL_LIFECYCLE(T, suffix) \
    Iron_List_##suffix Iron_List_##suffix##_clone(const Iron_List_##suffix *src) { \
        Iron_List_##suffix dst; \
        dst.count = src->count; \
        dst.capacity = src->count; \
        if (src->count > 0) { \
            dst.items = (T *)iron_mem_alloc((size_t)src->count * sizeof(T)); \
            if (!dst.items) iron_oom_abort("Iron_List_" #suffix "_clone"); \
            iron_mem_copy(dst.items, src->items, (size_t)src->count * sizeof(T)); \
        } else { \
            dst.items = NULL; \
        } \
        return dst; \
    } \
    void Iron_List_##suffix##_clear(Iron_List_##suffix *self) { \
        self->count = 0; \
    } \
    void Iron_List_##suffix##_free(Iron_List_##suffix *self) { \
        iron_mem_free(self->items); \
        self->items = NULL; self->count = 0; self->capacity = 0; \
    }

/* IRON_LIST_IMPL is a full standalone body (NOT a composition of CORE +
 * TRIVIAL) so the `##suffix` paste happens before `suffix` is argument-
 * prescanned — composing would expand `bool`->`_Bool` and break the paste.
 * The CORE / TRIVIAL macros above are for the emitter, which always passes
 * already-mangled (non-keyword) names. */
#define IRON_LIST_IMPL(T, suffix) \
    Iron_List_##suffix Iron_List_##suffix##_take(Iron_List_##suffix *self) { \
        Iron_List_##suffix out = *self; \
        self->items = NULL; self->count = 0; self->capacity = 0; \
        return out; \
    } \
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
            l.items = (T *)iron_mem_alloc((size_t)cap * sizeof(T)); \
            if (!l.items) iron_oom_abort("Iron_List_" #suffix "_create_with_capacity"); \
        } \
        return l; \
    } \
    Iron_List_##suffix Iron_List_##suffix##_clone(const Iron_List_##suffix *src) { \
        Iron_List_##suffix dst; \
        dst.count = src->count; \
        dst.capacity = src->count; \
        if (src->count > 0) { \
            dst.items = (T *)iron_mem_alloc((size_t)src->count * sizeof(T)); \
            if (!dst.items) iron_oom_abort("Iron_List_" #suffix "_clone"); \
            iron_mem_copy(dst.items, src->items, (size_t)src->count * sizeof(T)); \
        } else { \
            dst.items = NULL; \
        } \
        return dst; \
    } \
    void Iron_List_##suffix##_push(Iron_List_##suffix *self, T item) { \
        if (self->count >= self->capacity) { \
            int64_t new_cap = self->capacity ? self->capacity * 2 : 8; \
            if (new_cap < self->capacity) { \
                iron_oom_abort("Iron_List_" #suffix "_push: capacity overflow"); \
            } \
            T *new_items = (T *)iron_mem_realloc(self->items, (size_t)new_cap * sizeof(T)); \
            if (!new_items) iron_oom_abort("Iron_List_" #suffix "_push"); \
            self->items = new_items; \
            self->capacity = new_cap; \
        } \
        self->items[self->count++] = item; \
    } \
    T Iron_List_##suffix##_get(const Iron_List_##suffix *self, int64_t index) { \
        /* LIST-01: unsigned compare rejects negative and >= count in one test */ \
        if ((uint64_t)index >= (uint64_t)self->count) \
            iron_panic_index_oob("Iron_List_" #suffix "_get", 0, index, self->count); \
        return self->items[index]; \
    } \
    void Iron_List_##suffix##_set(Iron_List_##suffix *self, int64_t index, T item) { \
        if ((uint64_t)index >= (uint64_t)self->count) \
            iron_panic_index_oob("Iron_List_" #suffix "_set", 0, index, self->count); \
        self->items[index] = item; \
    } \
    T Iron_List_##suffix##_pop(Iron_List_##suffix *self) { \
        /* LIST-01: pop on empty read items[-1] and corrupted count to -1 */ \
        if (self->count <= 0) \
            iron_panic_index_oob("Iron_List_" #suffix "_pop", 0, -1, self->count); \
        return self->items[--self->count]; \
    } \
    T Iron_List_##suffix##_remove(Iron_List_##suffix *self, int64_t index) { \
        /* Removes and returns items[index], shifting the tail down. */ \
        if ((uint64_t)index >= (uint64_t)self->count) \
            iron_panic_index_oob("Iron_List_" #suffix "_remove", 0, index, self->count); \
        T removed = self->items[index]; \
        iron_mem_move(&self->items[index], &self->items[index + 1], \
                (size_t)(self->count - index - 1) * sizeof(T)); \
        self->count--; \
        return removed; \
    } \
    void Iron_List_##suffix##_insert(Iron_List_##suffix *self, int64_t index, T item) { \
        /* index == count appends. */ \
        if ((uint64_t)index > (uint64_t)self->count) \
            iron_panic_index_oob("Iron_List_" #suffix "_insert", 0, index, self->count + 1); \
        Iron_List_##suffix##_push(self, item); \
        iron_mem_move(&self->items[index + 1], &self->items[index], \
                (size_t)(self->count - 1 - index) * sizeof(T)); \
        self->items[index] = item; \
    } \
    void Iron_List_##suffix##_reverse(Iron_List_##suffix *self) { \
        for (int64_t i = 0, j = self->count - 1; i < j; i++, j--) { \
            T tmp = self->items[i]; \
            self->items[i] = self->items[j]; \
            self->items[j] = tmp; \
        } \
    } \
    int64_t Iron_List_##suffix##_len(const Iron_List_##suffix *self) { \
        return self->count; \
    } \
    void Iron_List_##suffix##_clear(Iron_List_##suffix *self) { \
        self->count = 0; \
    } \
    void Iron_List_##suffix##_free(Iron_List_##suffix *self) { \
        iron_mem_free(self->items); \
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
        iron_mem_copy(&map_fn, &f.fn, sizeof(map_fn)); \
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
        iron_mem_copy(&filter_fn, &f.fn, sizeof(filter_fn)); \
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
        iron_mem_copy(&reduce_fn, &f.fn, sizeof(reduce_fn)); \
        T acc = init; \
        for (int64_t i = 0; i < self->count; i++) { \
            acc = reduce_fn(f.env, acc, self->items[i]); \
        } \
        return acc; \
    } \
    void Iron_List_##suffix##_forEach(const Iron_List_##suffix *self, Iron_Closure f) { \
        typedef void (*ForEachFn)(void *, T); \
        ForEachFn each_fn; \
        iron_mem_copy(&each_fn, &f.fn, sizeof(each_fn)); \
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

/* ── Hash containers: Map[K, V] and Set[T] ───────────────────────────────────
 * Open addressing with linear probing; slot states EMPTY / FULL / DELETED.
 * The table grows at 70% occupancy (live + tombstones) and never shrinks.
 *
 * The compiler instantiates one table per concrete (K, V) by emitting six
 * small static helpers and then expanding IRON_HMAP_DEFINE (see
 * emit_ensure_map in src/lir/emit_helpers.c):
 *   uint64_t NAME_khash(const K *k);         hash of a key
 *   bool     NAME_keq(const K *a, const K *b); key equality
 *   void     NAME_kcopy(K *k);  NAME_kdrop(K *k);   key copy fixup / drop
 *   void     NAME_vcopy(V *v);  NAME_vdrop(V *v);   value copy fixup / drop
 * Copy fixups are the same ones every other container applies when a value
 * is duplicated (string retain, rc retain, list clone, object copy glue);
 * drops are what scope exit runs. Keys and values are owned by the table:
 * put takes ownership of its arguments, remove and free drop what they
 * remove, get and get_or hand out a fixed-up copy.
 */
#define IRON_HSLOT_EMPTY   0
#define IRON_HSLOT_FULL    1
#define IRON_HSLOT_DELETED 2

/* splitmix64 finalizer: a strong integer mixer, also used over user hash(). */
static inline uint64_t iron_hash_u64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
/* FNV-1a over bytes, mixed once more so short keys spread. */
static inline uint64_t iron_hash_bytes(const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return iron_hash_u64(h);
}
static inline uint64_t iron_hash_f64(double d) {
    if (d == 0.0) d = 0.0;              /* -0.0 and 0.0 compare equal */
    uint64_t bits; iron_mem_copy(&bits, &d, sizeof bits);
    return iron_hash_u64(bits);
}
uint64_t iron_string_hash(const Iron_String *s);

/* A key that is not in the map: `m.get(k)` panics (iron_panic.c). */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_key_missing(const char *site_file, int site_line);

/* Every instantiation lands in the program's single translation unit, so
 * the helpers are static; unused ones must not trip -Wunused-function. */
#if defined(__GNUC__) || defined(__clang__)
#define IRON_HT_FN static inline __attribute__((unused))
#else
#define IRON_HT_FN static inline
#endif

#define IRON_HMAP_DEFINE(NAME, K, V) \
    typedef struct NAME { K *keys; V *vals; uint8_t *st; int64_t count; int64_t used; int64_t cap; } NAME; \
    IRON_HT_FN NAME NAME##_create(void) { NAME m; iron_mem_set(&m, 0, sizeof m); return m; } \
    IRON_HT_FN int64_t NAME##_find(const NAME *m, const K *k) { \
        if (m->cap == 0) return -1; \
        uint64_t mask = (uint64_t)m->cap - 1; \
        uint64_t i = NAME##_khash(k) & mask; \
        for (;;) { \
            uint8_t s = m->st[i]; \
            if (s == IRON_HSLOT_EMPTY) return -1; \
            if (s == IRON_HSLOT_FULL && NAME##_keq(&m->keys[i], k)) return (int64_t)i; \
            i = (i + 1) & mask; \
        } \
    } \
    IRON_HT_FN void NAME##_rehash(NAME *m, int64_t ncap) { \
        K *nk = (K *)iron_mem_alloc((size_t)ncap * sizeof(K)); \
        V *nv = (V *)iron_mem_alloc((size_t)ncap * sizeof(V)); \
        uint8_t *ns = (uint8_t *)iron_mem_calloc((size_t)ncap, 1); \
        if (!nk || !nv || !ns) iron_oom_abort(#NAME "_rehash"); \
        uint64_t mask = (uint64_t)ncap - 1; \
        for (int64_t i = 0; i < m->cap; i++) { \
            if (m->st[i] != IRON_HSLOT_FULL) continue; \
            uint64_t j = NAME##_khash(&m->keys[i]) & mask; \
            while (ns[j] == IRON_HSLOT_FULL) j = (j + 1) & mask; \
            nk[j] = m->keys[i]; nv[j] = m->vals[i]; ns[j] = IRON_HSLOT_FULL; \
        } \
        iron_mem_free(m->keys); iron_mem_free(m->vals); iron_mem_free(m->st); \
        m->keys = nk; m->vals = nv; m->st = ns; m->cap = ncap; m->used = m->count; \
    } \
    IRON_HT_FN void NAME##_put(NAME *m, K key, V val) { \
        int64_t at = NAME##_find(m, &key); \
        if (at >= 0) { NAME##_kdrop(&key); NAME##_vdrop(&m->vals[at]); m->vals[at] = val; return; } \
        if ((m->used + 1) * 10 > m->cap * 7) NAME##_rehash(m, m->cap ? m->cap * 2 : 8); \
        uint64_t mask = (uint64_t)m->cap - 1; \
        uint64_t i = NAME##_khash(&key) & mask; \
        while (m->st[i] == IRON_HSLOT_FULL) i = (i + 1) & mask; \
        if (m->st[i] == IRON_HSLOT_EMPTY) m->used++; \
        m->keys[i] = key; m->vals[i] = val; m->st[i] = IRON_HSLOT_FULL; m->count++; \
    } \
    IRON_HT_FN bool NAME##_has(const NAME *m, K key) { \
        bool r = NAME##_find(m, &key) >= 0; NAME##_kdrop(&key); return r; \
    } \
    IRON_HT_FN V NAME##_get_at(const NAME *m, K key, const char *site_file, int site_line) { \
        int64_t at = NAME##_find(m, &key); NAME##_kdrop(&key); \
        if (at < 0) iron_panic_key_missing(site_file, site_line); \
        V out = m->vals[at]; NAME##_vcopy(&out); return out; \
    } \
    IRON_HT_FN V NAME##_get_or(const NAME *m, K key, V dflt) { \
        int64_t at = NAME##_find(m, &key); NAME##_kdrop(&key); \
        if (at < 0) return dflt; \
        NAME##_vdrop(&dflt); \
        V out = m->vals[at]; NAME##_vcopy(&out); return out; \
    } \
    IRON_HT_FN bool NAME##_remove(NAME *m, K key) { \
        int64_t at = NAME##_find(m, &key); NAME##_kdrop(&key); \
        if (at < 0) return false; \
        NAME##_kdrop(&m->keys[at]); NAME##_vdrop(&m->vals[at]); \
        m->st[at] = IRON_HSLOT_DELETED; m->count--; return true; \
    } \
    IRON_HT_FN int64_t NAME##_len(const NAME *m) { return m->count; } \
    IRON_HT_FN void NAME##_clear(NAME *m) { \
        for (int64_t i = 0; i < m->cap; i++) { \
            if (m->st[i] != IRON_HSLOT_FULL) continue; \
            NAME##_kdrop(&m->keys[i]); NAME##_vdrop(&m->vals[i]); \
        } \
        if (m->st) iron_mem_set(m->st, 0, (size_t)m->cap); \
        m->count = 0; m->used = 0; \
    } \
    IRON_HT_FN void NAME##_free(NAME *m) { \
        NAME##_clear(m); \
        iron_mem_free(m->keys); iron_mem_free(m->vals); iron_mem_free(m->st); \
        m->keys = NULL; m->vals = NULL; m->st = NULL; m->cap = 0; \
    } \
    IRON_HT_FN NAME NAME##_clone(const NAME *src) { \
        NAME dst; iron_mem_set(&dst, 0, sizeof dst); \
        if (src->count == 0) return dst; \
        int64_t ncap = 8; while (src->count * 10 > ncap * 7) ncap *= 2; \
        NAME##_rehash(&dst, ncap); \
        for (int64_t i = 0; i < src->cap; i++) { \
            if (src->st[i] != IRON_HSLOT_FULL) continue; \
            K k = src->keys[i]; V v = src->vals[i]; \
            NAME##_kcopy(&k); NAME##_vcopy(&v); \
            NAME##_put(&dst, k, v); \
        } \
        return dst; \
    } \
    IRON_HT_FN NAME NAME##_take(NAME *src) { NAME out = *src; iron_mem_set(src, 0, sizeof *src); return out; }

#define IRON_HSET_DEFINE(NAME, T) \
    typedef struct NAME { T *items; uint8_t *st; int64_t count; int64_t used; int64_t cap; } NAME; \
    IRON_HT_FN NAME NAME##_create(void) { NAME s; iron_mem_set(&s, 0, sizeof s); return s; } \
    IRON_HT_FN int64_t NAME##_find(const NAME *s, const T *k) { \
        if (s->cap == 0) return -1; \
        uint64_t mask = (uint64_t)s->cap - 1; \
        uint64_t i = NAME##_khash(k) & mask; \
        for (;;) { \
            uint8_t st = s->st[i]; \
            if (st == IRON_HSLOT_EMPTY) return -1; \
            if (st == IRON_HSLOT_FULL && NAME##_keq(&s->items[i], k)) return (int64_t)i; \
            i = (i + 1) & mask; \
        } \
    } \
    IRON_HT_FN void NAME##_rehash(NAME *s, int64_t ncap) { \
        T *ni = (T *)iron_mem_alloc((size_t)ncap * sizeof(T)); \
        uint8_t *ns = (uint8_t *)iron_mem_calloc((size_t)ncap, 1); \
        if (!ni || !ns) iron_oom_abort(#NAME "_rehash"); \
        uint64_t mask = (uint64_t)ncap - 1; \
        for (int64_t i = 0; i < s->cap; i++) { \
            if (s->st[i] != IRON_HSLOT_FULL) continue; \
            uint64_t j = NAME##_khash(&s->items[i]) & mask; \
            while (ns[j] == IRON_HSLOT_FULL) j = (j + 1) & mask; \
            ni[j] = s->items[i]; ns[j] = IRON_HSLOT_FULL; \
        } \
        iron_mem_free(s->items); iron_mem_free(s->st); \
        s->items = ni; s->st = ns; s->cap = ncap; s->used = s->count; \
    } \
    IRON_HT_FN bool NAME##_add(NAME *s, T item) { \
        if (NAME##_find(s, &item) >= 0) { NAME##_kdrop(&item); return false; } \
        if ((s->used + 1) * 10 > s->cap * 7) NAME##_rehash(s, s->cap ? s->cap * 2 : 8); \
        uint64_t mask = (uint64_t)s->cap - 1; \
        uint64_t i = NAME##_khash(&item) & mask; \
        while (s->st[i] == IRON_HSLOT_FULL) i = (i + 1) & mask; \
        if (s->st[i] == IRON_HSLOT_EMPTY) s->used++; \
        s->items[i] = item; s->st[i] = IRON_HSLOT_FULL; s->count++; return true; \
    } \
    IRON_HT_FN bool NAME##_has(const NAME *s, T item) { \
        bool r = NAME##_find(s, &item) >= 0; NAME##_kdrop(&item); return r; \
    } \
    IRON_HT_FN bool NAME##_remove(NAME *s, T item) { \
        int64_t at = NAME##_find(s, &item); NAME##_kdrop(&item); \
        if (at < 0) return false; \
        NAME##_kdrop(&s->items[at]); s->st[at] = IRON_HSLOT_DELETED; s->count--; return true; \
    } \
    IRON_HT_FN int64_t NAME##_len(const NAME *s) { return s->count; } \
    IRON_HT_FN void NAME##_clear(NAME *s) { \
        for (int64_t i = 0; i < s->cap; i++) \
            if (s->st[i] == IRON_HSLOT_FULL) NAME##_kdrop(&s->items[i]); \
        if (s->st) iron_mem_set(s->st, 0, (size_t)s->cap); \
        s->count = 0; s->used = 0; \
    } \
    IRON_HT_FN void NAME##_free(NAME *s) { \
        NAME##_clear(s); iron_mem_free(s->items); iron_mem_free(s->st); \
        s->items = NULL; s->st = NULL; s->cap = 0; \
    } \
    IRON_HT_FN NAME NAME##_clone(const NAME *src) { \
        NAME dst; iron_mem_set(&dst, 0, sizeof dst); \
        if (src->count == 0) return dst; \
        int64_t ncap = 8; while (src->count * 10 > ncap * 7) ncap *= 2; \
        NAME##_rehash(&dst, ncap); \
        for (int64_t i = 0; i < src->cap; i++) { \
            if (src->st[i] != IRON_HSLOT_FULL) continue; \
            T k = src->items[i]; NAME##_kcopy(&k); NAME##_add(&dst, k); \
        } \
        return dst; \
    } \
    IRON_HT_FN NAME NAME##_take(NAME *src) { NAME out = *src; iron_mem_set(src, 0, sizeof *src); return out; }

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
Iron_List_Iron_String Iron_string_chars(Iron_String self);
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
int64_t               Iron_string_byte_len(Iron_String self);
void                  Iron_string_release(Iron_String self);

#endif /* IRON_RUNTIME_H */
