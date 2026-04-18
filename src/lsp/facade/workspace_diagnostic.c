/* Phase 3 Plan 06 Task 01 (NAV-12, D-12) -- workspace/diagnostic facade.
 *
 * Pull-aggregates per-file diagnostics across the workspace. On cache
 * hit returns kind="unchanged"; on miss runs facade analyze via
 * ilsp_facade_compile_for_nav (the single iron_analyze_buffer call
 * site is preserved via the shared private static facade_analyze in
 * compile.c -- CORE-22 stays at 1 hit).
 *
 * Per D-12: 2000 ms wall-clock budget; files not yet analyzed at the
 * deadline return kind="unchanged" with the previous resultId so the
 * client re-requests on the next workspace/diagnostic/refresh push.
 *
 * Cache: keyed on canonical_path; each slot holds content_hash +
 * arena-owned Iron_DiagList + resultId. LRU eviction at 200 entries.
 *
 * Note: the cache intentionally stores Iron_DiagList in a dedicated
 * per-entry arena. When the underlying workspace entry's content_hash
 * changes we evict (via ilsp_ws_diag_cache_evict_path on invalidate)
 * OR detect the mismatch here and rebuild.
 */

#include "lsp/facade/workspace_diagnostic.h"

#include "lsp/facade/compile.h"
#include "lsp/facade/diagnostics.h"
#include "lsp/facade/span.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/types.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/workspace_index.h"
#include "lsp/transport/json.h"
#include "diagnostics/diagnostics.h"
#include "runtime/iron_runtime.h"  /* iron_mutex_t */
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Cache types ──────────────────────────────────────────────────── */

/* One cache slot -- canonical_path keys into the stb_ds shmap. We keep
 * a freshly malloc'd Iron_Arena in each slot so the cached Iron_DiagList
 * remains valid independent of the workspace entry's own arena (which
 * may get replaced when analyze_lazy re-runs). */
typedef struct IronLsp_WsDiagCacheSlot {
    char          *canonical_path;   /* malloc'd; matches stb_ds key */
    uint64_t       content_hash;     /* must match index entry to stay fresh */
    char          *result_id;        /* "w<ver>-<hash8hex>", malloc'd */
    Iron_Arena    *arena;            /* heap-alloc; owns cached_diags arena slabs */
    Iron_DiagList  cached_diags;     /* cached DiagList; items live in `arena` */
    int64_t        last_used_tick;   /* LRU stamp */
} IronLsp_WsDiagCacheSlot;

struct IronLsp_WsDiagCache {
    /* stb_ds sh_new_strdup map: key = canonical_path, value = slot ptr. */
    struct { char *key; IronLsp_WsDiagCacheSlot *value; } *slots;
    _Atomic int_least64_t   tick;
    _Atomic uint_least64_t  workspace_version;
    iron_mutex_t            lock;
};

/* ── Private helpers ──────────────────────────────────────────────── */

static void free_slot(IronLsp_WsDiagCacheSlot *slot) {
    if (!slot) return;
    if (slot->arena) {
        iron_arena_free(slot->arena);
        free(slot->arena);
    }
    if (slot->canonical_path) free(slot->canonical_path);
    if (slot->result_id)      free(slot->result_id);
    iron_diaglist_free(&slot->cached_diags);
    free(slot);
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static void format_result_id(char out[32],
                              uint64_t ws_version,
                              uint64_t content_hash) {
    /* "w<ver>-<hash8hex>" -- the hash prefix is 8 hex chars so the
     * resultId stays compact (14-22 chars typical). */
    snprintf(out, 32, "w%llu-%08llx",
              (unsigned long long)ws_version,
              (unsigned long long)(content_hash & 0xffffffffULL));
}

/* Clone an Iron_DiagList into a fresh arena-owned copy. Used when we
 * want to cache diagnostics produced by the facade (whose arena is
 * short-lived) into the per-slot arena (long-lived). */
static void clone_diaglist(Iron_Arena *dst_arena,
                            Iron_DiagList *dst,
                            const Iron_DiagList *src) {
    *dst = iron_diaglist_create();
    if (!src || !src->items) return;
    int n = (int)arrlen(src->items);
    for (int i = 0; i < n; i++) {
        const Iron_Diagnostic *s = &src->items[i];
        /* Intern message + suggestion + filename strings into dst_arena
         * so they outlive the source arena. */
        char *msg = s->message
            ? iron_arena_strdup(dst_arena, s->message, strlen(s->message))
            : NULL;
        char *sug = (s->suggestion && s->suggestion[0])
            ? iron_arena_strdup(dst_arena, s->suggestion, strlen(s->suggestion))
            : NULL;
        Iron_Span span = s->span;
        if (span.filename) {
            span.filename = iron_arena_strdup(dst_arena, span.filename,
                                                strlen(span.filename));
        }
        Iron_Diagnostic cloned = {
            .level = s->level,
            .code  = s->code,
            .span  = span,
            .message    = msg,
            .suggestion = sug,
        };
        arrput(dst->items, cloned);
    }
}

/* Evict the slot with the lowest last_used_tick. Caller holds lock. */
static void evict_lru_locked(IronLsp_WsDiagCache *c) {
    if (!c || !c->slots) return;
    ptrdiff_t n = shlen(c->slots);
    if (n <= 0) return;
    ptrdiff_t victim_idx = -1;
    int64_t   victim_tick = INT64_MAX;
    for (ptrdiff_t i = 0; i < n; i++) {
        IronLsp_WsDiagCacheSlot *s = c->slots[i].value;
        if (!s) continue;
        if (s->last_used_tick < victim_tick) {
            victim_tick = s->last_used_tick;
            victim_idx  = i;
        }
    }
    if (victim_idx < 0) return;
    IronLsp_WsDiagCacheSlot *s = c->slots[victim_idx].value;
    char *key_copy = s && s->canonical_path ? strdup(s->canonical_path) : NULL;
    free_slot(s);
    if (key_copy) {
        shdel(c->slots, key_copy);
        free(key_copy);
    }
}

/* ── Cache public API ─────────────────────────────────────────────── */

IronLsp_WsDiagCache *ilsp_ws_diag_cache_create(void) {
    IronLsp_WsDiagCache *c = (IronLsp_WsDiagCache *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->slots = NULL;
    sh_new_strdup(c->slots);
    atomic_store(&c->tick, 0);
    atomic_store(&c->workspace_version, 1);
    IRON_MUTEX_INIT(c->lock);
    return c;
}

void ilsp_ws_diag_cache_destroy(IronLsp_WsDiagCache *c) {
    if (!c) return;
    IRON_MUTEX_LOCK(c->lock);
    if (c->slots) {
        for (ptrdiff_t i = 0; i < shlen(c->slots); i++) {
            free_slot(c->slots[i].value);
        }
        shfree(c->slots);
    }
    IRON_MUTEX_UNLOCK(c->lock);
    IRON_MUTEX_DESTROY(c->lock);
    free(c);
}

void ilsp_ws_diag_cache_evict_path(IronLsp_WsDiagCache *c,
                                    const char *canonical_path) {
    if (!c || !canonical_path || !c->slots) return;
    IRON_MUTEX_LOCK(c->lock);
    ptrdiff_t idx = shgeti(c->slots, canonical_path);
    if (idx >= 0) {
        IronLsp_WsDiagCacheSlot *s = c->slots[idx].value;
        free_slot(s);
        shdel(c->slots, canonical_path);
        atomic_fetch_add(&c->workspace_version, 1);
    }
    IRON_MUTEX_UNLOCK(c->lock);
}

size_t ilsp_ws_diag_cache_size(const IronLsp_WsDiagCache *c) {
    if (!c || !c->slots) return 0;
    /* shlen is a macro; cast away const -- the caller already promises
     * not to mutate, and shlen only reads the header. */
    return (size_t)shlen(((IronLsp_WsDiagCache *)c)->slots);
}

uint64_t ilsp_ws_diag_cache_bump_version(IronLsp_WsDiagCache *c) {
    if (!c) return 0;
    return (uint64_t)atomic_fetch_add(&c->workspace_version, 1) + 1;
}

/* ── JSON diagnostic-array builder (reused from facade/diagnostics.c) ─
 * We re-implement a stripped version here because the Plan 02 builder
 * requires an IronLsp_Document (full doc struct) but workspace entries
 * only expose line_idx + source bytes. */

static void attach_range_obj(yyjson_mut_doc *d, yyjson_mut_val *parent,
                               const char *key, IronLsp_Range r) {
    yyjson_mut_val *range = yyjson_mut_obj(d);
    yyjson_mut_val *start = yyjson_mut_obj(d);
    yyjson_mut_val *end   = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, start, "line",      r.start.line);
    yyjson_mut_obj_add_uint(d, start, "character", r.start.character);
    yyjson_mut_obj_add_uint(d, end,   "line",      r.end.line);
    yyjson_mut_obj_add_uint(d, end,   "character", r.end.character);
    yyjson_mut_obj_add_val(d, range, "start", start);
    yyjson_mut_obj_add_val(d, range, "end",   end);
    yyjson_mut_obj_add_val(d, parent, key, range);
}

/* Build a JSON Diagnostic[] array against a workspace entry's source +
 * line index. Entries need NOT have the IronLsp_Document shape. */
static yyjson_mut_val *build_items_array_for_entry(
    yyjson_mut_doc               *d,
    const Iron_DiagList          *diags,
    const IronLsp_IndexEntry     *entry,
    IronLsp_PositionEncoding      enc) {
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    if (!diags || !diags->items || !entry) return arr;
    int n = (int)arrlen(diags->items);
    for (int i = 0; i < n; i++) {
        const Iron_Diagnostic *dg = &diags->items[i];
        yyjson_mut_val *obj = yyjson_mut_obj(d);
        IronLsp_Range r = ilsp_nav_entry_span_to_range(entry, dg->span, enc);
        attach_range_obj(d, obj, "range", r);
        int severity;
        switch (dg->level) {
            case IRON_DIAG_ERROR:   severity = 1; break;
            case IRON_DIAG_WARNING: severity = 2; break;
            case IRON_DIAG_NOTE:    severity = 3; break;
            default:                severity = 1; break;
        }
        yyjson_mut_obj_add_int(d, obj, "severity", severity);
        char codebuf[16];
        snprintf(codebuf, sizeof(codebuf), "E%04d", dg->code);
        yyjson_mut_obj_add_strcpy(d, obj, "code", codebuf);
        yyjson_mut_obj_add_strcpy(d, obj, "source", "iron");
        yyjson_mut_obj_add_strcpy(d, obj, "message",
                                    dg->message ? dg->message : "");
        yyjson_mut_arr_append(arr, obj);
    }
    return arr;
}

/* ── Facade entry: workspace_diagnostic walker ───────────────────── */

void ilsp_facade_workspace_diagnostic(struct IronLsp_Server    *server,
                                        const char               *previous_result_id,
                                        _Atomic bool             *cancel,
                                        struct yyjson_mut_doc    *json_doc,
                                        Iron_Arena               *arena,
                                        IronLsp_WsDiagFileReport **out_reports,
                                        size_t                   *out_n) {
    (void)previous_result_id;  /* reserved; D-12 cache already handles staleness. */
    if (out_reports) *out_reports = NULL;
    if (out_n)       *out_n       = 0;
    if (!server || !arena || !json_doc || !out_reports || !out_n) return;

    IronLsp_WorkspaceIndex *wi = server->workspace_index;
    IronLsp_WsDiagCache    *cache = server->ws_diag_cache;
    /* Empty workspace or no cache: return an empty reports array. */
    if (!wi || !cache) return;

    int64_t start_ms = monotonic_ms();

    /* Snapshot the paths under wi->lock so we can release it during
     * analyze without re-entrant locking. */
    size_t n_paths = 0;
    char **paths = ilsp_workspace_index_snapshot_paths(wi, &n_paths);
    if (!paths || n_paths == 0) {
        if (paths) free(paths);
        return;
    }

    IronLsp_PositionEncoding enc = server->position_encoding;

    /* Pre-allocate the report array in `arena`. */
    IronLsp_WsDiagFileReport *reports = (IronLsp_WsDiagFileReport *)
        iron_arena_alloc(arena, n_paths * sizeof(IronLsp_WsDiagFileReport),
                          _Alignof(IronLsp_WsDiagFileReport));
    if (!reports) {
        for (size_t i = 0; i < n_paths; i++) free(paths[i]);
        free(paths);
        return;
    }
    memset(reports, 0, n_paths * sizeof(*reports));
    size_t filled = 0;

    uint64_t ws_version = (uint64_t)atomic_load(&cache->workspace_version);

    for (size_t i = 0; i < n_paths; i++) {
        /* D-16 cancel-at-iteration-boundary. */
        if (cancel && atomic_load(cancel)) break;
        if (!paths[i]) continue;

        /* Budget: stop at (BUDGET_MS - SERIALIZE_MS) so downstream
         * serialization still has headroom. */
        int64_t elapsed = monotonic_ms() - start_ms;
        bool   deadline_hit = (elapsed >=
            (ILSP_WS_DIAG_BUDGET_MS - ILSP_WS_DIAG_SERIALIZE_MS));

        IronLsp_IndexEntry *entry = ilsp_workspace_index_lookup(wi, paths[i]);
        if (!entry) continue;

        uint64_t content_hash = entry->content_hash;
        const char *canon     = entry->canonical_path
            ? entry->canonical_path : paths[i];
        const char *uri = ilsp_nav_path_to_uri(canon, arena);
        if (!uri) continue;

        /* ── Cache lookup ─── */
        char resultid_buf[32];
        format_result_id(resultid_buf, ws_version, content_hash);

        bool cache_hit = false;
        const Iron_DiagList *cached_diags_ptr = NULL;

        IRON_MUTEX_LOCK(cache->lock);
        ptrdiff_t slot_idx = shgeti(cache->slots, canon);
        if (slot_idx >= 0) {
            IronLsp_WsDiagCacheSlot *slot = cache->slots[slot_idx].value;
            if (slot && slot->content_hash == content_hash) {
                cache_hit = true;
                slot->last_used_tick = atomic_fetch_add(&cache->tick, 1);
                /* The cached resultId is stable for (path, hash). */
                snprintf(resultid_buf, sizeof(resultid_buf), "%s",
                          slot->result_id ? slot->result_id : resultid_buf);
                cached_diags_ptr = &slot->cached_diags;
            }
        }
        IRON_MUTEX_UNLOCK(cache->lock);

        /* Interned resultId string in the facade arena for the report. */
        const char *resultid_arena = iron_arena_strdup(
            arena, resultid_buf, strlen(resultid_buf));
        if (!resultid_arena) continue;

        IronLsp_WsDiagFileReport *rep = &reports[filled];
        rep->uri       = uri;
        rep->version   = -1;   /* non-open file; client knows the URI */
        rep->result_id = resultid_arena;

        if (cache_hit) {
            rep->kind       = "unchanged";
            rep->items_json = NULL;
            filled++;
            continue;
        }

        if (deadline_hit) {
            /* Over budget -- return unchanged with previousResultId if
             * available. The cache miss + deadline means we don't have
             * a cached resultId; fall back to the synthetic one so the
             * client can correlate later. */
            rep->kind       = "unchanged";
            rep->items_json = NULL;
            filled++;
            continue;
        }

        /* ── Cache miss: run facade analyze and cache the result ─── */
        /* Synthesize a minimal IronLsp_Document for the facade seam
         * (see workspace_index.c analyze_lazy -- same pattern). */
        IronLsp_Document doc = {0};
        doc.text     = entry->source_bytes;
        doc.text_len = entry->source_len;
        doc.uri      = entry->canonical_path;

        IronLsp_CompileRequest req = { .version = 0, .cancel_flag = cancel };

        Iron_Arena work_arena = iron_arena_create(64 * 1024);
        Iron_DiagList work_diags = iron_diaglist_create();
        (void)ilsp_facade_compile_for_nav(&doc, &req, &work_arena, &work_diags);

        /* Build the items[] array bound to the caller's yyjson_doc. */
        yyjson_mut_val *items = build_items_array_for_entry(
            json_doc, &work_diags, entry, enc);

        /* ── Populate cache (clone diag list into per-slot arena) ─── */
        IronLsp_WsDiagCacheSlot *new_slot = (IronLsp_WsDiagCacheSlot *)
            calloc(1, sizeof(*new_slot));
        if (new_slot) {
            Iron_Arena *slot_arena = (Iron_Arena *)malloc(sizeof(Iron_Arena));
            if (slot_arena) {
                *slot_arena = iron_arena_create(16 * 1024);
                new_slot->canonical_path = strdup(canon);
                new_slot->content_hash   = content_hash;
                new_slot->result_id      = strdup(resultid_buf);
                new_slot->arena          = slot_arena;
                new_slot->last_used_tick = atomic_fetch_add(&cache->tick, 1);
                clone_diaglist(slot_arena, &new_slot->cached_diags, &work_diags);

                IRON_MUTEX_LOCK(cache->lock);
                /* Replace existing slot if any. */
                ptrdiff_t existing = shgeti(cache->slots, canon);
                if (existing >= 0) {
                    free_slot(cache->slots[existing].value);
                    shdel(cache->slots, canon);
                }
                shput(cache->slots, canon, new_slot);
                /* LRU-cap enforcement. */
                while ((size_t)shlen(cache->slots) > ILSP_WS_DIAG_LRU_CAP) {
                    evict_lru_locked(cache);
                }
                IRON_MUTEX_UNLOCK(cache->lock);
                (void)cached_diags_ptr;
            } else {
                free(new_slot);
            }
        }

        /* Clean up facade's per-call scratch. */
        iron_diaglist_free(&work_diags);
        iron_arena_free(&work_arena);

        rep->kind       = "full";
        rep->items_json = items;
        filled++;
    }

    for (size_t i = 0; i < n_paths; i++) free(paths[i]);
    free(paths);

    *out_reports = reports;
    *out_n       = filled;
}
