; Phase 6 Plan 06-01 Task 3 (EXT-02, UI-SPEC S7). Tree-sitter highlight
; queries using Neovim + Zed's conventional capture names. Plan 06-02
; expands this file with operator / keyword / function / type captures
; after the grammar.js _top permissive rule is replaced with
; _declaration + full statement/expression rules.
;
; Capture naming reference:
;   https://github.com/nvim-treesitter/nvim-treesitter/blob/master/CONTRIBUTING.md

; --- comments ---
(line_comment) @comment
(block_comment) @comment
(doc_comment) @comment.documentation

; --- literals ---
(integer_literal) @number
(float_literal) @number.float

(string_literal) @string
(escape_sequence) @string.escape

(interpolated_string) @string.special
(interpolation) @punctuation.special

; --- identifiers ---
; General identifier capture. Plan 06-02 adds finer-grained captures
; (function, type, variable.parameter, variable.member) once the
; declaration/expression rules exist.
(identifier) @variable
