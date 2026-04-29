; Phase 6 Plan 06-02 (EXT-02, UI-SPEC S7). Tree-sitter highlight queries.
;
; Canonical nvim-treesitter capture names — no @iron.* custom groups.
; Reference: https://github.com/nvim-treesitter/nvim-treesitter/blob/master/CONTRIBUTING.md
;
; Capture order matters: more specific nodes appear FIRST so first-win
; dispatch routes leaf tokens to their narrowest category.

; ── Function / method calls (specific first) ──────────────────────────
(call_expression
  function: (member_expression
    property: (identifier) @function.method.call))
(call_expression
  function: (identifier) @function.call)

; ── Declaration-site captures ─────────────────────────────────────────
(func_declaration name: (identifier) @function)
(method_declaration name: (identifier) @function.method)
(extern_func_declaration name: (identifier) @function)
(method_signature name: (identifier) @function.method)

(object_declaration    name: (identifier) @type.definition)
(interface_declaration name: (identifier) @type.definition)
(enum_declaration      name: (identifier) @type.definition)
(enum_variant          name: (identifier) @constructor)

(parameter name: (identifier) @variable.parameter)
(val_declaration binding: (identifier) @variable)
(var_declaration binding: (identifier) @variable)
(field_declaration name: (identifier) @variable.member)
(for_statement variable: (identifier) @variable)

; ── Types ─────────────────────────────────────────────────────────────
(type_identifier (identifier) @type)
(parameter type: (_) @type)
(field_declaration type: (_) @type)
(func_declaration return_type: (_) @type)
(method_declaration return_type: (_) @type)
(extern_func_declaration return_type: (_) @type)
(method_signature return_type: (_) @type)

; ── Field access ──────────────────────────────────────────────────────
(member_expression property: (identifier) @variable.member)

; ── Patterns (variant constructor in match arms) ──────────────────────
(variant_pattern variant: (identifier) @constructor)

; ── Self ──────────────────────────────────────────────────────────────
(self_expression) @variable.builtin

; ── Keywords (per-role buckets; nvim-treesitter canonical groups) ─────
; Anonymous keyword tokens must be referenced inside a structural node
; context OR via the bracket `[...]` form. Tokens declared only as the
; full rule body (e.g. `break_statement: $ => 'break'`) surface as node
; types, not anonymous terminals — those use the node-kind form below.
(break_statement) @keyword.return
(continue_statement) @keyword.return
"return" @keyword.return

["if" "elif" "else" "match" "while" "for" "in" "defer"] @keyword.control
"func" @keyword.function
["object" "interface" "enum"] @keyword.type
"import" @keyword.import
["val" "var"] @keyword

; ── v3 modifier rules (named structural nodes per D-01) ───────────────
(visibility_modifier)    @keyword
(mutation_tier_modifier) @keyword.modifier
(param_mut_modifier)     @keyword.modifier

; ── v3 init/patch keywords (anonymous-token-in-context per D-20) ──────
(init_declaration  "init"  @keyword)
(patch_declaration "patch" @keyword)
["impl" "extends"] @keyword
["extern" "comptime" "spawn" "parallel"] @keyword
(heap_expression "heap" @keyword)
(rc_expression "rc" @keyword)
(await_expression "await" @keyword)
(comptime_expression "comptime" @keyword)
["and" "or" "not" "is"] @keyword.operator

; ── Literals ──────────────────────────────────────────────────────────
(boolean_literal) @boolean
(null_literal)    @constant.builtin
(integer_literal) @number
(float_literal)   @number.float
(string_literal)  @string
(escape_sequence) @string.escape

; Interpolation braces route to @punctuation.special; the expression inside
; is rendered by nested captures (identifier/call_expression/etc.).
(interpolation "{" @punctuation.special)
(interpolation "}" @punctuation.special)

; ── Operators ─────────────────────────────────────────────────────────
["+" "-" "*" "/" "%" "==" "!=" "<" ">" "<=" ">=" "&&" "||" "!" "="
 "+=" "-=" "*=" "/=" "<<" ">>" "<<=" ">>=" "&" "|" "^" "~" "&=" "|=" "^="]
  @operator

; ── Punctuation ───────────────────────────────────────────────────────
["{" "}" "[" "]" "(" ")"] @punctuation.bracket
["," ";" ":" "." "->"] @punctuation.delimiter

; ── Comments ──────────────────────────────────────────────────────────
(line_comment) @comment
(doc_comment)  @comment.documentation
