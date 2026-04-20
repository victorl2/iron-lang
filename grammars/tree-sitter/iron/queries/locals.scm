; Phase 6 Plan 06-01 Task 3 — locals.scm skeleton. Plan 06-02 expands
; with proper scope queries once declaration / block / parameter rules
; land in grammar.js. For now the permissive _top rule only exposes
; source_file as a scope boundary; identifier uses are all marked as
; references so default Neovim / Zed behaviours degrade gracefully.

; Top-level file scope
(source_file) @local.scope

; All identifiers treated as references in v0.1 — proper @local.definition
; captures arrive in Plan 06-02 once (func_declaration name: ...) +
; (val_declaration name: ...) + (parameter name: ...) rules exist.
(identifier) @local.reference
