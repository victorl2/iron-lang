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

#endif /* IRON_LSP_FACADE_BUILTIN_MEMBERS_H */
