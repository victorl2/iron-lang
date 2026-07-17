/* iron_leakcheck.c — Phase 31 Plan 31-03 (DBG-07): release-build opt-in leak
 * check.
 *
 * The release 16B IronAllocHdr is ABI-frozen and has NO room for the intrusive
 * registry links the debug allocator threads through its 64B header (see
 * docs/dev/POINTER-LAYOUT.md). DBG-07 therefore tracks live allocations in a
 * SEPARATE side-table — a hand-rolled malloc'd singly-linked node list keyed by
 * the user pointer — under a single mutex, armed ONLY when the environment
 * variable IRON_LEAK_CHECK=1 is set (read ONCE at iron_runtime_init via
 * iron_leakcheck_init_from_env, mirroring iron_panic_init_from_env's getenv-once
 * discipline at src/runtime/iron_panic.c:47).
 *
 * Zero-cost-when-off discipline (Pitfall 6): every hook begins with
 *   if (!s_leak_check_enabled) return;
 * so an env-UNSET release build pays exactly one predictable branch per
 * alloc/free and never touches the mutex, never mallocs a node, never poisons.
 * The env-unset release binary is therefore byte-for-byte behaviourally
 * unchanged from the pre-Phase-31 release allocator.
 *
 * NO poison in release (CONTEXT GA3 / Anti-Pattern): DBG-07 enables only the
 * side-table registry + atexit dump. The 0xDD poison-on-free is debug-only.
 *
 * Scope note (mirrors the debug registry's intended false-negative): only
 * iron_heap_alloc'd blocks are tracked. rc (iron_rc.c) and arena
 * (iron_arena_rt.c) allocate their own blocks WITHOUT routing through
 * iron_heap_alloc, so they are intentionally absent — documented in the dump
 * header line and in docs/dev/POINTER-LAYOUT.md, not a bug.
 *
 * Output goes to STDERR + fflush (Pitfall 7): stdout must stay byte-identical
 * across build modes / env settings; only stderr diagnostics differ.
 */

#include "runtime/iron_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>   /* fprintf, fflush */
#include <stdlib.h>  /* getenv, malloc, free, atexit */

/* ── DBG-07 side-table state ──────────────────────────────────────────────── */

/* Hand-rolled side-table node, keyed by the returned user pointer. One malloc'd
 * node per live allocation while the check is armed. A singly-linked list is
 * sufficient: unregister is O(n) but the whole feature is opt-in and explicitly
 * "slower when the env var is set" per the research (Open Question 3 / Pitfall
 * 6). Not stb_ds — keeps the off-path early-return trivial and avoids pulling
 * map semantics into the allocator. */
typedef struct LcNode {
    void           *user_ptr;    /* key: the pointer iron_heap_alloc returned */
    const char     *site_file;   /* alloc-site (string literal; no strdup) */
    int             site_line;
    uint64_t        size;
    struct LcNode  *next;
} LcNode;

/* Armed flag: read ONCE in iron_leakcheck_init_from_env, never per-alloc. When
 * false (the default), every hook early-returns before any side-table work. */
static bool         s_leak_check_enabled = false;
static iron_mutex_t s_lc_lock;
static bool         s_lc_inited = false;   /* lock-initialized guard */
static LcNode      *s_lc_head  = NULL;

/* iron_leakcheck_init_from_env — DBG-07 arming. Idempotent across repeated
 * iron_runtime_init calls (unit-test harness pattern). Reads IRON_LEAK_CHECK
 * exactly once per process; when "1", initializes the lock and registers the
 * atexit dump. When unset / not "1", does NOTHING (no lock, no atexit) so the
 * common case pays zero teardown cost.
 *
 * Called only from the RELEASE (#else) branch of iron_runtime_init
 * (src/runtime/iron_string.c); in a debug build the in-header registry from
 * Plan 31-01 is the active mechanism and this is never invoked, so the two
 * trackers never double-count. */
void iron_leakcheck_init_from_env(void) {
    if (s_lc_inited) return;  /* already armed once this process */

    const char *env = getenv("IRON_LEAK_CHECK");
    if (env && env[0] == '1' && env[1] == '\0') {
        IRON_MUTEX_INIT(s_lc_lock);
        s_lc_inited = true;
        s_leak_check_enabled = true;
        atexit(iron_leakcheck_dump);
    } else {
        /* Explicit reset so a repeated init in a test harness cannot latch a
         * stale enabled state from a previous run. No lock, no atexit. */
        s_leak_check_enabled = false;
    }
}

/* iron_leakcheck_register — DBG-07 alloc hook. Hot-path early-return when the
 * check is disarmed (the common case). When armed, malloc a node keyed by the
 * user pointer and push it at the head under the lock. */
void iron_leakcheck_register(void *user_ptr, const char *site_file,
                             int site_line, uint64_t size) {
    if (!s_leak_check_enabled) return;   /* zero-cost off-path */
    if (!user_ptr) return;

    LcNode *node = (LcNode *)malloc(sizeof(LcNode));
    if (!node) return;   /* best-effort: a failed track is a silent miss, not a
                          * crash — the leak check must never destabilize the
                          * program it is observing. */
    node->user_ptr  = user_ptr;
    node->site_file = site_file;
    node->site_line = site_line;
    node->size      = size;

    IRON_MUTEX_LOCK(s_lc_lock);
    node->next = s_lc_head;
    s_lc_head  = node;
    IRON_MUTEX_UNLOCK(s_lc_lock);
}

/* iron_leakcheck_unregister — DBG-07 free hook. Hot-path early-return when
 * disarmed. When armed, find the node by user pointer, unlink, and free it. */
void iron_leakcheck_unregister(void *user_ptr) {
    if (!s_leak_check_enabled) return;   /* zero-cost off-path */
    if (!user_ptr) return;

    IRON_MUTEX_LOCK(s_lc_lock);
    LcNode **link = &s_lc_head;
    while (*link) {
        if ((*link)->user_ptr == user_ptr) {
            LcNode *dead = *link;
            *link = dead->next;
            free(dead);
            break;
        }
        link = &(*link)->next;
    }
    IRON_MUTEX_UNLOCK(s_lc_lock);
}

/* iron_leakcheck_dump — DBG-07 atexit handler. Registered ONLY when armed.
 * Walks the side-table under lock and reports each still-live allocation to
 * STDERR with its alloc-site provenance, then fflush. Empty list → prints
 * NOTHING (clean exit). Mirrors the debug iron_leak_dump output shape. */
void iron_leakcheck_dump(void) {
    if (!s_leak_check_enabled) return;

    IRON_MUTEX_LOCK(s_lc_lock);
    if (s_lc_head == NULL) {
        IRON_MUTEX_UNLOCK(s_lc_lock);
        return;   /* clean exit — no leaks */
    }
    uint64_t count = 0;
    for (LcNode *n = s_lc_head; n; n = n->next) count++;
    fprintf(stderr,
            "iron: %llu heap allocation(s) leaked at exit "
            "[IRON_LEAK_CHECK] (heap allocations only; rc/arena not tracked)\n",
            (unsigned long long)count);
    for (LcNode *n = s_lc_head; n; n = n->next) {
        const char *af = n->site_file ? n->site_file : "<unknown>";
        fprintf(stderr, "  leaked: %s:%d  (size=%llu)\n",
                af, n->site_line, (unsigned long long)n->size);
    }
    IRON_MUTEX_UNLOCK(s_lc_lock);
    fflush(stderr);
}
