/* emit_structs.h -- Struct body, tagged union, and type declaration emission.
 *
 * This module handles all type declaration emission: forward declarations,
 * topologically sorted struct bodies, interface tagged union types, wrapping
 * constructors, split collection structs, push/free functions, enum
 * definitions, and ADT enum layouts.
 *
 * The main entry point, emit_type_decls(), is called from iron_lir_emit_c()
 * as a single orchestrator that writes into ctx->forward_decls,
 * ctx->struct_bodies, ctx->enum_defs, and ctx->prototypes.
 */

#ifndef IRON_EMIT_STRUCTS_H
#define IRON_EMIT_STRUCTS_H

#include "lir/emit_helpers.h"

/* Emit all type declarations: forward decls, struct bodies (topo-sorted),
 * tagged union types, wrapping constructors, split collection structs,
 * push/free functions, and interface dispatch functions.
 * Writes to ctx->forward_decls, ctx->struct_bodies, ctx->prototypes,
 * and ctx->lifted_funcs. */
void emit_type_decls(EmitCtx *ctx);

/* Estimate size of a concrete type in bytes (for variant split decisions). */
int emit_estimate_type_size(Iron_ObjectDecl *od);

#endif /* IRON_EMIT_STRUCTS_H */
