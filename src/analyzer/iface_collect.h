#ifndef IRON_IFACE_COLLECT_H
#define IRON_IFACE_COLLECT_H

#include "parser/ast.h"
#include "analyzer/types.h"
#include "util/arena.h"
#include <stdbool.h>

/* ── Interface Implementor Registry ─────────────────────────────────────── */

/* A single implementor of an interface. */
typedef struct {
    const char          *type_name;    /* concrete object type name */
    int                  tag;          /* canonical tag number (alphabetical) */
    Iron_ObjectDecl     *decl;         /* AST declaration */
    Iron_Type           *type;         /* resolved type */
    bool                 is_alive;     /* false if never instantiated (dead) */
} Iron_IfaceImpl;

/* All implementors of a single interface. */
typedef struct {
    const char          *iface_name;   /* interface name */
    Iron_InterfaceDecl  *iface_decl;   /* AST declaration */
    Iron_Type           *iface_type;   /* resolved type */
    Iron_IfaceImpl      *impls;        /* stb_ds array of implementors, sorted alphabetically */
    int                  impl_count;   /* number of implementors */
    int                  alive_count;  /* number of live (instantiated) implementors */
} Iron_IfaceEntry;

/* stb_ds string-keyed hashmap entry */
typedef struct {
    char            *key;    /* interface name */
    Iron_IfaceEntry  value;  /* entry data */
} Iron_IfaceMapEntry;

/* The registry: maps interface name → IfaceEntry.
 * Uses stb_ds string-keyed hash map internally. */
typedef struct {
    Iron_IfaceMapEntry  *map;     /* stb_ds shmap */
    Iron_Arena          *arena;
} Iron_IfaceRegistry;

/* ── API ────────────────────────────────────────────────────────────────── */

/* Build the registry by scanning all declarations in the program.
 * 1. Finds all interface declarations
 * 2. Finds all object declarations with `impl` clauses
 * 3. Assigns canonical tag numbers (alphabetical by type name)
 * 4. Returns the populated registry
 */
Iron_IfaceRegistry iron_iface_collect(Iron_Program *program, Iron_Arena *arena);

/* Look up an interface entry by name. Returns NULL if not found. */
Iron_IfaceEntry *iron_iface_lookup(Iron_IfaceRegistry *reg, const char *iface_name);

/* Mark implementors as dead if they are never instantiated in the program.
 * This is dead implementor elimination — prunes the union. */
void iron_iface_eliminate_dead(Iron_IfaceRegistry *reg, Iron_Program *program);

/* Get the number of live implementors for an interface. */
int iron_iface_alive_count(Iron_IfaceEntry *entry);

/* Get the total number of interfaces in the registry. */
int iron_iface_count(Iron_IfaceRegistry *reg);

#endif /* IRON_IFACE_COLLECT_H */
