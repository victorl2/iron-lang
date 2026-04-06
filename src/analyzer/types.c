#include "analyzer/types.h"
#include "parser/ast.h"
#include "util/arena.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

/* ── Primitive singleton table ───────────────────────────────────────────── */

/* Static storage for interned primitive types.
 * Indexed directly by Iron_TypeKind value.
 * Only kinds in the "primitive" range are populated; others are left zeroed.
 */
static Iron_Type s_primitives[IRON_TYPE_ERROR + 1];
static bool      s_initialized = false;

/* Kinds that have interned singletons (IRON_TYPE_VOID and IRON_TYPE_NULL
 * and IRON_TYPE_ERROR are also interned for convenience). */
static bool is_primitive_kind(Iron_TypeKind kind) {
    switch (kind) {
        case IRON_TYPE_INT:
        case IRON_TYPE_INT8:
        case IRON_TYPE_INT16:
        case IRON_TYPE_INT32:
        case IRON_TYPE_INT64:
        case IRON_TYPE_UINT:
        case IRON_TYPE_UINT8:
        case IRON_TYPE_UINT16:
        case IRON_TYPE_UINT32:
        case IRON_TYPE_UINT64:
        case IRON_TYPE_FLOAT:
        case IRON_TYPE_FLOAT32:
        case IRON_TYPE_FLOAT64:
        case IRON_TYPE_BOOL:
        case IRON_TYPE_STRING:
        case IRON_TYPE_VOID:
        case IRON_TYPE_NULL:
        case IRON_TYPE_ERROR:
            return true;
        default:
            return false;
    }
}

void iron_types_init(Iron_Arena *arena) {
    (void)arena; /* reserved for future use */
    if (s_initialized) return;
    memset(s_primitives, 0, sizeof(s_primitives));
    /* Stamp the kind field for every internable slot */
    for (int k = IRON_TYPE_INT; k <= IRON_TYPE_ERROR; k++) {
        s_primitives[k].kind = (Iron_TypeKind)k;
    }
    s_initialized = true;
}

Iron_Type *iron_type_make_primitive(Iron_TypeKind kind) {
    if (!is_primitive_kind(kind)) return NULL;
    return &s_primitives[kind];
}

/* ── Compound type constructors ──────────────────────────────────────────── */

Iron_Type *iron_type_make_nullable(Iron_Arena *a, Iron_Type *inner) {
    Iron_Type *t = ARENA_ALLOC(a, Iron_Type);
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->kind           = IRON_TYPE_NULLABLE;
    t->nullable.inner = inner;
    return t;
}

Iron_Type *iron_type_make_rc(Iron_Arena *a, Iron_Type *inner) {
    Iron_Type *t = ARENA_ALLOC(a, Iron_Type);
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->kind     = IRON_TYPE_RC;
    t->rc.inner = inner;
    return t;
}

Iron_Type *iron_type_make_func(Iron_Arena *a, Iron_Type **params, int count, Iron_Type *ret) {
    Iron_Type *t = ARENA_ALLOC(a, Iron_Type);
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->kind              = IRON_TYPE_FUNC;
    t->func.param_count  = count;
    t->func.return_type  = ret;
    if (count > 0 && params) {
        Iron_Type **copy = (Iron_Type **)iron_arena_alloc(a,
            sizeof(Iron_Type *) * (size_t)count, _Alignof(Iron_Type *));
        if (!copy) return NULL;
        memcpy(copy, params, sizeof(Iron_Type *) * (size_t)count);
        t->func.param_types = copy;
    } else {
        t->func.param_types = NULL;
    }
    return t;
}

Iron_Type *iron_type_make_array(Iron_Arena *a, Iron_Type *elem, int size) {
    Iron_Type *t = ARENA_ALLOC(a, Iron_Type);
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->kind       = IRON_TYPE_ARRAY;
    t->array.elem = elem;
    t->array.size = size;
    return t;
}

Iron_Type *iron_type_make_object(Iron_Arena *a, struct Iron_ObjectDecl *decl) {
    Iron_Type *t = ARENA_ALLOC(a, Iron_Type);
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->kind        = IRON_TYPE_OBJECT;
    t->object.decl = decl;
    return t;
}

Iron_Type *iron_type_make_interface(Iron_Arena *a, struct Iron_InterfaceDecl *decl) {
    Iron_Type *t = ARENA_ALLOC(a, Iron_Type);
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->kind            = IRON_TYPE_INTERFACE;
    t->interface.decl  = decl;
    return t;
}

Iron_Type *iron_type_make_enum(Iron_Arena *a, struct Iron_EnumDecl *decl) {
    Iron_Type *t = ARENA_ALLOC(a, Iron_Type);
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->kind     = IRON_TYPE_ENUM;
    t->enu.decl = decl;
    return t;
}

Iron_Type *iron_type_make_generic_param(Iron_Arena *a, const char *name, Iron_Type *constraint) {
    Iron_Type *t = ARENA_ALLOC(a, Iron_Type);
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->kind                    = IRON_TYPE_GENERIC_PARAM;
    t->generic_param.name       = name;
    t->generic_param.constraint = constraint;
    return t;
}

/* ── Structural equality ─────────────────────────────────────────────────── */

bool iron_type_equals(const Iron_Type *a, const Iron_Type *b) {
    if (a == b) return true;          /* pointer equality covers all interned primitives */
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        /* Interned primitives: pointer equality already handled above.
         * If we arrive here with matching kind, they must be the same singleton
         * because iron_type_make_primitive always returns the same pointer.
         * However, we compare structural fields for robustness. */
        case IRON_TYPE_INT:   case IRON_TYPE_INT8:  case IRON_TYPE_INT16:
        case IRON_TYPE_INT32: case IRON_TYPE_INT64:
        case IRON_TYPE_UINT:  case IRON_TYPE_UINT8: case IRON_TYPE_UINT16:
        case IRON_TYPE_UINT32: case IRON_TYPE_UINT64:
        case IRON_TYPE_FLOAT: case IRON_TYPE_FLOAT32: case IRON_TYPE_FLOAT64:
        case IRON_TYPE_BOOL:  case IRON_TYPE_STRING:
        case IRON_TYPE_VOID:  case IRON_TYPE_NULL:  case IRON_TYPE_ERROR:
            return true; /* same kind == same primitive */

        case IRON_TYPE_NULLABLE:
            return iron_type_equals(a->nullable.inner, b->nullable.inner);

        case IRON_TYPE_RC:
            return iron_type_equals(a->rc.inner, b->rc.inner);

        case IRON_TYPE_ARRAY:
            return a->array.size == b->array.size &&
                   iron_type_equals(a->array.elem, b->array.elem);

        case IRON_TYPE_FUNC: {
            if (a->func.param_count != b->func.param_count) return false;
            if (!iron_type_equals(a->func.return_type, b->func.return_type)) return false;
            for (int i = 0; i < a->func.param_count; i++) {
                if (!iron_type_equals(a->func.param_types[i], b->func.param_types[i]))
                    return false;
            }
            return true;
        }

        case IRON_TYPE_OBJECT:
            return a->object.decl == b->object.decl;

        case IRON_TYPE_INTERFACE:
            return a->interface.decl == b->interface.decl;

        case IRON_TYPE_ENUM:
            if (a->enu.decl != b->enu.decl) return false;
            if (a->enu.type_arg_count != b->enu.type_arg_count) return false;
            for (int i = 0; i < a->enu.type_arg_count; i++) {
                if (!iron_type_equals(a->enu.type_args[i], b->enu.type_args[i]))
                    return false;
            }
            return true;

        case IRON_TYPE_GENERIC_PARAM: {
            if (!a->generic_param.name || !b->generic_param.name) return false;
            return strcmp(a->generic_param.name, b->generic_param.name) == 0 &&
                   iron_type_equals(a->generic_param.constraint, b->generic_param.constraint);
        }
    }
    return false;
}

/* ── Type to string ──────────────────────────────────────────────────────── */

const char *iron_type_to_string(const Iron_Type *t, Iron_Arena *a) {
    if (!t) return "<null>";

    switch (t->kind) {
        case IRON_TYPE_INT:     return "Int";
        case IRON_TYPE_INT8:    return "Int8";
        case IRON_TYPE_INT16:   return "Int16";
        case IRON_TYPE_INT32:   return "Int32";
        case IRON_TYPE_INT64:   return "Int64";
        case IRON_TYPE_UINT:    return "UInt";
        case IRON_TYPE_UINT8:   return "UInt8";
        case IRON_TYPE_UINT16:  return "UInt16";
        case IRON_TYPE_UINT32:  return "UInt32";
        case IRON_TYPE_UINT64:  return "UInt64";
        case IRON_TYPE_FLOAT:   return "Float";
        case IRON_TYPE_FLOAT32: return "Float32";
        case IRON_TYPE_FLOAT64: return "Float64";
        case IRON_TYPE_BOOL:    return "Bool";
        case IRON_TYPE_STRING:  return "String";
        case IRON_TYPE_VOID:    return "Void";
        case IRON_TYPE_NULL:    return "Null";
        case IRON_TYPE_ERROR:   return "<error>";

        case IRON_TYPE_NULLABLE: {
            const char *inner = iron_type_to_string(t->nullable.inner, a);
            size_t len = strlen(inner) + 2; /* + '?' + '\0' */
            char *buf = (char *)iron_arena_alloc(a, len, 1);
            if (!buf) return "<null>?";
            snprintf(buf, len, "%s?", inner);
            return buf;
        }

        case IRON_TYPE_RC: {
            const char *inner = iron_type_to_string(t->rc.inner, a);
            size_t len = strlen(inner) + 4; /* "rc " + inner + '\0' */
            char *buf = (char *)iron_arena_alloc(a, len, 1);
            if (!buf) return "rc <null>";
            snprintf(buf, len, "rc %s", inner);
            return buf;
        }

        case IRON_TYPE_ARRAY: {
            const char *elem = iron_type_to_string(t->array.elem, a);
            /* Build into arena directly; estimate max size generously */
            size_t elem_len = strlen(elem);
            size_t buf_size = elem_len + 32; /* "[" + elem + "; " + number + "]" + NUL */
            char *buf = (char *)iron_arena_alloc(a, buf_size, 1);
            if (!buf) return "[...]";
            if (t->array.size < 0) {
                snprintf(buf, buf_size, "[%s]", elem);
            } else {
                snprintf(buf, buf_size, "[%s; %d]", elem, t->array.size);
            }
            return buf;
        }

        case IRON_TYPE_FUNC: {
            /* Build "func(T1, T2, ...) -> R" */
            char buf[512];
            int  pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "func(");
            for (int i = 0; i < t->func.param_count; i++) {
                if (i > 0)
                    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ", ");
                const char *ps = iron_type_to_string(t->func.param_types[i], a);
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s", ps);
            }
            const char *rs = iron_type_to_string(t->func.return_type, a);
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ") -> %s", rs);
            size_t len = (size_t)pos + 1;
            char *out = (char *)iron_arena_alloc(a, len, 1);
            if (!out) return "func(...)";
            memcpy(out, buf, len);
            return out;
        }

        case IRON_TYPE_OBJECT:
            /* Return name if available through decl; fallback */
            return "<object>";

        case IRON_TYPE_INTERFACE:
            return "<interface>";

        case IRON_TYPE_ENUM: {
            if (!t->enu.decl) return "<enum>";
            if (t->enu.type_arg_count == 0) return t->enu.decl->name;
            /* Generic enum: build "Option[Int]" style string */
            char buf[512];
            int pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s[", t->enu.decl->name);
            for (int i = 0; i < t->enu.type_arg_count; i++) {
                if (i > 0)
                    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ", ");
                const char *as = iron_type_to_string(t->enu.type_args[i], a);
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s", as);
            }
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]");
            size_t len = (size_t)pos + 1;
            char *out = (char *)iron_arena_alloc(a, len, 1);
            if (!out) return t->enu.decl->name;
            memcpy(out, buf, len);
            return out;
        }

        case IRON_TYPE_GENERIC_PARAM:
            return t->generic_param.name ? t->generic_param.name : "<T>";
    }
    return "<unknown>";
}

/* ── Type predicates ─────────────────────────────────────────────────────── */

bool iron_type_is_integer(const Iron_Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case IRON_TYPE_INT:   case IRON_TYPE_INT8:  case IRON_TYPE_INT16:
        case IRON_TYPE_INT32: case IRON_TYPE_INT64:
        case IRON_TYPE_UINT:  case IRON_TYPE_UINT8: case IRON_TYPE_UINT16:
        case IRON_TYPE_UINT32: case IRON_TYPE_UINT64:
            return true;
        default:
            return false;
    }
}

bool iron_type_is_float(const Iron_Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case IRON_TYPE_FLOAT:
        case IRON_TYPE_FLOAT32:
        case IRON_TYPE_FLOAT64:
            return true;
        default:
            return false;
    }
}

bool iron_type_is_numeric(const Iron_Type *t) {
    return iron_type_is_integer(t) || iron_type_is_float(t);
}
