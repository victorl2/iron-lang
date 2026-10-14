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

/* Stable bridge for the slash-comment stage (#652). Until that AST lands on
 * main, reflection deliberately observes no documentation or directives. */
const char *iron_reflect_field_doc(const Iron_Field *field);
const char *iron_reflect_field_tag(const Iron_Field *field, const char *key);
const char *iron_reflect_object_doc(const Iron_ObjectDecl *object);
const char *iron_reflect_object_tag(const Iron_ObjectDecl *object,
                                    const char *key);

#endif
