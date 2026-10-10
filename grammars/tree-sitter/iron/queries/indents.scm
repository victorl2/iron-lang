; Tree-sitter indent queries for Iron, in nvim-treesitter's format
; (@indent.begin / @indent.branch / @indent.end). Zed has its own copy in
; editors/zed/languages/iron/indents.scm.

[
  (block)
  (object_declaration)
  (patch_declaration)
  (interface_declaration)
  (enum_declaration)
  (match_statement)
  (array_literal)
  (parameter_list)
  (call_expression)
  (tuple_expression)
  (heap_options)
] @indent.begin

[
  "}"
  "]"
  ")"
] @indent.branch

[
  "}"
  "]"
  ")"
] @indent.end

[
  (line_comment)
  (doc_comment)
] @indent.auto

(string_literal) @indent.ignore
