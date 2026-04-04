#ifndef IRON_TYPES_H
#define IRON_TYPES_H

#include "util/arena.h"
#include <stdbool.h>

/* Forward-declare AST declaration types to avoid circular includes. */
struct Iron_ObjectDecl;
struct Iron_InterfaceDecl;
struct Iron_EnumDecl;

/* ── Type kind ───────────────────────────────────────────────────────────── */

typedef enum {
    /* Primitive integer types */
    IRON_TYPE_INT,
    IRON_TYPE_INT8,
    IRON_TYPE_INT16,
    IRON_TYPE_INT32,
    IRON_TYPE_INT64,

    /* Primitive unsigned integer types */
    IRON_TYPE_UINT,
    IRON_TYPE_UINT8,
    IRON_TYPE_UINT16,
    IRON_TYPE_UINT32,
    IRON_TYPE_UINT64,

    /* Primitive float types */
    IRON_TYPE_FLOAT,
    IRON_TYPE_FLOAT32,
    IRON_TYPE_FLOAT64,

    /* Other primitive types */
    IRON_TYPE_BOOL,
    IRON_TYPE_STRING,

    /* User-defined types */
    IRON_TYPE_OBJECT,
    IRON_TYPE_INTERFACE,
    IRON_TYPE_ENUM,

    /* Compound / wrapper types */
    IRON_TYPE_NULLABLE,      /* T?         */
    IRON_TYPE_FUNC,          /* func(...)->R */
    IRON_TYPE_ARRAY,         /* [T; N] or [T] */
    IRON_TYPE_RC,            /* rc T       */

    /* Meta types */
    IRON_TYPE_GENERIC_PARAM, /* generic type parameter */
    IRON_TYPE_VOID,
    IRON_TYPE_NULL,
    IRON_TYPE_ERROR          /* sentinel for error recovery */
} Iron_TypeKind;

/* ── Type node (discriminated union) ─────────────────────────────────────── */

typedef struct Iron_Type {
    Iron_TypeKind kind;

    union {
        /* IRON_TYPE_OBJECT */
        struct {
            struct Iron_ObjectDecl    *decl;
        } object;

        /* IRON_TYPE_INTERFACE */
        struct {
            struct Iron_InterfaceDecl *decl;
        } interface;

        /* IRON_TYPE_ENUM */
        struct {
            struct Iron_EnumDecl      *decl;
            struct Iron_Type        ***variant_payload_types; /* [variant_idx][payload_idx]; populated by Phase 33 type checker */
            struct Iron_Type         **type_args;             /* NULL for non-generic enums */
            int                        type_arg_count;        /* 0 for non-generic enums */
            const char                *mangled_name;          /* e.g. "Iron_Option_Int"; NULL for non-generic */
        } enu;

        /* IRON_TYPE_NULLABLE */
        struct {
            struct Iron_Type          *inner;
        } nullable;

        /* IRON_TYPE_RC */
        struct {
            struct Iron_Type          *inner;
        } rc;

        /* IRON_TYPE_FUNC */
        struct {
            struct Iron_Type         **param_types;
            int                        param_count;
            struct Iron_Type          *return_type;
        } func;

        /* IRON_TYPE_ARRAY */
        struct {
            struct Iron_Type          *elem;
            int                        size;  /* -1 = dynamic */
        } array;

        /* IRON_TYPE_GENERIC_PARAM */
        struct {
            const char                *name;
            struct Iron_Type          *constraint;  /* NULL if unconstrained */
        } generic_param;
    };
} Iron_Type;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Initialize the interned primitive singletons.  Must be called once before
 * any iron_type_make_primitive() call.  The arena is used only for
 * future heap-allocated (non-primitive) types. */
void iron_types_init(Iron_Arena *arena);

/* Return the interned singleton for a primitive type kind.
 * kind must be a primitive kind (INT, BOOL, etc.).
 * Returns NULL if kind is not a primitive. */
Iron_Type *iron_type_make_primitive(Iron_TypeKind kind);

/* Construct a nullable wrapper type T? */
Iron_Type *iron_type_make_nullable(Iron_Arena *a, Iron_Type *inner);

/* Construct an Rc<T> wrapper type */
Iron_Type *iron_type_make_rc(Iron_Arena *a, Iron_Type *inner);

/* Construct a function type func(params...) -> ret */
Iron_Type *iron_type_make_func(Iron_Arena *a, Iron_Type **params, int count, Iron_Type *ret);

/* Construct an array type [elem; size] (size == -1 for dynamic) */
Iron_Type *iron_type_make_array(Iron_Arena *a, Iron_Type *elem, int size);

/* Construct an object type backed by a declaration node */
Iron_Type *iron_type_make_object(Iron_Arena *a, struct Iron_ObjectDecl *decl);

/* Construct an interface type backed by a declaration node */
Iron_Type *iron_type_make_interface(Iron_Arena *a, struct Iron_InterfaceDecl *decl);

/* Construct an enum type backed by a declaration node */
Iron_Type *iron_type_make_enum(Iron_Arena *a, struct Iron_EnumDecl *decl);

/* Construct a generic type parameter (e.g. T with optional constraint) */
Iron_Type *iron_type_make_generic_param(Iron_Arena *a, const char *name, Iron_Type *constraint);

/* Structural equality — two types are equal iff they have the same structure.
 * Primitive singletons compare by pointer, compound types compare recursively. */
bool iron_type_equals(const Iron_Type *a, const Iron_Type *b);

/* Return a human-readable string for the type (arena-allocated).
 * Examples: "Int", "Bool", "Int?", "[Int; 5]", "func(Int, Bool) -> String"
 */
const char *iron_type_to_string(const Iron_Type *t, Iron_Arena *a);

/* Type category predicates */
bool iron_type_is_numeric(const Iron_Type *t);
bool iron_type_is_integer(const Iron_Type *t);
bool iron_type_is_float(const Iron_Type *t);

#endif /* IRON_TYPES_H */
