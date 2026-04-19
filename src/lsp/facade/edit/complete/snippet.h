#ifndef ILSP_SNIPPET_H
#define ILSP_SNIPPET_H

/* Phase 4 Plan 04-03 Task 02 (EDIT-04, D-15) -- snippet-body renderer
 * for completion candidates whose CompletionItemKind maps to one of
 * the 5 template shapes (function/method call, object literal, match,
 * import, enum variant).
 *
 * LSP 3.17 Snippet Syntax (Appendix) summary:
 *   $<int>            unnamed tab-stop
 *   ${<int>}          unnamed tab-stop (braced form)
 *   ${<int>:default}  tab-stop with default text
 *   $0                terminal cursor stop (always last)
 *   \$  \}  \\        escaped literal $, }, \
 *
 * PITFALL D (SECURITY): when embedding user-source text (identifiers,
 * parameter names, object field names) in a placeholder default, we
 * MUST escape `$`, `}`, `\`. Otherwise a hostile identifier like
 * `${USER}` would be interpreted by the client's snippet engine as a
 * variable substitution.  sb_append_escaped() in snippet.c performs
 * that escape on every user-derived byte stream.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/arena.h"      /* Iron_Arena (typedef to anonymous struct) */

#ifdef __cplusplus
extern "C" {
#endif

struct Iron_Symbol;

typedef enum {
    ILSP_SNIPPET_FUNCTION,       /* name(${1:p1}, ${2:p2})$0 */
    ILSP_SNIPPET_METHOD,         /* same shape as FUNCTION; caller picks */
    ILSP_SNIPPET_OBJECT_LITERAL, /* Name { ${1:f1}: ${2:v1}, ... }$0 */
    ILSP_SNIPPET_MATCH,          /* match ${1:expr} { ${2:Pattern} -> ${3:result}, }$0 */
    ILSP_SNIPPET_IMPORT,         /* import ${1:module}$0 */
    ILSP_SNIPPET_ENUM_VARIANT,   /* Name.Variant$0 or Name.Variant(${1:payload})$0 */
} IronLsp_SnippetKind;

/* Metadata the renderer needs for the kinds whose shape depends on
 * analyzer-derived info. Callers that have a resolved Iron_Symbol may
 * pass it for convenience; callers that only have AST node data can
 * fill the metadata fields directly.
 *
 * For FUNCTION / METHOD:
 *   - `name` is the callable's name (escaped by the renderer)
 *   - `param_names` is an optional array of parameter name strings;
 *      NULL means "zero-arg" regardless of param_count.
 *   - `param_count` is the count of param_names.
 *
 * For OBJECT_LITERAL:
 *   - `name` is the type name
 *   - `field_names` is an optional array of field name strings;
 *      NULL or field_count==0 emits `<Name> {}$0`.
 *
 * For ENUM_VARIANT:
 *   - `name` is the enum type name (e.g. "Color")
 *   - `variant_name` is the variant's name (e.g. "Red")
 *   - `payload_count` is 0 for payload-less, 1 for single-payload
 *     (multi-payload is treated the same as single-payload per D-15).
 *
 * For MATCH / IMPORT: no metadata needed; the template is a literal. */
typedef struct {
    const char        *name;
    const char        *variant_name;      /* ENUM_VARIANT only */
    const char *const *param_names;       /* FUNCTION / METHOD */
    const char *const *field_names;       /* OBJECT_LITERAL */
    int                param_count;
    int                field_count;
    int                payload_count;     /* ENUM_VARIANT */
} IronLsp_SnippetMeta;

/* Render a snippet body. Returns arena-owned NUL-terminated string on
 * success, NULL on OOM or bad input. All user-source text regions
 * pass through the `\$ \} \\` escaper (sb_append_escaped) before
 * embedding. */
const char *ilsp_snippet_render(IronLsp_SnippetKind        kind,
                                 const IronLsp_SnippetMeta *meta,
                                 Iron_Arena                *arena);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_SNIPPET_H */
