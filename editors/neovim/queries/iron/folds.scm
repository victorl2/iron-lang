; Tree-sitter fold queries: fold ranges for the container rules so Neovim
; can collapse declarations and blocks.

(func_declaration)         @fold
(method_declaration)       @fold
(block_method_declaration) @fold
(extern_func_declaration)  @fold
(method_signature)         @fold
(init_declaration)         @fold
(lambda_expression)        @fold
(object_declaration)       @fold
(patch_declaration)        @fold
(interface_declaration)    @fold
(enum_declaration)         @fold
(test_declaration)         @fold
(drop_block)               @fold
(copy_block)               @fold
(block)                    @fold
(match_statement)          @fold
(arena_block)              @fold
(spawn_expression)         @fold
(defer_statement)          @fold
(array_literal)            @fold
(parameter_list)           @fold
(string_literal)           @fold
