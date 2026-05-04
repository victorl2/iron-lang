; Phase 6 Plan 06-02 (EXT-02). Tree-sitter locals queries — scope /
; definition / reference captures enabling nvim-treesitter + Zed to
; differentiate variable declarations from uses.

; ── Scopes ────────────────────────────────────────────────────────────
(func_declaration)        @local.scope
(method_declaration)      @local.scope
(extern_func_declaration) @local.scope
(lambda_expression)       @local.scope
(block)                   @local.scope
(for_statement)           @local.scope
(match_arm)               @local.scope
(match_else_arm)          @local.scope
(defer_statement)         @local.scope
(spawn_expression)        @local.scope

; ── Definitions ───────────────────────────────────────────────────────
(parameter           name:    (identifier) @local.definition.parameter)
(val_declaration     binding: (identifier) @local.definition.var)
(var_declaration     binding: (identifier) @local.definition.var)
(for_statement       variable:(identifier) @local.definition.var)
(func_declaration    name:    (identifier) @local.definition.function)
(method_declaration  name:    (identifier) @local.definition.method)
(method_signature    name:    (identifier) @local.definition.method)
(extern_func_declaration name: (identifier) @local.definition.function)
(object_declaration  name:    (identifier) @local.definition.type)
(interface_declaration name:  (identifier) @local.definition.type)
(enum_declaration    name:    (identifier) @local.definition.type)
(enum_variant        name:    (identifier) @local.definition.enum)
(field_declaration   name:    (identifier) @local.definition.field)
(import_declaration  alias:   (identifier) @local.definition.import)

; Tuple destructure: val (x, y) = pair — bind each name.
(tuple_binding (identifier) @local.definition.var)

; ── References ────────────────────────────────────────────────────────
(identifier) @local.reference

; ── v3 scopes (per D-15) ──────────────────────────────────────────────
(init_declaration)         @local.scope
(patch_declaration)        @local.scope
(block_method_declaration) @local.scope

; ── v3 method definition (parallel to method_declaration per D-17) ────
(block_method_declaration name: (identifier) @local.definition.method)

; ── v3 patch target as a reference (per D-18) ────────────────────────
(patch_declaration target: (identifier) @local.reference)
