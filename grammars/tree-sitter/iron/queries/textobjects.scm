; Text objects for nvim-treesitter-textobjects (af / if, ac / ic, ...).

(func_declaration) @function.outer
(func_declaration body: (block) @function.inner)
(method_declaration) @function.outer
(method_declaration body: (block) @function.inner)
(block_method_declaration) @function.outer
(block_method_declaration body: (block) @function.inner)
(init_declaration) @function.outer
(init_declaration body: (block) @function.inner)
(lambda_expression) @function.outer
(lambda_expression body: (block) @function.inner)
(test_declaration) @function.outer
(test_declaration body: (block) @function.inner)

[
  (object_declaration)
  (patch_declaration)
  (interface_declaration)
  (enum_declaration)
] @class.outer

(parameter) @parameter.inner
(parameter) @parameter.outer

(call_expression) @call.outer

[
  (if_statement)
  (match_statement)
] @conditional.outer

[
  (while_statement)
  (for_statement)
] @loop.outer
(while_statement body: (block) @loop.inner)
(for_statement body: (block) @loop.inner)

(return_statement) @return.outer

[
  (line_comment)
  (doc_comment)
] @comment.outer
