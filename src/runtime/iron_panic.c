/* iron_panic.c — Phase 19 stale-pointer panic implementation.
 *
 * Mirrors iron_oom.c structure: stderr write → fflush → abort. Differences:
 *   1. multi-line text block instead of one line
 *   2. JSON alternative gated by IRON_PANIC_FORMAT env (cached at
 *      iron_runtime_init time per Pitfall 6)
 *   3. richer diagnostic payload (deref site + alloc site in debug)
 *
 * Panic-during-panic defense (Pitfall 1):
 *   - This file uses ONLY fputs/fprintf with compile-time format strings
 *     and stack-only buffers. No malloc, no Iron_String, no iron_heap_*.
 *   - The __FILE__ strings stored in IronAllocHdr are string literals
 *     (static-storage); pointer dereference yields stable memory.
 *   - fputs/fprintf are NOT async-signal-safe in the strict sense, but
 *     iron_oom_abort uses the same pattern; the precedent is locked.
 *
 * Layout / API lock: docs/dev/POINTER-LAYOUT.md (Plan 19-03 closeout).
 * Single-TU implementation discipline: only this TU defines the panic
 * function and only this TU defines iron_panic_init_from_env.
 */

#include "runtime/iron_runtime.h"   /* IronAllocHdr full definition; needed for hdr->fields access */
#include "runtime/iron_arena_rt.h"  /* IronArenaAllocHdr full def — iron_panic_arena_stale reads hdr->size */
#include "runtime/iron_panic.h"
#include "diagnostics/diagnostics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cached at iron_runtime_init time. 0 = text (default), 1 = json.
 * File-scope static; never re-read on the panic path. */
static int s_iron_panic_format = 0;

/* Phase 24 DROP-04/05 (Plan 24-03): TLS state for panic-trap + partial-init cleanup.
 * iron_in_destructor: set true by codegen before each <T>_drop call; checked
 *   at top of every iron_panic_* function to divert to iron_panic_destructor_aborted.
 * iron_current_dropping_type: set by codegen alongside iron_in_destructor so
 *   iron_panic_destructor_aborted can name the type being dropped.
 * iron_init_cleanup_top: LIFO linked-list head for partial-init cleanup entries;
 *   each self.field assignment in an init body pushes an entry; on panic,
 *   iron_init_cleanup_run_and_clear walks LIFO calling each drop_fn. */
_Thread_local IronInitCleanupEntry *iron_init_cleanup_top = NULL;
_Thread_local bool iron_in_destructor = false;
_Thread_local const char *iron_current_dropping_type = NULL;

void iron_panic_init_from_env(void) {
    /* Single getenv per process (idempotent across repeated init calls in
     * test harnesses). NEVER called from the panic path itself. Pitfall 6
     * mitigation: per-panic getenv would not be async-signal-safe and may
     * itself allocate or take a lock. */
    const char *env = getenv("IRON_PANIC_FORMAT");
    if (env && strcmp(env, "json") == 0) {
        s_iron_panic_format = 1;
    } else {
        /* Explicit reset so a repeated iron_runtime_init in a test harness
         * does NOT latch a stale json value from a previous test run. */
        s_iron_panic_format = 0;
    }
}

/* Phase 20 PTR-10: stack-pointer panic helper (OQ-B Option C lock).
 *
 * Mirrors iron_panic_stale_pointer text + JSON channels but with a
 * distinct header substring ("dangling stack pointer to frame") and a
 * distinct JSON "panic":"stack_pointer" tag. Stack pointers carry no
 * IronAllocHdr; the captured_frame_gen parameter is the value stored in
 * Iron_FatPtr.gen at &-site, compared against current iron_stack_gen by
 * the caller (iron_check_stack_pointer_gen).
 *
 * Reuses the same s_iron_panic_format channel-pick state cached at
 * iron_runtime_init time per Pitfall 6 (single getenv-once-at-init);
 * panic-during-panic discipline matches iron_panic_stale_pointer (no
 * malloc, no Iron_String, only fputs/fprintf with stack buffers). */
void iron_panic_stale_stack_pointer(const char *deref_file,
                                    int deref_line,
                                    uint64_t captured_frame_gen) {
    /* Phase 24 DROP-04/05 (Plan 24-03): init-time cleanup + drop-time abort divert */
    if (iron_init_cleanup_top) iron_init_cleanup_run_and_clear();
    if (iron_in_destructor) {
        iron_panic_destructor_aborted(iron_current_dropping_type, __FILE__, __LINE__);
        /* noreturn — abort() inside */
    }
    const char *df = deref_file ? deref_file : "<unknown>";

    if (s_iron_panic_format == 1) {
        /* JSON line — distinct "panic":"stack_pointer" tag. */
        fputs("{\"panic\":\"stack_pointer\",", stderr);
        fprintf(stderr, "\"deref_site\":{\"file\":\"%s\",\"line\":%d}",
                df, deref_line);
        fprintf(stderr,
                ",\"captured_frame_gen\":%llu,\"current_stack_gen\":%llu",
                (unsigned long long)captured_frame_gen,
                (unsigned long long)iron_stack_gen);
        fputc('}', stderr);
        fputc('\n', stderr);
    } else {
        /* Text format — distinct header substring asserted by
         * tests/unit/test_runtime_stack_pointer_dangling.c */
        fputs("iron: dangling stack pointer to frame\n", stderr);
        fprintf(stderr, "  deref site: %s:%d\n", df, deref_line);
        fprintf(stderr,
                "  captured frame: #%llu (current frame: #%llu)\n",
                (unsigned long long)captured_frame_gen,
                (unsigned long long)iron_stack_gen);
    }
    fflush(stderr);
    abort();
}

/* Phase 23 VEC-03: bounded vector out-of-bounds panic.
 *
 * Mirrors iron_panic_stale_stack_pointer shape: no malloc, fputs/fprintf only,
 * reuses s_iron_panic_format channel cache (getenv-once-at-init Pitfall 6).
 * JSON channel:  {"panic":"bvec_oob","deref_site":{"file":"...","line":N},"index":I,"bound":B}
 * Text channel:  iron: bounded vector access out of bounds
 *                  deref site: <file>:<line>
 *                  index: <I> >= bound: <B>
 * Finishes with abort() (noreturn). */
void iron_panic_bvec_oob(const char *deref_file,
                         int deref_line,
                         int64_t index,
                         int64_t bound) {
    /* Phase 24 DROP-04/05 (Plan 24-03): init-time cleanup + drop-time abort divert */
    if (iron_init_cleanup_top) iron_init_cleanup_run_and_clear();
    if (iron_in_destructor) {
        iron_panic_destructor_aborted(iron_current_dropping_type, __FILE__, __LINE__);
        /* noreturn — abort() inside */
    }
    const char *df = deref_file ? deref_file : "<unknown>";

    if (s_iron_panic_format == 1) {
        /* JSON line — distinct "panic":"bvec_oob" tag */
        fputs("{\"panic\":\"bvec_oob\",", stderr);
        fprintf(stderr, "\"deref_site\":{\"file\":\"%s\",\"line\":%d}", df, deref_line);
        fprintf(stderr, ",\"index\":%lld,\"bound\":%lld",
                (long long)index, (long long)bound);
        fputc('}', stderr);
        fputc('\n', stderr);
    } else {
        /* Text format */
        fputs("iron: bounded vector access out of bounds\n", stderr);
        fprintf(stderr, "  deref site: %s:%d\n", df, deref_line);
        fprintf(stderr, "  index: %lld >= bound: %lld\n",
                (long long)index, (long long)bound);
    }
    fflush(stderr);
    abort();
}

/* Phase 24 DROP-04 (Plan 24-03): partial-init cleanup helpers.
 *
 * iron_init_cleanup_register: push a cleanup entry (stack-allocated by caller)
 *   onto the TLS linked list head; O(1) linked-list push.
 * iron_init_cleanup_run_and_clear: walk LIFO calling each drop_fn(field_ptr),
 *   then unconditionally reset iron_init_cleanup_top = NULL (Pitfall 5:
 *   ALWAYS reset even on success path to avoid stale entries leaking
 *   across Unity test harness runs). */
void iron_init_cleanup_register(IronInitCleanupEntry *entry,
                                 void (*drop_fn)(void *),
                                 void *field_ptr) {
    if (!entry) return;
    entry->drop_fn   = drop_fn;
    entry->field_ptr = field_ptr;
    entry->prev      = iron_init_cleanup_top;
    iron_init_cleanup_top = entry;
}

void iron_init_cleanup_run_and_clear(void) {
    IronInitCleanupEntry *e = iron_init_cleanup_top;
    while (e) {
        IronInitCleanupEntry *prev = e->prev;
        if (e->drop_fn && e->field_ptr) {
            e->drop_fn(e->field_ptr);
        }
        e = prev;
    }
    iron_init_cleanup_top = NULL;  /* Pitfall 5: ALWAYS reset */
}

/* Phase 24 DROP-04 (Plan 24-03): panicking destructor abort.
 *
 * Called when any iron_panic_* fires while iron_in_destructor == true.
 * Mirrors iron_panic_bvec_oob (no malloc, fputs/fprintf only, reuses
 * s_iron_panic_format channel cache).
 * JSON channel: {"panic":"destructor_aborted","type":"<T>","drop_site":{"file":"<f>","line":<l>}}
 * Text channel: iron: destructor panicked\n  type: <T>\n  drop site: <f>:<l>\n */
void iron_panic_destructor_aborted(const char *type_name,
                                    const char *drop_site_file,
                                    int drop_site_line) {
    const char *tn = type_name      ? type_name      : "<unknown>";
    const char *df = drop_site_file ? drop_site_file : "<unknown>";

    if (s_iron_panic_format == 1) {
        /* JSON line — distinct "panic":"destructor_aborted" tag */
        fputs("{\"panic\":\"destructor_aborted\",", stderr);
        fprintf(stderr, "\"type\":\"%s\",", tn);
        fprintf(stderr, "\"drop_site\":{\"file\":\"%s\",\"line\":%d}",
                df, drop_site_line);
        fputc('}', stderr);
        fputc('\n', stderr);
    } else {
        /* Text format */
        fputs("iron: destructor panicked\n", stderr);
        fprintf(stderr, "  type: %s\n", tn);
        fprintf(stderr, "  drop site: %s:%d\n", df, drop_site_line);
    }
    fflush(stderr);
    abort();
}

/* Phase 28 GA1 (Plan 28-02): arena-stale-pointer panic helper.
 *
 * Fired by iron_check_arena_pointer_gen when a fat pointer's generation
 * snapshot no longer matches the arena's live generation — i.e. the arena was
 * reset()/restore()'d since the pointer was taken. Mirrors
 * iron_panic_stale_pointer's dual text/JSON channel, in-destructor divert, and
 * init-cleanup pump. Distinct header substring ("stale arena pointer
 * dereference") + JSON "panic":"arena_pointer" tag. hdr->size is reported when
 * available. noreturn (abort). */
void iron_panic_arena_stale(const char *deref_file,
                            int deref_line,
                            const struct IronArenaAllocHdr *hdr) {
    /* Phase 24 DROP-04/05: init-time cleanup + drop-time abort divert */
    if (iron_init_cleanup_top) iron_init_cleanup_run_and_clear();
    if (iron_in_destructor) {
        iron_panic_destructor_aborted(iron_current_dropping_type, __FILE__, __LINE__);
        /* noreturn — abort() inside */
    }
    const char *df = deref_file ? deref_file : "<unknown>";

    if (s_iron_panic_format == 1) {
        /* JSON line — distinct "panic":"arena_pointer" tag. */
        fputs("{\"panic\":\"arena_pointer\",", stderr);
        fprintf(stderr, "\"deref_site\":{\"file\":\"%s\",\"line\":%d}",
                df, deref_line);
        if (hdr) {
            fprintf(stderr, ",\"allocation\":{\"size\":%llu}",
                    (unsigned long long)hdr->size);
        } else {
            fputs(",\"allocation\":null", stderr);
        }
        fputc('}', stderr);
        fputc('\n', stderr);
    } else {
        /* Text format — distinct header substring. */
        fputs("iron: stale arena pointer dereference\n", stderr);
        fprintf(stderr, "  deref site: %s:%d\n", df, deref_line);
        if (hdr) {
            fprintf(stderr, "  allocation: size=%llu\n",
                    (unsigned long long)hdr->size);
        }
    }
    fflush(stderr);
    abort();
}

/* Phase 28 ARENA-10 (Plan 28-02): arena out-of-memory panic helper.
 *
 * Fired by iron_arena_rt_alloc / iron_arena_rt_new when an allocation would exceed
 * the arena's fixed capacity. The bump-pointer contract never returns null —
 * this is the deterministic abort path. Message carries the arena name, the
 * requested size, and the arena capacity (ARENA-10). Mirrors
 * iron_panic_stale_pointer's channels + divert. noreturn (abort). */
void iron_panic_arena_oom(const char *arena_name,
                          uint64_t requested_size,
                          uint64_t capacity) {
    /* Phase 24 DROP-04/05: init-time cleanup + drop-time abort divert */
    if (iron_init_cleanup_top) iron_init_cleanup_run_and_clear();
    if (iron_in_destructor) {
        iron_panic_destructor_aborted(iron_current_dropping_type, __FILE__, __LINE__);
        /* noreturn — abort() inside */
    }
    const char *an = arena_name ? arena_name : "<unnamed>";

    if (s_iron_panic_format == 1) {
        /* JSON line — distinct "panic":"arena_oom" tag. */
        fputs("{\"panic\":\"arena_oom\",", stderr);
        fprintf(stderr, "\"arena\":\"%s\"", an);
        fprintf(stderr, ",\"requested\":%llu,\"capacity\":%llu",
                (unsigned long long)requested_size,
                (unsigned long long)capacity);
        fputc('}', stderr);
        fputc('\n', stderr);
    } else {
        /* Text format. */
        fputs("iron: arena out of memory\n", stderr);
        fprintf(stderr, "  arena: %s\n", an);
        fprintf(stderr, "  requested: %llu bytes (capacity: %llu bytes)\n",
                (unsigned long long)requested_size,
                (unsigned long long)capacity);
    }
    fflush(stderr);
    abort();
}

/* Phase 31 DBG-04 (Plan 31-01): double-free panic — reports BOTH free-sites.
 *
 * Fired by iron_heap_free_dbg when the gen-mismatch branch detects a second
 * free of an already-freed allocation. The header still holds the FIRST
 * free-site (recorded on the first free before the gen bump); the SECOND
 * free-site is the caller's current __FILE__:__LINE__. Both — plus the
 * alloc-site (debug header) — are reported so the diagnostic is actionable.
 *
 * Mirrors iron_panic_stale_pointer's dual text/JSON channels, in-destructor
 * divert, init-cleanup pump, and panic-during-panic discipline (no malloc,
 * fputs/fprintf with stack buffers only; __FILE__ strings are static-storage
 * literals). The text channel emits two distinct "<file>:<line>" site
 * substrings so a double-free test can grep both. noreturn (abort). */
void iron_panic_double_free(const char *first_free_file,
                            int first_free_line,
                            const char *second_free_file,
                            int second_free_line,
                            const struct IronAllocHdr *hdr) {
    /* Phase 24 DROP-04/05: init-time cleanup + drop-time abort divert */
    if (iron_init_cleanup_top) iron_init_cleanup_run_and_clear();
    if (iron_in_destructor) {
        iron_panic_destructor_aborted(iron_current_dropping_type, __FILE__, __LINE__);
        /* noreturn — abort() inside */
    }
    const char *ff = first_free_file  ? first_free_file  : "<unknown>";
    const char *sf = second_free_file ? second_free_file : "<unknown>";

    if (s_iron_panic_format == 1) {
        /* JSON line — distinct "panic":"double_free" tag with both sites. */
        fputs("{\"panic\":\"double_free\",", stderr);
        fprintf(stderr, "\"first_free\":{\"file\":\"%s\",\"line\":%d}",
                ff, first_free_line);
        fprintf(stderr, ",\"second_free\":{\"file\":\"%s\",\"line\":%d}",
                sf, second_free_line);
#ifdef IRON_DEBUG_ALLOCATOR
        if (hdr) {
            const char *af = hdr->alloc_site_file ? hdr->alloc_site_file
                                                  : "<unknown>";
            fprintf(stderr, ",\"alloc_site\":{\"file\":\"%s\",\"line\":%u}",
                    af, (unsigned)hdr->alloc_site_line);
        } else {
            fputs(",\"alloc_site\":null", stderr);
        }
#else
        (void)hdr;
        fputs(",\"alloc_site\":null", stderr);
#endif
        fputc('}', stderr);
        fputc('\n', stderr);
    } else {
        /* Text format — two distinct site lines (grep-able by tests). */
        fputs("iron: double free detected\n", stderr);
        fprintf(stderr, "  first free site: %s:%d\n", ff, first_free_line);
        fprintf(stderr, "  second free site: %s:%d\n", sf, second_free_line);
#ifdef IRON_DEBUG_ALLOCATOR
        if (hdr) {
            const char *af = hdr->alloc_site_file ? hdr->alloc_site_file
                                                  : "<unknown>";
            fprintf(stderr, "  allocation site: %s:%u\n",
                    af, (unsigned)hdr->alloc_site_line);
        }
#else
        (void)hdr;
#endif
    }
    fflush(stderr);
    abort();
}

void iron_panic_stale_pointer(const char *deref_file,
                              int deref_line,
                              const struct IronAllocHdr *hdr) {
    /* Phase 24 DROP-04/05 (Plan 24-03): init-time cleanup + drop-time abort divert */
    if (iron_init_cleanup_top) iron_init_cleanup_run_and_clear();
    if (iron_in_destructor) {
        iron_panic_destructor_aborted(iron_current_dropping_type, __FILE__, __LINE__);
        /* noreturn — abort() inside */
    }
    const char *df = deref_file ? deref_file : "<unknown>";

    if (s_iron_panic_format == 1) {
        /* JSON line — alloc_site/allocation are null in release builds. */
        fputs("{\"kind\":\"stale_pointer\",", stderr);
        fprintf(stderr, "\"deref_site\":{\"file\":\"%s\",\"line\":%d}",
                df, deref_line);
#ifdef IRON_DEBUG_ALLOCATOR
        if (hdr) {
            const char *af = hdr->alloc_site_file ? hdr->alloc_site_file
                                                  : "<unknown>";
            fprintf(stderr, ",\"alloc_site\":{\"file\":\"%s\",\"line\":%u}",
                    af, (unsigned)hdr->alloc_site_line);
            fprintf(stderr, ",\"allocation\":{\"id\":%u,\"size\":%llu}",
                    (unsigned)hdr->alloc_id,
                    (unsigned long long)hdr->size);
        } else {
            fputs(",\"alloc_site\":null,\"allocation\":null", stderr);
        }
#else
        (void)hdr;
        fputs(",\"alloc_site\":null,\"allocation\":null", stderr);
#endif
        fputc('}', stderr);
        fputc('\n', stderr);
    } else {
        /* Text format — multi-line block, matches iron_oom_abort prefix style. */
        fputs("iron: stale pointer dereference\n", stderr);
        fprintf(stderr, "  deref site: %s:%d\n", df, deref_line);
#ifdef IRON_DEBUG_ALLOCATOR
        if (hdr) {
            const char *af = hdr->alloc_site_file ? hdr->alloc_site_file
                                                  : "<unknown>";
            fprintf(stderr, "  allocation site: %s:%u\n",
                    af, (unsigned)hdr->alloc_site_line);
            fprintf(stderr, "  allocation: id=%u size=%llu\n",
                    (unsigned)hdr->alloc_id,
                    (unsigned long long)hdr->size);
        }
#else
        (void)hdr;
#endif
    }
    fflush(stderr);
    abort();
}
