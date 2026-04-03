/* iron_collections.c — Pre-instantiated collection implementations
 *
 * This file provides the function bodies for the most common monomorphized
 * collection types.  The Iron compiler's monomorphization pass (gen_types.c
 * ensure_monomorphized_type) emits struct typedefs + prototype stubs.  These
 * IRON_*_IMPL macro expansions provide the matching definitions.
 *
 * Naming follows gen_types.c mangle_generic() exactly:
 *   List[Int]        -> Iron_List_int64_t
 *   Map[String, Int] -> Iron_Map_Iron_String_int64_t
 *   Set[Int]         -> Iron_Set_int64_t
 *
 * For new element types the caller can expand the macros in any .c file that
 * includes iron_runtime.h — only one TU may do so per type to avoid ODR
 * violations.
 */

#include <stdlib.h>
#include <string.h>

#include "runtime/iron_runtime.h"

/* ── Equality helpers ────────────────────────────────────────────────────── */

static bool iron_string_eq_ptr(const Iron_String *a, const Iron_String *b) {
    return iron_string_equals(a, b);
}

static bool int64_eq_ptr(const int64_t *a, const int64_t *b) {
    return *a == *b;
}

/* ── List implementations ─────────────────────────────────────────────────── */

IRON_LIST_IMPL(int64_t,     int64_t)
IRON_LIST_IMPL(int32_t,     int32_t)
IRON_LIST_IMPL(double,      double)
IRON_LIST_IMPL(bool,        bool)
IRON_LIST_IMPL(Iron_String, Iron_String)
IRON_LIST_IMPL(Iron_Closure, Iron_Closure)

/* ── Map implementations ──────────────────────────────────────────────────── */

IRON_MAP_IMPL(Iron_String, int64_t,     Iron_String, int64_t,     iron_string_eq_ptr)
IRON_MAP_IMPL(Iron_String, Iron_String, Iron_String, Iron_String, iron_string_eq_ptr)

/* ── Set implementations ──────────────────────────────────────────────────── */

IRON_SET_IMPL(int64_t,     int64_t,     int64_eq_ptr)
IRON_SET_IMPL(Iron_String, Iron_String, iron_string_eq_ptr)
