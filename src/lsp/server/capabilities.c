/* Phase 2 Plan 03 Task 02 -- ServerCapabilities builder + positionEncoding
 * negotiation. Capability matrix is derived from ilsp_handler_table to
 * guarantee every advertised capability has a registered handler
 * (PITFALLS.md #15 mitigation; CORE-06). */
#include "lsp/server/capabilities.h"
#include "lsp/server/dispatch.h"

#include <stdbool.h>
#include <string.h>

const char *ilsp_position_encoding_str(IronLsp_PositionEncoding e) {
    switch (e) {
        case ILSP_ENC_UTF8:  return "utf-8";
        case ILSP_ENC_UTF16: return "utf-16";
    }
    return "utf-16";
}

IronLsp_PositionEncoding
ilsp_capabilities_negotiate_position_encoding(yyjson_val *client_caps) {
    /* LSP 3.17 §ClientCapabilities.general.positionEncodings.
     * Spec default when absent: "utf-16". */
    if (!client_caps || !yyjson_is_obj(client_caps)) return ILSP_ENC_UTF16;

    yyjson_val *general = yyjson_obj_get(client_caps, "general");
    if (!general || !yyjson_is_obj(general)) return ILSP_ENC_UTF16;

    yyjson_val *arr = yyjson_obj_get(general, "positionEncodings");
    if (!arr || !yyjson_is_arr(arr)) return ILSP_ENC_UTF16;

    /* First pass: prefer UTF-8. */
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(arr, idx, max, item) {
        const char *s = yyjson_get_str(item);
        if (s && strcmp(s, "utf-8") == 0) return ILSP_ENC_UTF8;
    }
    /* Second pass: accept UTF-16 explicitly. */
    yyjson_arr_foreach(arr, idx, max, item) {
        const char *s = yyjson_get_str(item);
        if (s && strcmp(s, "utf-16") == 0) return ILSP_ENC_UTF16;
    }
    /* Client offered only non-spec encodings (e.g. utf-32). Fall back to
     * the spec default. */
    return ILSP_ENC_UTF16;
}

/* Check whether a given capability string has already been added to the
 * capabilities object. A capability may be referenced by more than one
 * handler (e.g. textDocument/didOpen + textDocument/didChange both map
 * to "textDocumentSync"); guard against double-add. */
static bool caps_has(yyjson_mut_val *caps, const char *name) {
    return yyjson_mut_obj_get(caps, name) != NULL;
}

/* Attach the spec-shaped value for a given capability name. For now every
 * capability advertised by Plan 03 is lifecycle-only (no capability
 * strings yet), so this function only handles the shapes Plans 04/05/06
 * will need -- see comments inline for the mapping. */
static void caps_add(yyjson_mut_doc *d, yyjson_mut_val *caps,
                     const char *name) {
    if (!name || caps_has(caps, name)) return;

    /* textDocumentSync expects an object of shape { openClose: true,
     * change: 2 (Incremental), save: true }. */
    if (strcmp(name, "textDocumentSync") == 0) {
        yyjson_mut_val *sync = yyjson_mut_obj(d);
        yyjson_mut_obj_add_bool(d, sync, "openClose", true);
        yyjson_mut_obj_add_int (d, sync, "change",    2);
        yyjson_mut_obj_add_bool(d, sync, "save",      true);
        yyjson_mut_obj_add_val (d, caps, name, sync);
        return;
    }

    /* diagnosticProvider expects { interFileDependencies: bool, workspaceDiagnostics: bool }.
     * Plan 06 (NAV-12, NAV-13, D-12): workspaceDiagnostics flipped from
     * false to true; the workspace/diagnostic pull handler is registered
     * in dispatch.c and the workspace/diagnostic/refresh notification is
     * emitted on non-open-file invalidation in handlers_document.c. */
    if (strcmp(name, "diagnosticProvider") == 0) {
        yyjson_mut_val *dp = yyjson_mut_obj(d);
        yyjson_mut_obj_add_bool(d, dp, "interFileDependencies", false);
        yyjson_mut_obj_add_bool(d, dp, "workspaceDiagnostics",  true);
        yyjson_mut_obj_add_val (d, caps, name, dp);
        return;
    }

    /* Phase 4 Plan 04-02 (D-01 + D-04 + D-16): completionProvider expects
     *   { resolveProvider: true, triggerCharacters: [".", ":", "/"] }.
     * The trigger-char list is locked by D-16; "/" catches import-path
     * continuation. resolveProvider=true is required for EDIT-03 lazy
     * markdown fill. */
    if (strcmp(name, "completionProvider") == 0) {
        yyjson_mut_val *cp = yyjson_mut_obj(d);
        yyjson_mut_obj_add_bool(d, cp, "resolveProvider", true);
        yyjson_mut_val *trig = yyjson_mut_arr(d);
        yyjson_mut_arr_add_strcpy(d, trig, ".");
        yyjson_mut_arr_add_strcpy(d, trig, ":");
        yyjson_mut_arr_add_strcpy(d, trig, "/");
        yyjson_mut_obj_add_val(d, cp, "triggerCharacters", trig);
        yyjson_mut_obj_add_val(d, caps, name, cp);
        return;
    }

    /* Phase 4 Plan 04-04 (D-07): codeActionProvider expects
     *   { resolveProvider: true,
     *     codeActionKinds: ["quickfix", "source.organizeImports"] }.
     * resolveProvider=true gates the lazy codeAction/resolve flow
     * (EDIT-08). codeActionKinds advertises both "quickfix" (this plan)
     * and "source.organizeImports" (forward-advertised for Plan 04-05;
     * shipping both here lets clients light up the Source Actions menu
     * without a capability bump when Plan 04-05 lands). */
    if (strcmp(name, "codeActionProvider") == 0) {
        yyjson_mut_val *cap = yyjson_mut_obj(d);
        yyjson_mut_obj_add_bool(d, cap, "resolveProvider", true);
        yyjson_mut_val *kinds = yyjson_mut_arr(d);
        yyjson_mut_arr_add_strcpy(d, kinds, "quickfix");
        yyjson_mut_arr_add_strcpy(d, kinds, "source.organizeImports");
        yyjson_mut_obj_add_val(d, cap, "codeActionKinds", kinds);
        yyjson_mut_obj_add_val(d, caps, name, cap);
        return;
    }

    /* Phase 4 Plan 04-06 Task 03 (EDIT-10, D-09): renameProvider expects
     *   { prepareProvider: true }
     * to signal that textDocument/prepareRename is supported (so the
     * client does a round-trip classification before asking the user
     * for a new name). Without prepareProvider, the client rename UI
     * never gates the call — it dispatches textDocument/rename on
     * every cursor, regardless of whether the target is renameable. */
    if (strcmp(name, "renameProvider") == 0) {
        yyjson_mut_val *rp = yyjson_mut_obj(d);
        yyjson_mut_obj_add_bool(d, rp, "prepareProvider", true);
        yyjson_mut_obj_add_val(d, caps, name, rp);
        return;
    }

    /* Phase 5 Plan 05-04 (FMT-04, D-14): documentOnTypeFormattingProvider
     * expects { firstTriggerCharacter: "}", moreTriggerCharacter: [] }.
     * Handler-table auto-advertise emits booleans by default; this
     * explicit branch emits the required object shape. Pattern mirrors
     * signatureHelpProvider below. The trigger set is locked to `}`
     * only in v1 per D-05; additional triggers (`;`, `\n`, `)`, `:`)
     * are deferred to v1.x (FMT-DIFF-01). */
    if (strcmp(name, "documentOnTypeFormattingProvider") == 0) {
        yyjson_mut_val *ot = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, ot, "firstTriggerCharacter", "}");
        yyjson_mut_val *more = yyjson_mut_arr(d);   /* empty array */
        yyjson_mut_obj_add_val(d, ot, "moreTriggerCharacter", more);
        yyjson_mut_obj_add_val(d, caps, name, ot);
        return;
    }

    /* Phase 14 Plan 14-02 (CMD-01, CMD-03): executeCommandProvider expects
     *   { commands: ["iron.migrate"] }
     * Handler-table auto-advertise cannot produce this nested-object shape
     * (it only emits booleans by default); this explicit branch mirrors the
     * signatureHelpProvider precedent (Phase 3 D-13 decision: "signatureHelpProvider
     * shape override lives in capabilities.c caps_add — not handler-table auto-derive"). */
    if (strcmp(name, "executeCommandProvider") == 0) {
        yyjson_mut_val *exec_cmd = yyjson_mut_obj(d);
        yyjson_mut_val *cmds_arr = yyjson_mut_arr(d);
        yyjson_mut_arr_append(cmds_arr, yyjson_mut_str(d, "iron.migrate"));
        yyjson_mut_obj_add_val(d, exec_cmd, "commands", cmds_arr);
        yyjson_mut_obj_add_val(d, caps, name, exec_cmd);
        return;
    }

    /* Phase 3 Plan 04 Task 03 (D-13): signatureHelpProvider expects
     *   { triggerCharacters: ["(", ","], retriggerCharacters: [","] }.
     * Handler-table auto-advertise cannot produce this shape (it only
     * emits booleans + object-shape special cases), hence this explicit
     * branch. */
    if (strcmp(name, "signatureHelpProvider") == 0) {
        yyjson_mut_val *shp = yyjson_mut_obj(d);
        yyjson_mut_val *trig = yyjson_mut_arr(d);
        yyjson_mut_arr_add_strcpy(d, trig, "(");
        yyjson_mut_arr_add_strcpy(d, trig, ",");
        yyjson_mut_obj_add_val(d, shp, "triggerCharacters", trig);
        yyjson_mut_val *retr = yyjson_mut_arr(d);
        yyjson_mut_arr_add_strcpy(d, retr, ",");
        yyjson_mut_obj_add_val(d, shp, "retriggerCharacters", retr);
        yyjson_mut_obj_add_val(d, caps, name, shp);
        return;
    }

    /* Default shape: boolean true. Covers hoverProvider / definitionProvider /
     * referencesProvider / documentSymbolProvider / etc. */
    yyjson_mut_obj_add_bool(d, caps, name, true);
}

yyjson_mut_val *ilsp_capabilities_build(yyjson_mut_doc           *out_doc,
                                        IronLsp_PositionEncoding  negotiated_enc) {
    yyjson_mut_val *caps = yyjson_mut_obj(out_doc);

    /* Always-present: positionEncoding result (string, not array -- this
     * is the server's choice, not a client advertisement). */
    yyjson_mut_obj_add_strcpy(out_doc, caps, "positionEncoding",
                              ilsp_position_encoding_str(negotiated_enc));

    /* Derive capabilities from the compile-time handler table.
     * PITFALLS.md #15: no capability may be advertised without a handler. */
    for (size_t i = 0; i < ilsp_handler_table_size; i++) {
        const IronLsp_HandlerEntry *entry = &ilsp_handler_table[i];
        if (entry->capability) caps_add(out_doc, caps, entry->capability);
    }

    return caps;
}
