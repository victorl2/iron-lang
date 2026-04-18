/* Phase 2 Plan 05 Task 02 (CORE-16, CORE-17) -- Diagnostic translation.
 *
 * Walks the Iron_DiagList once, building a yyjson array of LSP
 * Diagnostic objects. Each Iron_Diagnostic is translated via:
 *   - span:    ilsp_span_to_lsp_range
 *   - severity: IRON_DIAG_* -> 1/2/3
 *   - code:    "E%04d" from Iron_Diagnostic.code
 *   - source:  "iron"
 *   - message: d->message
 *   - suggestion (if present): relatedInformation[0] with same range
 *
 * Callers wrap the returned doc:
 *   - publishDiagnostics: method + params.uri + params.version
 *   - pull-diagnostic report: id + result.{kind,resultId,items}
 *
 * Both builders route yyjson allocations through ilsp_json_alc so the
 * body bytes live in the caller's arena. */

#include "lsp/facade/diagnostics.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"
#include "lsp/transport/json.h"
#include "vendor/yyjson/yyjson.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>

/* Attach an LSP Range object at `parent[key]`. */
static void attach_range(yyjson_mut_doc *d, yyjson_mut_val *parent,
                          const char *key, IronLsp_Range r) {
    yyjson_mut_val *range = yyjson_mut_obj(d);
    yyjson_mut_val *start = yyjson_mut_obj(d);
    yyjson_mut_val *end   = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, start, "line",      r.start.line);
    yyjson_mut_obj_add_uint(d, start, "character", r.start.character);
    yyjson_mut_obj_add_uint(d, end,   "line",      r.end.line);
    yyjson_mut_obj_add_uint(d, end,   "character", r.end.character);
    yyjson_mut_obj_add_val (d, range, "start", start);
    yyjson_mut_obj_add_val (d, range, "end",   end);
    yyjson_mut_obj_add_val (d, parent, key, range);
}

/* Build the diagnostics[] array. Caller owns the returned value. */
static yyjson_mut_val *build_diagnostics_array(yyjson_mut_doc           *d,
                                                 const Iron_DiagList      *diags,
                                                 const IronLsp_Document   *doc,
                                                 IronLsp_PositionEncoding  enc) {
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    if (!diags || !diags->items) return arr;

    int n = (int)arrlen(diags->items);
    for (int i = 0; i < n; i++) {
        const Iron_Diagnostic *dg = &diags->items[i];
        yyjson_mut_val *obj = yyjson_mut_obj(d);

        /* range */
        IronLsp_Range range = ilsp_span_to_lsp_range(dg->span, doc, enc);
        attach_range(d, obj, "range", range);

        /* severity */
        int severity;
        switch (dg->level) {
            case IRON_DIAG_ERROR:   severity = 1; break;
            case IRON_DIAG_WARNING: severity = 2; break;
            case IRON_DIAG_NOTE:    severity = 3; break;
            default:                severity = 1; break;
        }
        yyjson_mut_obj_add_int(d, obj, "severity", severity);

        /* code */
        char codebuf[16];
        snprintf(codebuf, sizeof(codebuf), "E%04d", dg->code);
        yyjson_mut_obj_add_strcpy(d, obj, "code", codebuf);

        /* source */
        yyjson_mut_obj_add_strcpy(d, obj, "source", "iron");

        /* message */
        yyjson_mut_obj_add_strcpy(d, obj, "message",
                                    dg->message ? dg->message : "");

        /* suggestion -> relatedInformation */
        if (dg->suggestion && dg->suggestion[0] != '\0') {
            yyjson_mut_val *rel_arr = yyjson_mut_arr(d);
            yyjson_mut_val *rel_obj = yyjson_mut_obj(d);
            /* location: { uri, range } */
            yyjson_mut_val *loc = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, loc, "uri",
                                        doc && doc->uri ? doc->uri : "");
            attach_range(d, loc, "range", range);
            yyjson_mut_obj_add_val(d, rel_obj, "location", loc);
            /* message */
            char sugg_msg[512];
            snprintf(sugg_msg, sizeof(sugg_msg),
                     "suggestion: %s", dg->suggestion);
            yyjson_mut_obj_add_strcpy(d, rel_obj, "message", sugg_msg);
            yyjson_mut_arr_append(rel_arr, rel_obj);
            yyjson_mut_obj_add_val(d, obj, "relatedInformation", rel_arr);
        }

        yyjson_mut_arr_append(arr, obj);
    }
    return arr;
}

yyjson_mut_doc *ilsp_build_publish_diagnostics(const Iron_DiagList           *diags,
                                                const IronLsp_Document        *doc,
                                                IronLsp_PositionEncoding       enc,
                                                Iron_Arena                    *arena) {
    yyjson_alc      alc = ilsp_json_alc(arena);
    yyjson_mut_doc *d   = yyjson_mut_doc_new(&alc);
    if (!d) return NULL;

    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_strcpy(d, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_strcpy(d, root, "method",
                                "textDocument/publishDiagnostics");

    yyjson_mut_val *params = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, params, "uri",
                                doc && doc->uri ? doc->uri : "");
    if (doc) {
        yyjson_mut_obj_add_int(d, params, "version", doc->version);
    }
    yyjson_mut_val *arr = build_diagnostics_array(d, diags, doc, enc);
    yyjson_mut_obj_add_val(d, params, "diagnostics", arr);

    yyjson_mut_obj_add_val(d, root, "params", params);
    return d;
}

yyjson_mut_doc *ilsp_build_pull_diagnostic_report(const Iron_DiagList           *diags,
                                                   const IronLsp_Document        *doc,
                                                   IronLsp_PositionEncoding       enc,
                                                   const char                    *request_id,
                                                   Iron_Arena                    *arena) {
    yyjson_alc      alc = ilsp_json_alc(arena);
    yyjson_mut_doc *d   = yyjson_mut_doc_new(&alc);
    if (!d) return NULL;

    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_strcpy(d, root, "jsonrpc", "2.0");

    /* id: if the request_id starts with a digit and parses as an integer
     * we emit it as an int (JSON-RPC prefers the same shape the client
     * sent); otherwise emit it as a string. The request_id from the
     * dispatcher is already a stringified form of either. */
    if (request_id && request_id[0] != '\0') {
        bool all_digits = true;
        for (const char *p = request_id; *p; p++) {
            if (*p < '0' || *p > '9') { all_digits = false; break; }
        }
        if (all_digits) {
            long long v = 0;
            for (const char *p = request_id; *p; p++) {
                v = v * 10 + (*p - '0');
            }
            yyjson_mut_obj_add_sint(d, root, "id", (int64_t)v);
        } else {
            yyjson_mut_obj_add_strcpy(d, root, "id", request_id);
        }
    } else {
        yyjson_mut_obj_add_null(d, root, "id");
    }

    yyjson_mut_val *result = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, result, "kind", "full");

    /* resultId: "v<version>" so clients can correlate the snapshot. */
    char resultid[32];
    if (doc) {
        snprintf(resultid, sizeof(resultid), "v%d", doc->version);
    } else {
        snprintf(resultid, sizeof(resultid), "v0");
    }
    yyjson_mut_obj_add_strcpy(d, result, "resultId", resultid);

    yyjson_mut_val *arr = build_diagnostics_array(d, diags, doc, enc);
    yyjson_mut_obj_add_val(d, result, "items", arr);

    yyjson_mut_obj_add_val(d, root, "result", result);
    return d;
}
