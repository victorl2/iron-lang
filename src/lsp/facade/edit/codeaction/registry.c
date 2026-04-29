/* Phase 4 Plan 04-04 Task 01 (EDIT-07) — code-action registry.
 *
 * Dispatch table keyed on Iron_Diagnostic.code; mirrors the bsearch
 * pattern in src/lsp/server/dispatch.c. The table MUST remain sorted
 * ASC by code — bsearch relies on it, and the unit test asserts it.
 */

#include "lsp/facade/edit/codeaction/registry.h"

#include <stdlib.h>

/* Sort order MUST be ASC by numeric code value (bsearch invariant; the
 * test_table_sorted_asc_by_code unit test enforces this). Note that
 * IRON_ERR_TYPE_MISMATCH_LITERAL = 292 and IRON_ERR_MISSING_RETURN = 293
 * after the Phase 80 MUT renumber (see diagnostics.h:283-285); they
 * therefore sort AFTER 260/261, not before. */
const IronLsp_QuickfixEntry ilsp_quickfix_table[] = {
    /* 200  */ { IRON_ERR_UNDEFINED_VAR,           ilsp_quickfix_undefined_var          },
    /* 260  */ { IRON_ERR_V3_RECEIVER_SYNTAX,      ilsp_quickfix_v3_receiver_syntax     },
    /* 261  */ { IRON_ERR_V3_MUT_RECEIVER,         ilsp_quickfix_v3_receiver_syntax     },  /* same handler — D-18 */
    /* 262  */ { IRON_ERR_V3_INLINE_DEFAULT,       ilsp_quickfix_v3_inline_default      },
    /* 264  */ { IRON_ERR_V3_NO_INIT,              ilsp_quickfix_object_no_init         },
    /* 292  */ { IRON_ERR_TYPE_MISMATCH_LITERAL,   ilsp_quickfix_type_mismatch_literal  },
    /* 293  */ { IRON_ERR_MISSING_RETURN,          ilsp_quickfix_missing_return         },
    /* 611  */ { IRON_WARN_UNUSED_IMPORT,          ilsp_quickfix_unused_import          },
    /* 612  */ { IRON_WARN_REDUNDANT_CAST,         ilsp_quickfix_redundant_cast         },
};
const size_t ilsp_quickfix_table_size =
    sizeof(ilsp_quickfix_table) / sizeof(ilsp_quickfix_table[0]);

static int qf_cmp(const void *key, const void *entry) {
    int ca = ((const IronLsp_QuickfixEntry *)key)->code;
    int cb = ((const IronLsp_QuickfixEntry *)entry)->code;
    return (ca > cb) - (ca < cb);
}

IronLsp_QuickfixFn ilsp_quickfix_lookup(int code) {
    IronLsp_QuickfixEntry key = { code, NULL };
    const IronLsp_QuickfixEntry *hit = (const IronLsp_QuickfixEntry *)bsearch(
        &key, ilsp_quickfix_table, ilsp_quickfix_table_size,
        sizeof(IronLsp_QuickfixEntry), qf_cmp);
    return hit ? hit->handler : NULL;
}
