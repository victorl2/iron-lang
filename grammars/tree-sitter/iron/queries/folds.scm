; Phase 6 Plan 06-02 (EXT-02). Tree-sitter fold queries — fold ranges for
; all container rules so Neovim / Zed can collapse declarations + blocks.

(func_declaration)        @fold
(method_declaration)      @fold
(extern_func_declaration) @fold
(method_signature)        @fold
(lambda_expression)       @fold
(object_declaration)      @fold
(interface_declaration)   @fold
(enum_declaration)        @fold
(block)                   @fold
(match_statement)         @fold
(spawn_expression)        @fold
(defer_statement)         @fold
(array_literal)           @fold
(string_literal)          @fold
