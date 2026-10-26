#ifndef IRON_ANALYZER_EXPAND_H
#define IRON_ANALYZER_EXPAND_H

#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

/* Expand type reflection that has a concrete object type. Generic templates
 * are left intact and are expanded after monomorphisation. Returns the number
 * of compile-time statements removed or replaced. */
int iron_expand_reflection(Iron_Program *program, Iron_Arena *arena,
                           Iron_DiagList *diags);

/* Reflected metadata from comments (#652): the documentation run above an
 * object or field (NULL when there is none) and the value of its
 * `//key:value` directive for `key` (NULL when absent, "" when written
 * `//key:`). A key written more than once gives its first value. */
const char *iron_reflect_field_doc(const Iron_Field *field);
const char *iron_reflect_field_tag(const Iron_Field *field, const char *key);
const char *iron_reflect_object_doc(const Iron_ObjectDecl *object);
const char *iron_reflect_object_tag(const Iron_ObjectDecl *object,
                                    const char *key);

#endif
