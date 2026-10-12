#ifndef IRON_LSP_FACADE_BUILTIN_MEMBERS_H
#define IRON_LSP_FACADE_BUILTIN_MEMBERS_H

/* Functions and methods the compiler provides by name, with no
 * declaration in the stdlib to read: `println`, `len`, the list
 * methods (`push`, `pop`...), and the `Map` / `Set` methods. Completion
 * lists them with these signatures and signature help shows them.
 *
 * The signatures name the element types T (list and Set item), K and V
 * (Map key and value); ilsp_builtin_signature substitutes the
 * receiver's own types for them. */

#include "analyzer/intrinsics.h"
#include "analyzer/types.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *detail;   /* `func push(item: T)` */
} IronLsp_BuiltinMember;

static const IronLsp_BuiltinMember ilsp_builtin_funcs[] = {
    { "print",     "func print(value: String)" },
    { "println",   "func println(value: String)" },
    { "len",       "func len(s: String) -> Int" },
    { "min",       "func min(a: Int, b: Int) -> Int" },
    { "max",       "func max(a: Int, b: Int) -> Int" },
    { "clamp",     "func clamp(value: Int, lo: Int, hi: Int) -> Int" },
    { "abs",       "func abs(x: Int) -> Int" },
    { "range",     "func range(n: Int) -> [Int]" },
    { "assert",    "func assert(condition: Bool, message: String)" },
    { "fill",      "func fill(count: Int, value: T) -> [T]" },
    { "read_file", "func read_file(path: String) -> String" },
    { NULL, NULL },
};

static const IronLsp_BuiltinMember ilsp_list_builtins[] = {
    { "len", "func len() -> Int" },          { "push", "func push(item: T)" },
    { "pop", "func pop() -> T" },            { "get", "func get(i: Int) -> T" },
    { "set", "func set(i: Int, item: T)" },  { "insert", "func insert(i: Int, item: T)" },
    { "remove", "func remove(i: Int) -> T" },{ "clear", "func clear()" },
    { "reverse", "func reverse()" },         { "contains", "func contains(item: T) -> Bool" },
    { "sort", "func sort()" },               { "copy", "func copy() -> [T]" },
    { "take", "func take() -> [T]" },        { NULL, NULL },
};

static const IronLsp_BuiltinMember ilsp_map_builtins[] = {
    { "put", "func put(key: K, value: V)" }, { "get", "func get(key: K) -> V" },
    { "get_or", "func get_or(key: K, default: V) -> V" },
    { "has", "func has(key: K) -> Bool" },   { "remove", "func remove(key: K) -> Bool" },
    { "len", "func len() -> Int" },          { "clear", "func clear()" },
    { "keys", "func keys() -> [K]" },        { "values", "func values() -> [V]" },
    { "copy", "func copy() -> Map[K, V]" },  { "take", "func take() -> Map[K, V]" },
    { NULL, NULL },
};

static const IronLsp_BuiltinMember ilsp_set_builtins[] = {
    { "add", "func add(item: T) -> Bool" },  { "has", "func has(item: T) -> Bool" },
    { "remove", "func remove(item: T) -> Bool" }, { "len", "func len() -> Int" },
    { "clear", "func clear()" },             { "values", "func values() -> [T]" },
    { "copy", "func copy() -> Set[T]" },     { "take", "func take() -> Set[T]" },
    { NULL, NULL },
};

static inline const char *ilsp_builtin_find(const IronLsp_BuiltinMember *table,
                                            const char *name) {
    if (!table || !name) return NULL;
    for (int i = 0; table[i].name; i++) {
        if (strcmp(table[i].name, name) == 0) return table[i].detail;
    }
    return NULL;
}

/* The builtin method table for a receiver of type `t` (handles already
 * stripped): lists, Map and Set; NULL for any other type. */
static inline const IronLsp_BuiltinMember *ilsp_builtin_table_for(const Iron_Type *t) {
    if (!t) return NULL;
    if (t->kind == IRON_TYPE_ARRAY) return ilsp_list_builtins;
    if (t->kind == IRON_TYPE_OBJECT && t->object.decl && t->object.decl->name) {
        if (strcmp(t->object.decl->name, "Map") == 0) return ilsp_map_builtins;
        if (strcmp(t->object.decl->name, "Set") == 0) return ilsp_set_builtins;
    }
    return NULL;
}

/* `detail` with the element types of `recv` written in for T / K / V
 * (`func push(item: Int)` for an [Int]); `detail` itself when `recv`
 * has none. Arena-owned. */
static inline const char *ilsp_builtin_signature(const char *detail, const Iron_Type *recv,
                                                 Iron_Arena *arena) {
    if (!detail) return NULL;
    const char *t = NULL, *k = NULL, *v = NULL;
    if (recv && recv->kind == IRON_TYPE_ARRAY && recv->array.elem) {
        t = iron_type_to_string(recv->array.elem, arena);
    } else if (recv && recv->kind == IRON_TYPE_OBJECT && recv->object.elem &&
               recv->object.decl && recv->object.decl->name) {
        /* Map[K, V]: elem is K and elem2 V; Set[T]: elem is T. */
        const char *a0 = iron_type_to_string(recv->object.elem, arena);
        if (strcmp(recv->object.decl->name, "Map") == 0) {
            k = a0;
            v = recv->object.elem2 ? iron_type_to_string(recv->object.elem2, arena) : NULL;
        } else {
            t = a0;
        }
    }
    if (!t && !k && !v) return detail;
    char buf[512];
    size_t n = 0;
    for (const char *p = detail; *p && n + 1 < sizeof(buf); p++) {
        bool alone = (p == detail || !(p[-1] == '_' || (p[-1] >= 'a' && p[-1] <= 'z') ||
                                       (p[-1] >= 'A' && p[-1] <= 'Z') ||
                                       (p[-1] >= '0' && p[-1] <= '9'))) &&
                     !(p[1] == '_' || (p[1] >= 'a' && p[1] <= 'z') ||
                       (p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= '0' && p[1] <= '9'));
        const char *sub = NULL;
        if (alone && *p == 'T') sub = t;
        else if (alone && *p == 'K') sub = k;
        else if (alone && *p == 'V') sub = v;
        if (sub) {
            size_t l = strlen(sub);
            if (n + l + 1 >= sizeof(buf)) break;
            memcpy(buf + n, sub, l);
            n += l;
        } else {
            buf[n++] = *p;
        }
    }
    buf[n] = '\0';
    return iron_arena_strdup(arena, buf, n);
}

/* Intrinsic receiver categories shared with the type checker. */
static inline unsigned ilsp_intrinsic_receiver(const Iron_Type *t) {
    if (!t) return 0;
    if (iron_type_is_integer(t)) {
        bool is_signed = t->kind >= IRON_TYPE_INT && t->kind <= IRON_TYPE_INT64;
        return IRON_INTR_RECV_INT | (is_signed ? IRON_INTR_RECV_SIGNED : 0);
    }
    if (iron_type_is_float(t)) return IRON_INTR_RECV_FLOAT;
    if (t->kind == IRON_TYPE_STRING) return IRON_INTR_RECV_STRING;
    if (t->kind == IRON_TYPE_ARRAY) return IRON_INTR_RECV_LIST;
    if (t->kind == IRON_TYPE_VIEW) {
        unsigned result = IRON_INTR_RECV_VIEW;
        if (t->view.is_mut) result |= IRON_INTR_RECV_VIEW_MUT;
        if (iron_type_is_bytes(t)) result |= IRON_INTR_RECV_BYTES;
        return result;
    }
    if (t->kind == IRON_TYPE_OBJECT && t->object.decl && t->object.decl->name) {
        if (strcmp(t->object.decl->name, "Hint") == 0) return IRON_INTR_RECV_HINT;
        if (strcmp(t->object.decl->name, "StringBuilder") == 0) return IRON_INTR_RECV_SB;
    }
    return 0;
}

static inline unsigned ilsp_static_intrinsic_receiver(const char *name) {
    if (!name) return 0;
    if (strcmp(name, "Hint") == 0) return IRON_INTR_RECV_HINT;
    if (strcmp(name, "String") == 0) return IRON_INTR_RECV_STRING;
    if (strcmp(name, "StringBuilder") == 0) return IRON_INTR_RECV_SB;
    if (strcmp(name, "Float") == 0 || strcmp(name, "Float32") == 0 ||
        strcmp(name, "Float64") == 0) return IRON_INTR_RECV_FLOAT;
    if (strcmp(name, "Int") == 0 || strcmp(name, "Int8") == 0 ||
        strcmp(name, "Int16") == 0 || strcmp(name, "Int32") == 0 ||
        strcmp(name, "Int64") == 0) return IRON_INTR_RECV_INT | IRON_INTR_RECV_SIGNED;
    return 0;
}

static inline const Iron_Type *ilsp_primitive_type_named(const char *name) {
    static const char *const names[] = {
        "Int", "Int8", "Int16", "Int32", "Int64", "UInt", "UInt8", "UInt16",
        "UInt32", "UInt64", "Float", "Float32", "Float64",
    };
    if (!name) return NULL;
    for (int i = 0; i <= IRON_TYPE_FLOAT64; i++)
        if (strcmp(name, names[i]) == 0) return iron_type_make_primitive((Iron_TypeKind)i);
    return NULL;
}

static inline const char *ilsp_unsigned_type_name(const Iron_Type *t) {
    if (!t) return NULL;
    switch ((int)t->kind) {
        case IRON_TYPE_INT: case IRON_TYPE_INT64:
        case IRON_TYPE_FLOAT: case IRON_TYPE_FLOAT64: return "UInt64";
        case IRON_TYPE_INT8: return "UInt8";
        case IRON_TYPE_INT16: return "UInt16";
        case IRON_TYPE_INT32: case IRON_TYPE_FLOAT32: return "UInt32";
        case IRON_TYPE_UINT: return "UInt";
        case IRON_TYPE_UINT8: return "UInt8";
        case IRON_TYPE_UINT16: return "UInt16";
        case IRON_TYPE_UINT32: return "UInt32";
        case IRON_TYPE_UINT64: return "UInt64";
        default: return NULL;
    }
}

/* Substitute the T, U, E and Self placeholders used by intrinsics.h. */
static inline const char *ilsp_intrinsic_signature(const Iron_IntrinsicInfo *info,
                                                    const Iron_Type *recv,
                                                    Iron_Arena *arena) {
    if (!info || !info->signature) return NULL;
    const Iron_Type *elem = NULL;
    if (recv && recv->kind == IRON_TYPE_ARRAY) elem = recv->array.elem;
    else if (recv && recv->kind == IRON_TYPE_VIEW) elem = recv->view.elem;
    const char *self = recv ? iron_type_to_string(recv, arena) : NULL;
    const char *e = elem ? iron_type_to_string(elem, arena) : NULL;
    const char *u = ilsp_unsigned_type_name(recv);
    char buf[512];
    size_t n = 0;
    for (const char *p = info->signature; *p && n + 1 < sizeof(buf);) {
        const char *start = p;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_') {
            p++;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                   (*p >= '0' && *p <= '9') || *p == '_') p++;
        } else {
            p++;
        }
        size_t len = (size_t)(p - start);
        const char *sub = NULL;
        if (len == 1 && start[0] == 'T') sub = self;
        else if (len == 1 && start[0] == 'U') sub = u;
        else if (len == 1 && start[0] == 'E') sub = e;
        else if (len == 4 && memcmp(start, "Self", 4) == 0) sub = self;
        if (sub) {
            len = strlen(sub);
            if (n + len + 1 >= sizeof(buf)) break;
            memcpy(buf + n, sub, len);
        } else {
            if (n + len + 1 >= sizeof(buf)) break;
            memcpy(buf + n, start, len);
        }
        n += len;
    }
    buf[n] = '\0';
    return iron_arena_strdup(arena, buf, n);
}

#endif /* IRON_LSP_FACADE_BUILTIN_MEMBERS_H */
