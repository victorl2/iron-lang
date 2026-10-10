; Tree-sitter highlight queries for Iron.
;
; Capture names follow nvim-treesitter's standard groups (Neovim 0.10+ links
; each one to a default highlight group). Zed reads a copy of this file
; (editors/zed/languages/iron/highlights.scm, written by
; scripts/sync-editor-queries.sh) with a few captures renamed to Zed's theme
; keys.
;
; Order matters: when several patterns capture the same node, the LAST one
; wins in both Neovim and Zed. Generic patterns come first, specific ones
; after them.

; ── Identifiers (generic) ─────────────────────────────────────────────
(identifier) @variable

; Capitalized names are types and constructors by convention.
((identifier) @type
  (#match? @type "^[A-Z]"))

; ALL_CAPS names are constants.
((identifier) @constant
  (#match? @constant "^[A-Z][A-Z0-9_]+$"))

; ── Field access ──────────────────────────────────────────────────────
(member_expression property: (identifier) @variable.member)

; ── Function / method calls ───────────────────────────────────────────
(call_expression
  function: (identifier) @function.call)
(call_expression
  function: (member_expression
    property: (identifier) @function.method.call))

; Point(1, 2): a call to a capitalized name constructs an object.
(call_expression
  function: (identifier) @constructor
  (#match? @constructor "^[A-Z]"))

; ── Declaration sites ─────────────────────────────────────────────────
(func_declaration name: (identifier) @function)
(method_declaration name: (identifier) @function.method)
(block_method_declaration name: (identifier) @function.method)
(extern_func_declaration name: (identifier) @function)
(method_signature name: (identifier) @function.method)
(init_declaration name: (identifier) @constructor)
(method_declaration type_name: (identifier) @type)

(object_declaration    name: (identifier) @type.definition)
(interface_declaration name: (identifier) @type.definition)
(enum_declaration      name: (identifier) @type.definition)
(patch_declaration   target: (identifier) @type)
(object_declaration  parent: (identifier) @type)
(object_declaration  implements: (identifier) @type)
(patch_declaration   implements: (identifier) @type)
(enum_variant          name: (identifier) @constructor)

(parameter name: (identifier) @variable.parameter)
(field_declaration name: (identifier) @variable.member)
(test_declaration name: (string_literal) @string.special)

; ── Types ─────────────────────────────────────────────────────────────
(type_identifier (identifier) @type)
(generic_params (identifier) @type)
(generic_expression name: (identifier) @type)
(type_pattern type: (identifier) @type)
(method_declaration elem_type: (identifier) @type)

((type_identifier (identifier) @type.builtin)
  (#match? @type.builtin "^(Int|Int8|Int16|Int32|Int64|UInt|UInt8|UInt16|UInt32|UInt64|Float|Float32|Float64|Bool|String|Char|Byte|Void)$"))

; ── Patterns ──────────────────────────────────────────────────────────
(variant_pattern enum_name: (identifier) @type)
(variant_pattern variant: (identifier) @constructor)
(wildcard_pattern) @character.special

; ── Self ──────────────────────────────────────────────────────────────
(self_expression) @variable.builtin

; ── Keywords ──────────────────────────────────────────────────────────
(break_statement) @keyword.repeat
(continue_statement) @keyword.repeat
"return" @keyword.return

["if" "elif" "else" "match"] @keyword.conditional
["while" "for"] @keyword.repeat
(for_statement "in" @keyword.repeat)
(arena_block "in" @keyword)
"func" @keyword.function
["object" "interface" "enum" "patch"] @keyword.type
"import" @keyword.import
(import_declaration "as" @keyword.import)
["val" "var"] @keyword
"init" @keyword.function
"test" @keyword
["defer" "free" "leak" "drop" "copy"] @keyword
["impl" "implements" "extends"] @keyword
["extern" "comptime" "spawn" "parallel" "heap" "rc" "weak" "await"] @keyword
["unchecked" "layout" "unordered"] @keyword.modifier
(heap_option key: "in" @keyword)
["and" "or" "not" "is"] @keyword.operator

(visibility_modifier)    @keyword.modifier
(mutation_tier_modifier) @keyword.modifier
(param_mut_modifier)     @keyword.modifier
(nocopy_modifier)        @keyword.modifier
(func_declaration "@" @attribute "fusible" @attribute)

; ── Literals ──────────────────────────────────────────────────────────
(boolean_literal) @boolean
(null_literal)    @constant.builtin
(integer_literal) @number
(float_literal)   @number.float
(string_literal)  @string
(escape_sequence) @string.escape

; ── Operators ─────────────────────────────────────────────────────────
["+" "-" "*" "/" "%" "==" "!=" "<" ">" "<=" ">=" "&&" "||" "!" "="
 "+=" "-=" "*=" "/=" "<<" ">>" "<<=" ">>=" "&" "|" "^" "~" "&=" "|=" "^="
 ".." "?"]
  @operator

; ── Punctuation ───────────────────────────────────────────────────────
["{" "}" "[" "]" "(" ")"] @punctuation.bracket
["," ";" ":" "." "->"] @punctuation.delimiter

; Interpolation braces (after the generic brackets so they win). The
; expression inside keeps its own captures.
(interpolation "{" @punctuation.special)
(interpolation "}" @punctuation.special)

; ── Comments ──────────────────────────────────────────────────────────
(line_comment) @comment
(doc_comment)  @comment.documentation
