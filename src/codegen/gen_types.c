/* gen_types.c — Iron-to-C type mapping, Optional struct emission,
 *               interface vtable codegen, and generic monomorphization.
 *
 * Provides:
 *   iron_type_to_c()           — map an Iron_Type to its C representation
 *   ensure_optional_type()     — emit Iron_Optional_T struct if not yet emitted
 *   emit_interface_vtable_struct() — emit vtable struct and ref type
 *   emit_vtable_instance()     — emit static vtable initializer
 *   mangle_generic()           — build mangled name for generic instantiation
 *   ensure_monomorphized_type() — emit stub struct for generic, dedup via registry
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

/* ── Interface vtable emission ────────────────────────────────────────────── */

void emit_interface_vtable_struct(Iron_Codegen *ctx,
                                   Iron_InterfaceDecl *iface) {
    const char *iface_mangled = iron_mangle_name(iface->name, ctx->arena);

    /* typedef struct Iron_<IFace>_vtable { ... } Iron_<IFace>_vtable; */
    Iron_StrBuf *sb = &ctx->struct_bodies;
    iron_strbuf_appendf(sb, "typedef struct %s_vtable {\n", iface_mangled);

    for (int i = 0; i < iface->method_count; i++) {
        Iron_Node *sig_node = iface->method_sigs[i];
        if (!sig_node) continue;

        /* Method sigs are FuncDecl nodes with body=NULL */
        if (sig_node->kind == IRON_NODE_FUNC_DECL) {
            Iron_FuncDecl *sig = (Iron_FuncDecl *)sig_node;
            const char *ret_type = "void";
            if (sig->resolved_return_type) {
                ret_type = iron_type_to_c(sig->resolved_return_type, ctx);
            }
            /* Build param list for cast (void* self, param_types...) */
            iron_strbuf_appendf(sb, "    %s (*%s)(void* self", ret_type,
                                 sig->name);
            for (int j = 0; j < sig->param_count; j++) {
                Iron_Param *p = (Iron_Param *)sig->params[j];
                /* Use type annotation directly since params may not be resolved */
                const char *pt = "void*";
                if (p->type_ann) {
                    /* Minimal type resolution from annotation name */
                    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)p->type_ann;
                    if (strcmp(ta->name, "Int") == 0) pt = "int64_t";
                    else if (strcmp(ta->name, "Float") == 0) pt = "double";
                    else if (strcmp(ta->name, "Bool") == 0) pt = "bool";
                    else if (strcmp(ta->name, "String") == 0) pt = "Iron_String";
                    else pt = iron_mangle_name(ta->name, ctx->arena);
                }
                iron_strbuf_appendf(sb, ", %s %s", pt, p->name);
            }
            iron_strbuf_appendf(sb, ");\n");
        }
    }

    iron_strbuf_appendf(sb, "} %s_vtable;\n", iface_mangled);

    /* Interface ref type: typedef struct { void *object; Iron_<IFace>_vtable *vtable; } Iron_<IFace>_ref; */
    iron_strbuf_appendf(sb,
        "typedef struct { void *object; %s_vtable *vtable; } %s_ref;\n",
        iface_mangled, iface_mangled);
}

void emit_vtable_instance(Iron_Codegen *ctx, const char *type_name,
                            Iron_InterfaceDecl *iface) {
    const char *iface_mangled = iron_mangle_name(iface->name, ctx->arena);
    const char *type_mangled  = iron_mangle_name(type_name, ctx->arena);

    /* static Iron_<IFace>_vtable Iron_<Type>_<IFace>_vtable = { ... }; */
    Iron_StrBuf *sb = &ctx->implementations;
    iron_strbuf_appendf(sb, "static %s_vtable %s_%s_vtable = {\n",
                         iface_mangled, type_mangled, iface->name);

    for (int i = 0; i < iface->method_count; i++) {
        Iron_Node *sig_node = iface->method_sigs[i];
        if (!sig_node) continue;

        if (sig_node->kind == IRON_NODE_FUNC_DECL) {
            Iron_FuncDecl *sig = (Iron_FuncDecl *)sig_node;
            /* Build the cast: (ret(*)(void*,...))Iron_<Type>_<method> */
            const char *ret_type = "void";
            if (sig->resolved_return_type) {
                ret_type = iron_type_to_c(sig->resolved_return_type, ctx);
            }
            const char *impl_name = iron_mangle_method(type_name, sig->name,
                                                         ctx->arena);
            /* Cast signature: (ret (*)(void*, params...)) */
            iron_strbuf_appendf(sb, "    .%s = (%s (*)(void*", sig->name,
                                 ret_type);
            for (int j = 0; j < sig->param_count; j++) {
                Iron_Param *p = (Iron_Param *)sig->params[j];
                const char *pt = "void*";
                if (p->type_ann) {
                    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)p->type_ann;
                    if (strcmp(ta->name, "Int") == 0) pt = "int64_t";
                    else if (strcmp(ta->name, "Float") == 0) pt = "double";
                    else if (strcmp(ta->name, "Bool") == 0) pt = "bool";
                    else if (strcmp(ta->name, "String") == 0) pt = "Iron_String";
                    else pt = iron_mangle_name(ta->name, ctx->arena);
                }
                iron_strbuf_appendf(sb, ", %s", pt);
            }
            iron_strbuf_appendf(sb, "))%s", impl_name);
            if (i < iface->method_count - 1) {
                iron_strbuf_appendf(sb, ",");
            }
            iron_strbuf_appendf(sb, "\n");
        }
    }

    iron_strbuf_appendf(sb, "};\n\n");
}

/* ── Generic monomorphization ─────────────────────────────────────────────── */

const char *mangle_generic(const char *base, Iron_Type **args, int count,
                             Iron_Arena *a) {
    /* Build "Iron_<base>_<c_arg1>_<c_arg2>..." */
    Iron_StrBuf sb = iron_strbuf_create(64);
    iron_strbuf_appendf(&sb, "Iron_%s", base);
    for (int i = 0; i < count; i++) {
        /* Map the arg type to a simple identifier-safe name */
        const char *arg_name = NULL;
        if (!args[i]) {
            arg_name = "void";
        } else {
            switch (args[i]->kind) {
                case IRON_TYPE_INT:     arg_name = "int64_t";  break;
                case IRON_TYPE_INT8:    arg_name = "int8_t";   break;
                case IRON_TYPE_INT16:   arg_name = "int16_t";  break;
                case IRON_TYPE_INT32:   arg_name = "int32_t";  break;
                case IRON_TYPE_INT64:   arg_name = "int64_t";  break;
                case IRON_TYPE_UINT:    arg_name = "uint64_t"; break;
                case IRON_TYPE_UINT8:   arg_name = "uint8_t";  break;
                case IRON_TYPE_UINT16:  arg_name = "uint16_t"; break;
                case IRON_TYPE_UINT32:  arg_name = "uint32_t"; break;
                case IRON_TYPE_UINT64:  arg_name = "uint64_t"; break;
                case IRON_TYPE_FLOAT:   arg_name = "double";   break;
                case IRON_TYPE_FLOAT32: arg_name = "float";    break;
                case IRON_TYPE_FLOAT64: arg_name = "double";   break;
                case IRON_TYPE_BOOL:    arg_name = "bool";     break;
                case IRON_TYPE_STRING:  arg_name = "Iron_String"; break;
                case IRON_TYPE_OBJECT:
                    arg_name = iron_mangle_name(args[i]->object.decl->name, a);
                    break;
                case IRON_TYPE_ENUM:
                    arg_name = iron_mangle_name(args[i]->enu.decl->name, a);
                    break;
                case IRON_TYPE_INTERFACE:
                    arg_name = iron_mangle_name(args[i]->interface.decl->name, a);
                    break;
                default:
                    arg_name = "void_ptr";
                    break;
            }
        }
        iron_strbuf_appendf(&sb, "_%s", arg_name);
    }
    const char *result = iron_arena_strdup(a, iron_strbuf_get(&sb), sb.len);
    iron_strbuf_free(&sb);
    return result;
}

const char *ensure_monomorphized_type(Iron_Codegen *ctx, const char *base_name,
                                       Iron_Type **args, int count) {
    const char *mangled = mangle_generic(base_name, args, count, ctx->arena);

    /* Check registry — if already emitted, return the name */
    ptrdiff_t idx = shgeti(ctx->mono_registry, mangled);
    if (idx >= 0 && ctx->mono_registry[idx].value) {
        return mangled;
    }

    /* Add to registry */
    shput(ctx->mono_registry, mangled, true);

    /* Emit forward declaration */
    iron_strbuf_appendf(&ctx->forward_decls,
                         "typedef struct %s %s;\n", mangled, mangled);

    /* Emit stub struct body with items/count/capacity pattern.
     * Actual collection implementations are Phase 3 runtime. */
    iron_strbuf_appendf(&ctx->struct_bodies, "struct %s {\n", mangled);

    /* items field: first arg type pointer */
    if (count > 0 && args[0]) {
        const char *elem_c = iron_type_to_c(args[0], ctx);
        iron_strbuf_appendf(&ctx->struct_bodies,
                             "    %s *items;\n", elem_c);
    } else {
        iron_strbuf_appendf(&ctx->struct_bodies, "    void *items;\n");
    }
    iron_strbuf_appendf(&ctx->struct_bodies, "    int64_t count;\n");
    iron_strbuf_appendf(&ctx->struct_bodies, "    int64_t capacity;\n");
    iron_strbuf_appendf(&ctx->struct_bodies, "};\n");

    /* Emit prototype stubs for standard methods */
    iron_strbuf_appendf(&ctx->prototypes,
                         "void %s_push(%s *self, %s item);\n",
                         mangled, mangled,
                         (count > 0 && args[0]) ? iron_type_to_c(args[0], ctx) : "void*");
    iron_strbuf_appendf(&ctx->prototypes,
                         "int64_t %s_len(const %s *self);\n",
                         mangled, mangled);

    return mangled;
}
