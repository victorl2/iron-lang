/* gen_types.c — Iron-to-C type mapping and Optional struct emission.
 *
 * Provides:
 *   iron_type_to_c()     — map an Iron_Type to its C representation
 *   ensure_optional_type() — emit Iron_Optional_T struct if not yet emitted
 */

#include "codegen/codegen.h"

#include <stdio.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static const char *optional_struct_name(const Iron_Type *inner,
                                         Iron_Codegen *ctx) {
    /* Build "Iron_Optional_<c_inner_type>" but replace spaces/stars with _ */
    const char *c_inner = iron_type_to_c(inner, ctx);

    /* Allocate a buffer via strbuf, then arena-dup it */
    Iron_StrBuf sb = iron_strbuf_create(64);
    iron_strbuf_appendf(&sb, "Iron_Optional_");
    /* Replace special chars in c_inner to make a valid C identifier */
    for (const char *p = c_inner; *p; p++) {
        if (*p == ' ' || *p == '*' || *p == '[' || *p == ']') {
            iron_strbuf_appendf(&sb, "_");
        } else {
            char ch[2] = { *p, '\0' };
            iron_strbuf_appendf(&sb, "%s", ch);
        }
    }
    const char *result = iron_arena_strdup(ctx->arena, iron_strbuf_get(&sb),
                                            sb.len);
    iron_strbuf_free(&sb);
    return result;
}

/* ── iron_type_to_c ───────────────────────────────────────────────────────── */

const char *iron_type_to_c(const Iron_Type *t, Iron_Codegen *ctx) {
    if (!t) return "void";

    switch (t->kind) {
        case IRON_TYPE_INT:     return "int64_t";
        case IRON_TYPE_INT8:    return "int8_t";
        case IRON_TYPE_INT16:   return "int16_t";
        case IRON_TYPE_INT32:   return "int32_t";
        case IRON_TYPE_INT64:   return "int64_t";
        case IRON_TYPE_UINT:    return "uint64_t";
        case IRON_TYPE_UINT8:   return "uint8_t";
        case IRON_TYPE_UINT16:  return "uint16_t";
        case IRON_TYPE_UINT32:  return "uint32_t";
        case IRON_TYPE_UINT64:  return "uint64_t";
        case IRON_TYPE_FLOAT:   return "double";
        case IRON_TYPE_FLOAT32: return "float";
        case IRON_TYPE_FLOAT64: return "double";
        case IRON_TYPE_BOOL:    return "bool";
        case IRON_TYPE_STRING:  return "Iron_String";
        case IRON_TYPE_VOID:    return "void";
        case IRON_TYPE_NULL:    return "void*";
        case IRON_TYPE_ERROR:   return "int";  /* error recovery fallback */

        case IRON_TYPE_OBJECT:
            return iron_mangle_name(t->object.decl->name, ctx->arena);

        case IRON_TYPE_ENUM:
            return iron_mangle_name(t->enu.decl->name, ctx->arena);

        case IRON_TYPE_INTERFACE:
            return iron_mangle_name(t->interface.decl->name, ctx->arena);

        case IRON_TYPE_NULLABLE: {
            ensure_optional_type(ctx, t->nullable.inner);
            return optional_struct_name(t->nullable.inner, ctx);
        }

        case IRON_TYPE_RC: {
            /* Stub: emit a pointer to the inner type */
            const char *inner_c = iron_type_to_c(t->rc.inner, ctx);
            Iron_StrBuf sb = iron_strbuf_create(64);
            iron_strbuf_appendf(&sb, "%s*", inner_c);
            const char *result = iron_arena_strdup(ctx->arena,
                                                    iron_strbuf_get(&sb),
                                                    sb.len);
            iron_strbuf_free(&sb);
            return result;
        }

        case IRON_TYPE_FUNC: {
            /* Emit as void* for now (function pointer syntax is complex) */
            return "void*";
        }

        case IRON_TYPE_ARRAY: {
            /* Emit element type; caller is responsible for [] suffix */
            return iron_type_to_c(t->array.elem, ctx);
        }

        case IRON_TYPE_GENERIC_PARAM:
            /* Generic params should be monomorphized before codegen; fallback */
            return "void*";
    }

    return "int";  /* unreachable fallback */
}

/* ── ensure_optional_type ─────────────────────────────────────────────────── */

void ensure_optional_type(Iron_Codegen *ctx, const Iron_Type *inner) {
    const char *struct_name = optional_struct_name(inner, ctx);

    /* Check if already emitted */
    for (int i = 0; i < arrlen(ctx->emitted_optionals); i++) {
        if (strcmp(ctx->emitted_optionals[i], struct_name) == 0) {
            return;
        }
    }

    /* Mark as emitted */
    arrput(ctx->emitted_optionals,
           iron_arena_strdup(ctx->arena, struct_name, strlen(struct_name)));

    /* Emit the Optional struct definition into struct_bodies */
    const char *c_inner = iron_type_to_c(inner, ctx);
    iron_strbuf_appendf(&ctx->struct_bodies,
                         "typedef struct { %s value; bool has_value; } %s;\n",
                         c_inner, struct_name);
}
