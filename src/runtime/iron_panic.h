/* iron_panic.h — Phase 19 stale-pointer panic API (runtime-side).
 *
 * The user-facing declaration of iron_panic_stale_pointer lives in
 * src/diagnostics/diagnostics.h next to iron_oom_abort (single coherent
 * "how we die" surface). This header additionally exposes
 * iron_panic_init_from_env, called from iron_runtime_init in
 * src/runtime/iron_string.c to cache the IRON_PANIC_FORMAT environment
 * variable BEFORE any allocation can panic (Pitfall 6: getenv-once-at-
 * init).
 *
 * Definition: src/runtime/iron_panic.c
 * Layout / ABI lock: docs/dev/POINTER-LAYOUT.md (Plan 19-03 closeout).
 */
#ifndef IRON_PANIC_H
#define IRON_PANIC_H

#include "diagnostics/diagnostics.h"  /* iron_panic_stale_pointer + iron_panic_stale_stack_pointer */

/* Read IRON_PANIC_FORMAT env once and cache the result.
 * Idempotent across repeated iron_runtime_init calls (test-harness pattern).
 * Must be called BEFORE any allocation that could panic. */
void iron_panic_init_from_env(void);

/* Phase 20 PTR-10 (OQ-B Option C): stack-pointer panic helper.
 * Re-declared here (canonical declaration in diagnostics.h) so callers
 * inside src/runtime/ — notably the static-inline iron_check_stack_pointer_gen
 * in iron_runtime.h — can resolve the symbol with __attribute__((noreturn))
 * intact even when iron_runtime.h is included in TU's that don't pull
 * diagnostics.h into scope first. Definition in src/runtime/iron_panic.c. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_stale_stack_pointer(const char *deref_file,
                                    int deref_line,
                                    uint64_t captured_frame_gen);

/* Phase 23 VEC-03: bounded vector out-of-bounds panic.
 * Called inline from generated C at every push and index site.
 *   deref_file / deref_line : __FILE__ / __LINE__ at the access site
 *   index                   : requesting index (or current len at push site)
 *   bound                   : limit (N at push site; bv.len at index site)
 * Reuses s_iron_panic_format channel cache (no malloc, fputs/fprintf only).
 * Definition in src/runtime/iron_panic.c.
 * Forward-declared in src/runtime/iron_runtime.h for generated user binaries. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_bvec_oob(const char *deref_file,
                         int deref_line,
                         int64_t index,
                         int64_t bound);

/* Phase 24 DROP-04 (Plan 24-03): panicking destructor abort.
 * Called when any iron_panic_* fires while iron_in_destructor == true.
 * Mirrors iron_panic_bvec_oob: no malloc, fputs/fprintf only, reuses
 * s_iron_panic_format channel cache.
 * JSON channel: {"panic":"destructor_aborted","type":"<T>","drop_site":{"file":"<f>","line":<l>}}
 * Text channel: iron: destructor panicked\n  type: <T>\n  drop site: <f>:<l>\n
 * Finishes with abort() (noreturn).
 * Definition in src/runtime/iron_panic.c.
 * Forward-declared in src/runtime/iron_runtime.h for generated user binaries. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_destructor_aborted(const char *type_name,
                                    const char *drop_site_file,
                                    int drop_site_line);

/* Phase 28 GA1 / ARENA-10 (Plan 28-02): arena panic helpers.
 * Re-declared here (canonical declarations in diagnostics.h) so callers inside
 * src/runtime/ — notably iron_arena_rt.c's capacity-exhaustion path and the
 * static-inline iron_check_arena_pointer_gen in iron_runtime.h — resolve the
 * symbols with __attribute__((noreturn)) intact. Definitions in
 * src/runtime/iron_panic.c. */
struct IronArenaAllocHdr;  /* forward declaration; full def in runtime/iron_arena_rt.h */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_arena_stale(const char *deref_file,
                            int deref_line,
                            const struct IronArenaAllocHdr *hdr);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_arena_oom(const char *arena_name,
                          uint64_t requested_size,
                          uint64_t capacity);

/* Phase 24 DROP-05 (Plan 24-03): partial-init cleanup machinery.
 * HIR-lower instrumentation registers each self.field assignment in an init
 * body via iron_init_cleanup_register so that a panic mid-init unwinds
 * already-initialized fields in reverse-assignment (LIFO) order.
 * TLS definitions live in iron_panic.c; extern declarations also in
 * iron_runtime.h for generated user binaries. */
#ifndef IRON_INIT_CLEANUP_ENTRY_DEFINED
#define IRON_INIT_CLEANUP_ENTRY_DEFINED
typedef struct IronInitCleanupEntry {
    void (*drop_fn)(void *);
    void *field_ptr;
    struct IronInitCleanupEntry *prev;
} IronInitCleanupEntry;
#endif /* IRON_INIT_CLEANUP_ENTRY_DEFINED */

void iron_init_cleanup_register(IronInitCleanupEntry *entry,
                                 void (*drop_fn)(void *),
                                 void *field_ptr);
void iron_init_cleanup_run_and_clear(void);

/* Phase 24 DROP-04/05 (Plan 24-03): TLS state for panic-trap + cleanup.
 * Definitions in iron_panic.c; extern declarations also in iron_runtime.h. */
extern _Thread_local IronInitCleanupEntry *iron_init_cleanup_top;
extern _Thread_local bool iron_in_destructor;
extern _Thread_local const char *iron_current_dropping_type;

#endif /* IRON_PANIC_H */
