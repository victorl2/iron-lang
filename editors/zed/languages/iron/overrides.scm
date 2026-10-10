; Scopes for config.toml's `not_in` (no auto-closed quote inside a string
; or a comment).
(string_literal) @string
[
  (line_comment)
  (doc_comment)
] @comment
