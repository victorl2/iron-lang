/* Phase 2 Plan 03 Task 02 -- client/registerCapability outbound builder.
 *
 * Post-`initialized` the server sends a `client/registerCapability`
 * request to register interest in `workspace/didChangeWatchedFiles` for
 * Iron source files and package manifests:
 *   - "**" slash "*.iron"     (source files)
 *   - "**" slash "iron.toml"  (package manifests)
 *   - "**" slash "iron.lock"  (lockfiles)
 *
 * Watch kind = 7 (Create | Change | Delete; FileSystemWatcher.WatchKind
 * bitmask per LSP 3.17 spec).
 *
 * The dyn_register "subsystem" is a 1-field struct that owns the
 * registration bookkeeping (currently just the last-used registration id
 * string). Storage discipline: allocated in main.c via
 * ilsp_dyn_register_create, destroyed via ilsp_dyn_register_destroy. */
#include "lsp/server/dyn_register.h"
#include "lsp/server/server.h"
#include "lsp/transport/json.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/types.h"
#include "vendor/yyjson/yyjson.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Placeholder struct body -- Plan 06 may flesh this out. For now it is a
 * sentinel so the IronLsp_Server.dyn_reg pointer can be non-NULL (the
 * `initialized` handler gates on that pointer). */
struct IronLsp_DynRegister {
    int  _placeholder;
};

/* Internal constructors (called from main.c once the server is wired up). */
IronLsp_DynRegister *ilsp_dyn_register_create(void);
void                 ilsp_dyn_register_destroy(IronLsp_DynRegister *r);

IronLsp_DynRegister *ilsp_dyn_register_create(void) {
    IronLsp_DynRegister *r = (IronLsp_DynRegister *)calloc(1, sizeof(*r));
    return r;
}

void ilsp_dyn_register_destroy(IronLsp_DynRegister *r) {
    free(r);
}

/* Build the watchers array for the registerOptions payload. */
static yyjson_mut_val *build_watchers(yyjson_mut_doc *d) {
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    /* kind 7 = Create | Change | Delete. */
    const char *globs[] = { "**/*.iron", "**/iron.toml", "**/iron.lock" };
    for (size_t i = 0; i < sizeof(globs) / sizeof(globs[0]); i++) {
        yyjson_mut_val *w = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, w, "globPattern", globs[i]);
        yyjson_mut_obj_add_int   (d, w, "kind", 7);
        yyjson_mut_arr_append(arr, w);
    }
    return arr;
}

void ilsp_dyn_register_watched_files(IronLsp_Server *server) {
    if (!server || !server->writer) return;

    /* Build the request in a fresh arena (short-lived; freed once the
     * body is copied to the writer's malloc'd buffer). */
    Iron_Arena arena = iron_arena_create(4 * 1024);
    yyjson_alc alc = ilsp_json_alc(&arena);
    yyjson_mut_doc *d = yyjson_mut_doc_new(&alc);
    if (!d) { iron_arena_free(&arena); return; }

    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);

    /* Server-originated request id: atomic counter on the server. */
    uint64_t req_id = atomic_fetch_add(&server->next_request_id, 1);

    yyjson_mut_obj_add_strcpy(d, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_uint  (d, root, "id",      req_id);
    yyjson_mut_obj_add_strcpy(d, root, "method",  "client/registerCapability");

    yyjson_mut_val *params = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, root, "params", params);

    yyjson_mut_val *regs = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, params, "registrations", regs);

    yyjson_mut_val *reg = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, reg, "id",     "ironls-watch-1");
    yyjson_mut_obj_add_strcpy(d, reg, "method", "workspace/didChangeWatchedFiles");

    yyjson_mut_val *ropts = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, ropts, "watchers", build_watchers(d));
    yyjson_mut_obj_add_val(d, reg, "registerOptions", ropts);

    yyjson_mut_arr_append(regs, reg);

    size_t len = 0;
    char *body = ilsp_json_write_mut(d, &arena, &len);
    if (!body || len == 0) { iron_arena_free(&arena); return; }

    char *heap_body = (char *)malloc(len);
    if (!heap_body) { iron_arena_free(&arena); return; }
    memcpy(heap_body, body, len);

    /* Request-shaped outbound -- treat as RESPONSE priority so it can't
     * be dropped under back-pressure (it IS a request, just server-
     * originated; the client will reply to the id). */
    ilsp_writer_enqueue(server->writer, ILSP_PRIO_RESPONSE, heap_body, len);

    iron_arena_free(&arena);
}
