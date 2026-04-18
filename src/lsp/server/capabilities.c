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

    /* diagnosticProvider expects { interFileDependencies: bool, workspaceDiagnostics: bool }. */
    if (strcmp(name, "diagnosticProvider") == 0) {
        yyjson_mut_val *dp = yyjson_mut_obj(d);
        yyjson_mut_obj_add_bool(d, dp, "interFileDependencies", false);
        yyjson_mut_obj_add_bool(d, dp, "workspaceDiagnostics",  false);
        yyjson_mut_obj_add_val (d, caps, name, dp);
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
