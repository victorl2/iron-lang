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
#include "runtime/iron_panic.h"
#include "diagnostics/diagnostics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cached at iron_runtime_init time. 0 = text (default), 1 = json.
 * File-scope static; never re-read on the panic path. */
static int s_iron_panic_format = 0;

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

void iron_panic_stale_pointer(const char *deref_file,
                              int deref_line,
                              const struct IronAllocHdr *hdr) {
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
