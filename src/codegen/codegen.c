/* codegen.c — Main C code generator orchestrator.
 *
 * Implements iron_codegen():
 *   1. Includes
 *   2. Forward declarations (typedef struct Iron_Foo Iron_Foo;)
 *   3. Struct bodies (topologically sorted)
 *   4. Enum definitions
 *   5. Function prototypes
 *   6. Function implementations
 *   7. main() wrapper
 */

#include "codegen/codegen.h"
#include "analyzer/scope.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Utility helpers ──────────────────────────────────────────────────────── */

const char *iron_mangle_name(const char *name, Iron_Arena *arena) {
    /* "Player" -> "Iron_Player" */
    size_t len = strlen(name);
    size_t total = 5 + len + 1;  /* "Iron_" + name + '\0' */
    char *buf = (char *)iron_arena_alloc(arena, total, 1);
    memcpy(buf, "Iron_", 5);
    memcpy(buf + 5, name, len + 1);
    return buf;
}

const char *iron_mangle_method(const char *type_name, const char *method_name,
                                Iron_Arena *arena) {
    /* ("Player", "update") -> "Iron_Player_update" */
    size_t tlen = strlen(type_name);
    size_t mlen = strlen(method_name);
    size_t total = 5 + tlen + 1 + mlen + 1;  /* "Iron_" + type + "_" + method + '\0' */
    char *buf = (char *)iron_arena_alloc(arena, total, 1);
    memcpy(buf, "Iron_", 5);
    memcpy(buf + 5, type_name, tlen);
    buf[5 + tlen] = '_';
    memcpy(buf + 5 + tlen + 1, method_name, mlen + 1);
    return buf;
}

void codegen_indent(Iron_StrBuf *sb, int level) {
    for (int i = 0; i < level * 4; i++) {
        iron_strbuf_append(sb, " ", 1);
    }
}

/* ── Topological sort for struct emission ─────────────────────────────────── */

/* DFS visit colors */
#define TOPO_WHITE 0
#define TOPO_GRAY  1
#define TOPO_BLACK 2

typedef struct {
    Iron_ObjectDecl **sorted;   /* stb_ds array result */
    Iron_Program     *program;
    int              *colors;   /* per-decl color array (by index) */
    Iron_Codegen     *ctx;
    bool              has_cycle;
} TopoState;

/* Find the index of an ObjectDecl by name in program->decls */
static int find_object_index(Iron_Program *prog, const char *name) {
    for (int i = 0; i < prog->decl_count; i++) {
        if (prog->decls[i]->kind == IRON_NODE_OBJECT_DECL) {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)prog->decls[i];
            if (strcmp(od->name, name) == 0) return i;
        }
    }
    return -1;
}

static void topo_visit(TopoState *state, int idx) {
    if (state->colors[idx] == TOPO_BLACK) return;
    if (state->colors[idx] == TOPO_GRAY) {
        /* Cycle detected — emit E0223 */
        iron_diag_emit(state->ctx->diags, state->ctx->arena,
                        IRON_DIAG_ERROR, IRON_ERR_CIRCULAR_TYPE,
                        state->program->decls[idx]->span,
                        "circular type dependency detected", NULL);
        state->has_cycle = true;
        return;
    }

    state->colors[idx] = TOPO_GRAY;

    /* Visit dependencies: value-type fields that reference other objects */
    Iron_ObjectDecl *od = (Iron_ObjectDecl *)state->program->decls[idx];

    /* Dependency on parent (extends) */
    if (od->extends_name) {
        int dep = find_object_index(state->program, od->extends_name);
        if (dep >= 0) {
            topo_visit(state, dep);
        }
    }

    /* Value-type field dependencies */
    for (int i = 0; i < od->field_count; i++) {
        Iron_Field *f = (Iron_Field *)od->fields[i];
        if (!f->type_ann) continue;
        Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
        /* Only direct (non-nullable, non-pointer) fields create deps */
        if (ta->is_nullable) continue;
        int dep = find_object_index(state->program, ta->name);
        if (dep >= 0 && dep != idx) {
            topo_visit(state, dep);
        }
    }

    state->colors[idx] = TOPO_BLACK;
    arrput(state->sorted, od);
}

/* ── Type tag detection ───────────────────────────────────────────────────── */

/* Returns true if any object in the program extends the given object name */
static bool has_subtype(Iron_Program *prog, const char *name) {
    for (int i = 0; i < prog->decl_count; i++) {
        if (prog->decls[i]->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)prog->decls[i];
        if (od->extends_name && strcmp(od->extends_name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Struct body emission ─────────────────────────────────────────────────── */

static void emit_object_struct(Iron_Codegen *ctx, Iron_ObjectDecl *od,
                                int type_tag) {
    const char *mangled = iron_mangle_name(od->name, ctx->arena);
    iron_strbuf_appendf(&ctx->struct_bodies,
                         "struct %s {\n", mangled);

    if (od->extends_name) {
        /* Inheritance: embed parent struct as _base first field */
        const char *parent_mangled = iron_mangle_name(od->extends_name,
                                                       ctx->arena);
        iron_strbuf_appendf(&ctx->struct_bodies,
                             "    %s _base;\n", parent_mangled);
    } else if (has_subtype(ctx->program, od->name)) {
        /* Root of inheritance: add type tag */
        iron_strbuf_appendf(&ctx->struct_bodies,
                             "    int32_t iron_type_tag;\n");
    }

    for (int i = 0; i < od->field_count; i++) {
        Iron_Field *f = (Iron_Field *)od->fields[i];
        /* Resolve field type */
        const char *c_type = "int64_t";  /* fallback */
        if (f->type_ann) {
            Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
            if (ta->is_nullable) {
                /* Ensure optional type is emitted */
                /* Look up the inner type */
                Iron_Symbol *sym = iron_scope_lookup(ctx->global_scope,
                                                      ta->name);
                if (sym && sym->type) {
                    ensure_optional_type(ctx, sym->type);
                    Iron_StrBuf tmp = iron_strbuf_create(64);
                    iron_strbuf_appendf(&tmp, "Iron_Optional_%s",
                                        iron_mangle_name(ta->name, ctx->arena));
                    c_type = iron_arena_strdup(ctx->arena,
                                               iron_strbuf_get(&tmp),
                                               tmp.len);
                    iron_strbuf_free(&tmp);
                } else {
                    /* Build type name from annotation */
                    Iron_StrBuf tmp = iron_strbuf_create(64);
                    iron_strbuf_appendf(&tmp, "Iron_Optional_Iron_%s",
                                        ta->name);
                    c_type = iron_arena_strdup(ctx->arena,
                                               iron_strbuf_get(&tmp),
                                               tmp.len);
                    iron_strbuf_free(&tmp);
                }
            } else {
                /* Map annotation to C type */
                /* Try primitive mappings first */
                if (strcmp(ta->name, "Int") == 0) c_type = "int64_t";
                else if (strcmp(ta->name, "Int8") == 0) c_type = "int8_t";
                else if (strcmp(ta->name, "Int16") == 0) c_type = "int16_t";
                else if (strcmp(ta->name, "Int32") == 0) c_type = "int32_t";
                else if (strcmp(ta->name, "Int64") == 0) c_type = "int64_t";
                else if (strcmp(ta->name, "UInt") == 0) c_type = "uint64_t";
                else if (strcmp(ta->name, "UInt8") == 0) c_type = "uint8_t";
                else if (strcmp(ta->name, "UInt16") == 0) c_type = "uint16_t";
                else if (strcmp(ta->name, "UInt32") == 0) c_type = "uint32_t";
                else if (strcmp(ta->name, "UInt64") == 0) c_type = "uint64_t";
                else if (strcmp(ta->name, "Float") == 0) c_type = "double";
                else if (strcmp(ta->name, "Float32") == 0) c_type = "float";
                else if (strcmp(ta->name, "Float64") == 0) c_type = "double";
                else if (strcmp(ta->name, "Bool") == 0) c_type = "bool";
                else if (strcmp(ta->name, "String") == 0) c_type = "Iron_String";
                else {
                    /* User-defined type */
                    c_type = iron_mangle_name(ta->name, ctx->arena);
                }
            }
        }
        iron_strbuf_appendf(&ctx->struct_bodies,
                             "    %s %s;\n", c_type, f->name);
    }

    iron_strbuf_appendf(&ctx->struct_bodies, "};\n");

    /* Emit IRON_TAG define for this type */
    iron_strbuf_appendf(&ctx->struct_bodies,
                         "#define IRON_TAG_%s %d\n", mangled, type_tag);
}

/* ── iron_codegen orchestrator ────────────────────────────────────────────── */

const char *iron_codegen(Iron_Program *program, Iron_Scope *global_scope,
                         Iron_Arena *arena, Iron_DiagList *diags) {
    if (!program) return NULL;
    if (diags->error_count > 0) return NULL;

    /* Initialize context */
    Iron_Codegen ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena        = arena;
    ctx.diags        = diags;
    ctx.global_scope = global_scope;
    ctx.program      = program;
    ctx.next_type_tag = 1;

    ctx.includes        = iron_strbuf_create(256);
    ctx.forward_decls   = iron_strbuf_create(256);
    ctx.struct_bodies   = iron_strbuf_create(1024);
    ctx.enum_defs       = iron_strbuf_create(256);
    ctx.global_consts   = iron_strbuf_create(256);
    ctx.prototypes      = iron_strbuf_create(512);
    ctx.implementations = iron_strbuf_create(4096);
    ctx.main_wrapper    = iron_strbuf_create(128);

    ctx.defer_stacks         = NULL;
    ctx.defer_depth          = 0;
    ctx.function_scope_depth = 0;
    ctx.emitted_optionals    = NULL;
    ctx.mono_registry        = NULL;
    ctx.lambda_counter       = 0;
    ctx.spawn_counter        = 0;
    ctx.parallel_counter     = 0;
    ctx.lifted_funcs         = iron_strbuf_create(1024);

    /* ── 1. Includes ──────────────────────────────────────────────────────── */
    /* Runtime header first — it brings in Iron_String, Iron_println,
     * iron_runtime_init/shutdown, Iron_Pool, and all collection types.
     * Standard headers are pulled in transitively by the runtime header,
     * but we also emit them explicitly for clarity and editor tooling. */
    iron_strbuf_appendf(&ctx.includes,
                         "#include \"runtime/iron_runtime.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdint.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdbool.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdlib.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <string.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdio.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_math.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_io.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_time.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_log.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "\n");

    /* ── 2. Forward declarations ─────────────────────────────────────────── */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (decl->kind == IRON_NODE_OBJECT_DECL) {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)decl;
            const char *mangled = iron_mangle_name(od->name, arena);
            iron_strbuf_appendf(&ctx.forward_decls,
                                 "typedef struct %s %s;\n", mangled, mangled);
        }
        if (decl->kind == IRON_NODE_INTERFACE_DECL) {
            Iron_InterfaceDecl *id = (Iron_InterfaceDecl *)decl;
            const char *mangled = iron_mangle_name(id->name, arena);
            iron_strbuf_appendf(&ctx.forward_decls,
                                 "typedef struct %s %s;\n", mangled, mangled);
        }
    }
    if (ctx.forward_decls.len > 0) {
        iron_strbuf_appendf(&ctx.forward_decls, "\n");
    }

    /* ── 3. Struct bodies (topologically sorted) ──────────────────────────── */
    /* Count object decls */
    int obj_count = 0;
    for (int i = 0; i < program->decl_count; i++) {
        if (program->decls[i]->kind == IRON_NODE_OBJECT_DECL) obj_count++;
    }

    if (obj_count > 0) {
        int *colors = (int *)iron_arena_alloc(arena,
                                               sizeof(int) * (size_t)program->decl_count,
                                               _Alignof(int));
        memset(colors, 0, sizeof(int) * (size_t)program->decl_count);

        TopoState topo;
        topo.sorted    = NULL;
        topo.program   = program;
        topo.colors    = colors;
        topo.ctx       = &ctx;
        topo.has_cycle = false;

        for (int i = 0; i < program->decl_count; i++) {
            if (program->decls[i]->kind == IRON_NODE_OBJECT_DECL &&
                colors[i] == TOPO_WHITE) {
                topo_visit(&topo, i);
            }
        }

        for (int i = 0; i < (int)arrlen(topo.sorted); i++) {
            emit_object_struct(&ctx, topo.sorted[i], ctx.next_type_tag++);
        }
        iron_strbuf_appendf(&ctx.struct_bodies, "\n");

        arrfree(topo.sorted);
    }

    /* ── 3b. Interface vtable structs and ref types ───────────────────────── */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (decl->kind != IRON_NODE_INTERFACE_DECL) continue;
        Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)decl;
        emit_interface_vtable_struct(&ctx, iface);
    }
    if (ctx.struct_bodies.len > 0) {
        iron_strbuf_appendf(&ctx.struct_bodies, "\n");
    }

    /* ── 4. Enum definitions ─────────────────────────────────────────────── */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (decl->kind != IRON_NODE_ENUM_DECL) continue;
        Iron_EnumDecl *ed = (Iron_EnumDecl *)decl;
        const char *mangled = iron_mangle_name(ed->name, arena);
        iron_strbuf_appendf(&ctx.enum_defs, "typedef enum {\n");
        for (int j = 0; j < ed->variant_count; j++) {
            Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
            if (ev->has_explicit_value) {
                iron_strbuf_appendf(&ctx.enum_defs, "    %s_%s = %d",
                                     mangled, ev->name, ev->explicit_value);
            } else {
                iron_strbuf_appendf(&ctx.enum_defs, "    %s_%s",
                                     mangled, ev->name);
            }
            if (j < ed->variant_count - 1) {
                iron_strbuf_appendf(&ctx.enum_defs, ",");
            }
            iron_strbuf_appendf(&ctx.enum_defs, "\n");
        }
        iron_strbuf_appendf(&ctx.enum_defs, "} %s;\n\n", mangled);
    }

    /* ── 4b. Top-level val/var declarations (global constants) ───────────── */
    /* After comptime evaluation, top-level val GREETING = comptime "..."
     * becomes val GREETING = STRING_LIT "...".  We emit these as global C
     * variables so they are visible from all functions in the file.
     *
     * Strategy:
     *  - Non-string types (int, float, bool): emit as static global with
     *    a C constant-expression initializer.
     *  - String types: emit declaration only (no static initializer since
     *    iron_string_from_literal() is a runtime call).  Emit initialization
     *    at the start of Iron_main via global_consts (init code section). */
    Iron_StrBuf global_inits = iron_strbuf_create(256); /* init calls for Iron_main */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        const char *var_name = NULL;
        Iron_Node  *init_expr = NULL;

        if (decl->kind == IRON_NODE_VAL_DECL) {
            Iron_ValDecl *vd = (Iron_ValDecl *)decl;
            var_name  = vd->name;
            init_expr = vd->init;
        } else if (decl->kind == IRON_NODE_VAR_DECL) {
            Iron_VarDecl *vd = (Iron_VarDecl *)decl;
            var_name  = vd->name;
            init_expr = vd->init;
        }

        if (!var_name) continue;

        /* Determine C type from declared_type or init_expr kind */
        const char *c_type = "int64_t";  /* safe default */
        bool is_string_type = false;

        /* Helper to derive type */
        {
            const Iron_Type *dtype = NULL;
            if (decl->kind == IRON_NODE_VAL_DECL)
                dtype = ((Iron_ValDecl *)decl)->declared_type;
            else
                dtype = ((Iron_VarDecl *)decl)->declared_type;

            if (dtype) {
                c_type = iron_type_to_c(dtype, &ctx);
                is_string_type = (dtype->kind == IRON_TYPE_STRING);
            } else if (init_expr) {
                switch (init_expr->kind) {
                    case IRON_NODE_STRING_LIT:
                        c_type = "Iron_String"; is_string_type = true; break;
                    case IRON_NODE_INT_LIT:
                        c_type = "int64_t"; break;
                    case IRON_NODE_FLOAT_LIT:
                        c_type = "double"; break;
                    case IRON_NODE_BOOL_LIT:
                        c_type = "bool"; break;
                    default: break;
                }
            }
        }

        if (is_string_type) {
            /* Declare as uninitialized global; initialize in Iron_main preamble */
            iron_strbuf_appendf(&ctx.global_consts, "static Iron_String %s;\n",
                                 var_name);
            if (init_expr && init_expr->kind == IRON_NODE_STRING_LIT) {
                Iron_StringLit *sl = (Iron_StringLit *)init_expr;
                const char *sval = sl->value ? sl->value : "";
                size_t slen = sl->value ? strlen(sl->value) : 0;
                iron_strbuf_appendf(&global_inits,
                                     "    %s = iron_string_from_literal(\"%s\", %zu);\n",
                                     var_name, sval, slen);
            }
        } else {
            /* Emit as static global with C constant initializer */
            iron_strbuf_appendf(&ctx.global_consts, "static %s %s",
                                 c_type, var_name);
            if (init_expr) {
                iron_strbuf_appendf(&ctx.global_consts, " = ");
                emit_expr(&ctx.global_consts, init_expr, &ctx);
            }
            iron_strbuf_appendf(&ctx.global_consts, ";\n");
        }
    }
    if (ctx.global_consts.len > 0) {
        iron_strbuf_appendf(&ctx.global_consts, "\n");
    }

    /* ── 5 & 6. Function prototypes and implementations ───────────────────── */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (decl->kind == IRON_NODE_FUNC_DECL) {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
            /* Skip extern funcs — they are declared in external C headers */
            if (fd->is_extern) continue;
            emit_func_prototype(&ctx, fd);
        } else if (decl->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *md = (Iron_MethodDecl *)decl;
            emit_method_prototype(&ctx, md);
        }
    }
    if (ctx.prototypes.len > 0) {
        iron_strbuf_appendf(&ctx.prototypes, "\n");
    }

    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (decl->kind == IRON_NODE_FUNC_DECL) {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
            /* Skip extern funcs — no implementation to emit */
            if (fd->is_extern) continue;
            emit_func_impl(&ctx, fd);
        } else if (decl->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *md = (Iron_MethodDecl *)decl;
            emit_method_impl(&ctx, md);
        }
    }

    /* ── 6b. Vtable instances (emitted after all function impls are available) */
    /* For each object that implements interfaces, emit a static vtable instance */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (decl->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)decl;
        for (int j = 0; j < od->implements_count; j++) {
            const char *iface_name = od->implements_names[j];
            /* Find the interface decl */
            for (int k = 0; k < program->decl_count; k++) {
                Iron_Node *id = program->decls[k];
                if (id->kind != IRON_NODE_INTERFACE_DECL) continue;
                Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)id;
                if (strcmp(iface->name, iface_name) == 0) {
                    emit_vtable_instance(&ctx, od->name, iface);
                    break;
                }
            }
        }
    }

    /* ── 7. main() wrapper ───────────────────────────────────────────────── */
    iron_strbuf_appendf(&ctx.main_wrapper,
                         "int main(int argc, char** argv) {\n");
    iron_strbuf_appendf(&ctx.main_wrapper,
                         "    (void)argc; (void)argv;\n");
    iron_strbuf_appendf(&ctx.main_wrapper,
                         "    iron_runtime_init();\n");
    /* Emit global string constant initializations (require runtime to be up) */
    if (global_inits.len > 0) {
        iron_strbuf_append(&ctx.main_wrapper,
                            iron_strbuf_get(&global_inits), global_inits.len);
    }
    iron_strbuf_appendf(&ctx.main_wrapper,
                         "    Iron_main();\n");
    iron_strbuf_appendf(&ctx.main_wrapper,
                         "    iron_runtime_shutdown();\n");
    iron_strbuf_appendf(&ctx.main_wrapper,
                         "    return 0;\n");
    iron_strbuf_appendf(&ctx.main_wrapper,
                         "}\n");

    /* ── Concatenate all sections ─────────────────────────────────────────── */
    Iron_StrBuf output = iron_strbuf_create(8192);

    iron_strbuf_appendf(&output, "/* Generated by Iron compiler */\n\n");
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.includes),
                        ctx.includes.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.forward_decls),
                        ctx.forward_decls.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.struct_bodies),
                        ctx.struct_bodies.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.enum_defs),
                        ctx.enum_defs.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.global_consts),
                        ctx.global_consts.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.prototypes),
                        ctx.prototypes.len);
    /* Lifted functions (lambda bodies, spawn bodies, parallel chunks) */
    if (ctx.lifted_funcs.len > 0) {
        iron_strbuf_append(&output, iron_strbuf_get(&ctx.lifted_funcs),
                            ctx.lifted_funcs.len);
    }
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.implementations),
                        ctx.implementations.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.main_wrapper),
                        ctx.main_wrapper.len);

    /* Arena-dup the final string */
    const char *result = iron_arena_strdup(arena, iron_strbuf_get(&output),
                                            output.len);

    /* Free working buffers */
    iron_strbuf_free(&ctx.includes);
    iron_strbuf_free(&ctx.forward_decls);
    iron_strbuf_free(&ctx.struct_bodies);
    iron_strbuf_free(&ctx.enum_defs);
    iron_strbuf_free(&ctx.global_consts);
    iron_strbuf_free(&ctx.prototypes);
    iron_strbuf_free(&ctx.implementations);
    iron_strbuf_free(&ctx.lifted_funcs);
    iron_strbuf_free(&ctx.main_wrapper);
    iron_strbuf_free(&global_inits);
    iron_strbuf_free(&output);

    /* Free defer_stacks */
    for (int i = 0; i < (int)arrlen(ctx.defer_stacks); i++) {
        if (ctx.defer_stacks[i]) {
            arrfree(ctx.defer_stacks[i]);
        }
    }
    arrfree(ctx.defer_stacks);
    arrfree(ctx.emitted_optionals);
    shfree(ctx.mono_registry);

    return result;
}
