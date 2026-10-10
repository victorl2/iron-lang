; Vim-mode text objects in Zed (af / if, ac / ic, gc).

(func_declaration
  body: (block "{" (_)* @function.inside "}")) @function.around
(method_declaration
  body: (block "{" (_)* @function.inside "}")) @function.around
(block_method_declaration
  body: (block "{" (_)* @function.inside "}")) @function.around
(init_declaration
  body: (block "{" (_)* @function.inside "}")) @function.around
(lambda_expression
  body: (block "{" (_)* @function.inside "}")) @function.around
(test_declaration
  body: (block "{" (_)* @function.inside "}")) @function.around

(object_declaration "{" (_)* @class.inside "}") @class.around
(patch_declaration "{" (_)* @class.inside "}") @class.around
(interface_declaration "{" (_)* @class.inside "}") @class.around
(enum_declaration "{" (_)* @class.inside "}") @class.around

(line_comment)+ @comment.around
(doc_comment)+ @comment.around
