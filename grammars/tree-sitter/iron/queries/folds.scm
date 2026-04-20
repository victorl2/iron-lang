; Phase 6 Plan 06-01 Task 3 — folds.scm skeleton. Plan 06-02 expands
; with (object_declaration) / (interface_declaration) / (func_declaration
; body: ...) / (block) / (match_expression) folds once the grammar has
; structured declaration rules. For v0.1, only the two unambiguous
; multi-line constructs fold.

(block_comment) @fold
(interpolated_string) @fold
