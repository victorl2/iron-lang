// GENERATED at configure time from src/lexer/lexer.c kw_table.
// DO NOT EDIT. Edit src/lexer/lexer.c + this .in template, then run
// scripts/regenerate-grammars.sh. The committed grammar.js is drift-gated
// via CTest test_grammar_keyword_drift_tree_sitter (dual-labelled
// phase-m5-invariant + phase-m1-invariant).
//
// Phase 6 Plan 06-02: full parseable grammar replacing the Plan 06-01 v0.1
// permissive skeleton. Covers 7 declaration kinds (import, object, interface,
// enum, func, method, val/var) + statement / expression / pattern / type
// rules sufficient to parse every tests/integration/*.iron with zero ERROR
// nodes.
//
// NOTE: Iron does NOT have a top-level `impl` declaration. The `impl`
// keyword appears ONLY as a clause inside `object Name impl I1, I2 { ... }`
// (implements list). Methods are declared as `func TypeName.method_name(...)`
// producing an IRON_NODE_METHOD_DECL. This grammar matches the parser's
// iron_parse_decl_impl dispatch in src/parser/parser.c:3217.
//
// Comment syntax (per src/lexer/lexer.c:655 and :710):
//   --   line comment (Haskell/Ada-style; Iron does NOT use //)
//   ///  doc comment
//   Iron has NO block comment.
//
// String interpolation (per src/lexer/lexer.c:287 + :350):
//   "hello {name}"     — interpolation with plain double quotes, no $ prefix
//   "literal \{ brace" — backslash-escape to suppress interpolation
//   The lexer emits IRON_TOK_INTERP_STRING as a single token; tree-sitter
//   re-scans internally via its own literal rules.
//
// Match arm syntax (per src/parser/parser.c:1878):
//   pattern -> body     — use '->' (NOT '=>')
//   else    -> body     — wildcard arm
//
// Precedence ranks mirror src/parser/parser.c PREC_* enum (lines 131-145):
//   1  ASSIGN          = / += / -= / *= / /=  (right-associative, lowest)
//   2  IS              is
//   3  OR              or  ||
//   4  AND             and &&
//   5  BIT_OR          |
//   6  BIT_XOR         ^
//   7  BIT_AND         &
//   8  EQUALITY        == !=
//   9  COMPARISON      < > <= >=
//   10 SHIFT           << >>
//   11 TERM            + -
//   12 FACTOR          * / %
//   13 UNARY           - ! ~ not  (right-associative, prefix)
//   14 CALL            . [] ()    (left-associative, suffix/member/call)
//
// Drift-guard (preserved from Plan 06-01): 'and', 'await', 'comptime', 'defer', 'elif', 'else', 'enum', 'extends', 'extern', 'false', 'for', 'free', 'func', 'heap', 'if', 'impl', 'import', 'in', 'interface', 'is', 'leak', 'match', 'not', 'null', 'object', 'or', 'parallel', 'pool', 'private', 'rc', 'return', 'self', 'spawn', 'super', 'true', 'val', 'var', 'while' expands
// to the kw_table keywords at configure time; the _keyword rule below
// still carries it so test_grammar_keyword_drift_tree_sitter remains
// meaningful.

function commaSep(rule)  { return optional(commaSep1(rule)); }
function commaSep1(rule) { return seq(rule, repeat(seq(',', rule))); }

module.exports = grammar({
  name: 'iron',

  extras: $ => [
    /[ \t\r]/,
    /\n/,
    $.line_comment,
    $.doc_comment,
  ],

  word: $ => $.identifier,

  // No explicit conflicts needed — Iron's structure is LR(1) because braces
  // and the `func` keyword delineate declaration vs expression starts. Any
  // conflict detected by tree-sitter generate should be surfaced for review
  // rather than hidden here.
  conflicts: $ => [],

  // Tokens that look like keywords (`self`, `super`, `true`, `false`, `null`)
  // are reserved by the word token rule + the _keyword alternation.
  supertypes: $ => [
    $._declaration,
    $._statement,
    $._expression,
    $._pattern,
    $._type,
  ],

  rules: {
    // ── Source file ─────────────────────────────────────────────────────
    source_file: $ => repeat($._declaration),

    _declaration: $ => choice(
      $.import_declaration,
      $.object_declaration,
      $.interface_declaration,
      $.enum_declaration,
      $.func_declaration,
      $.method_declaration,
      $.extern_func_declaration,
      $.val_declaration,
      $.var_declaration,
    ),

    // Keywords generated from src/lexer/lexer.c kw_table at configure time.
    // 'and', 'await', 'comptime', 'defer', 'elif', 'else', 'enum', 'extends', 'extern', 'false', 'for', 'free', 'func', 'heap', 'if', 'impl', 'import', 'in', 'interface', 'is', 'leak', 'match', 'not', 'null', 'object', 'or', 'parallel', 'pool', 'private', 'rc', 'return', 'self', 'spawn', 'super', 'true', 'val', 'var', 'while' expands to 'and', 'await', ..., 'while'.
    // Present so test_grammar_keyword_drift_tree_sitter has a substitution
    // target even though the full grammar inlines keyword literals in
    // concrete rules.
    _keyword: $ => choice('and', 'await', 'comptime', 'defer', 'elif', 'else', 'enum', 'extends', 'extern', 'false', 'for', 'free', 'func', 'heap', 'if', 'impl', 'import', 'in', 'interface', 'is', 'leak', 'match', 'not', 'null', 'object', 'or', 'parallel', 'pool', 'private', 'rc', 'return', 'self', 'spawn', 'super', 'true', 'val', 'var', 'while'),

    // ── Declarations ───────────────────────────────────────────────────

    // import foo.bar.baz [as Alias]
    import_declaration: $ => seq(
      'import',
      field('path', $.import_path),
      optional(seq('as', field('alias', $.identifier))),
    ),
    import_path: $ => seq($.identifier, repeat(seq('.', $.identifier))),

    // object Name[<GenericParams>] [extends Parent] [impl I1, I2] { val name: T, var name: T }
    object_declaration: $ => seq(
      'object',
      field('name', $.identifier),
      optional($.generic_params),
      optional(seq('extends', field('parent', $.identifier))),
      optional(seq('impl', field('implements',
          seq($.identifier, repeat(seq(',', $.identifier)))))),
      '{',
      repeat($._object_member),
      '}',
    ),

    _object_member: $ => choice($.field_declaration),
    field_declaration: $ => seq(
      field('qualifier', choice('val', 'var')),
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      optional(','),
    ),

    // interface Name { func sig() -> T  func other(p: T) }
    interface_declaration: $ => seq(
      'interface',
      field('name', $.identifier),
      optional($.generic_params),
      '{',
      repeat($.method_signature),
      '}',
    ),

    method_signature: $ => seq(
      'func',
      field('name', $.identifier),
      field('parameters', $.parameter_list),
      optional(seq('->', field('return_type', $._type))),
    ),

    // enum Name[<Generics>] { Variant, Variant(T), Variant(T, U) }
    enum_declaration: $ => seq(
      'enum',
      field('name', $.identifier),
      optional($.generic_params),
      '{',
      optional(seq(
        $.enum_variant,
        repeat(seq(',', $.enum_variant)),
        optional(','),
      )),
      '}',
    ),

    enum_variant: $ => seq(
      field('name', $.identifier),
      optional(seq(
        '(',
        optional(seq($._type, repeat(seq(',', $._type)))),
        ')',
      )),
      optional(seq('=', field('ordinal', $.integer_literal))),
    ),

    // func [fusible] name[<G>](params) [-> T] { body }
    // Method form: func TypeName.method[<G>](params) [-> T] { body }
    // Array extension: func [T].method(...) { body }
    func_declaration: $ => seq(
      optional(seq('@', 'fusible')),
      'func',
      field('name', $.identifier),
      optional($.generic_params),
      field('parameters', $.parameter_list),
      optional(seq('->', field('return_type', $._type))),
      field('body', $.block),
    ),

    method_declaration: $ => seq(
      optional(seq('@', 'fusible')),
      'func',
      choice(
        // Array extension method: func [T].name(...)
        seq('[', field('elem_type', $.identifier), ']', '.'),
        // Type method: func TypeName.name(...)
        seq(field('type_name', $.identifier), '.'),
      ),
      field('name', $.identifier),
      optional($.generic_params),
      field('parameters', $.parameter_list),
      optional(seq('->', field('return_type', $._type))),
      field('body', $.block),
    ),

    val_declaration: $ => seq(
      'val',
      field('binding', choice($.identifier, $.tuple_binding)),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('value', $._expression))),
    ),

    var_declaration: $ => seq(
      'var',
      field('binding', choice($.identifier, $.tuple_binding, '_')),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('value', $._expression))),
    ),

    // Tuple destructure binding: val (x, y) = pair — Phase 59 P01d
    tuple_binding: $ => seq(
      '(',
      choice($.identifier, '_'),
      ',',
      choice($.identifier, '_'),
      repeat(seq(',', choice($.identifier, '_'))),
      ')',
    ),

    // extern func puts(s: String) -> Int   — no body
    extern_func_declaration: $ => seq(
      'extern',
      'func',
      field('name', $.identifier),
      field('parameters', $.parameter_list),
      optional(seq('->', field('return_type', $._type))),
    ),

    // generic params: [T] or [T, U] or [T, U, V]
    generic_params: $ => seq(
      '[',
      seq($.identifier, repeat(seq(',', $.identifier))),
      ']',
    ),

    parameter_list: $ => seq('(', optional(commaSep1($.parameter)), ')'),
    parameter: $ => seq(
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
    ),

    // ── Block & Statements ─────────────────────────────────────────────

    block: $ => seq('{', repeat($._statement), '}'),

    _statement: $ => choice(
      $.val_declaration,
      $.var_declaration,
      $.assignment_statement,
      $.if_statement,
      $.while_statement,
      $.for_statement,
      $.match_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.defer_statement,
      $.expression_statement,
    ),

    assignment_statement: $ => prec.right(1, seq(
      field('target', $._expression),
      field('op', choice(
        '=', '+=', '-=', '*=', '/=',
        // Phase 59 compound-assign bitwise forms
        '<<=', '>>=', '&=', '|=', '^=',
      )),
      field('value', $._expression),
    )),

    if_statement: $ => seq(
      'if',
      field('condition', $._expression),
      field('consequence', $.block),
      repeat($.elif_clause),
      optional($.else_clause),
    ),
    elif_clause: $ => seq(
      'elif', field('condition', $._expression), field('consequence', $.block),
    ),
    else_clause: $ => seq('else', field('consequence', $.block)),

    while_statement: $ => seq(
      'while', field('condition', $._expression), field('body', $.block),
    ),

    for_statement: $ => seq(
      'for',
      field('variable', $.identifier),
      'in',
      field('iterable', $._expression),
      // `parallel` modifier: `for i in n parallel { ... }` (Phase 49 pfor)
      optional(field('modifier', 'parallel')),
      field('body', $.block),
    ),

    match_statement: $ => seq(
      'match',
      field('subject', $._expression),
      '{',
      repeat($.match_arm),
      optional($.match_else_arm),
      '}',
    ),
    match_arm: $ => seq(
      field('pattern', $._pattern),
      '->',
      field('body', $._match_arm_body),
    ),
    match_else_arm: $ => seq(
      'else', '->',
      field('body', $._match_arm_body),
    ),
    _match_arm_body: $ => choice(
      $.block,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.assignment_statement,
      $._expression,
    ),

    return_statement: $ => prec.right(seq(
      'return',
      optional(field('value', $._expression)),
    )),

    break_statement: $ => 'break',
    continue_statement: $ => 'continue',

    // defer { block } | defer single_statement (e.g. `defer println(...)`)
    defer_statement: $ => seq('defer', choice(
      $.block,
      $._expression,
    )),

    // Guard expression_statement with low precedence so other statement
    // keywords win in the _statement choice.
    expression_statement: $ => prec(-1, $._expression),

    // ── Expressions ────────────────────────────────────────────────────

    _expression: $ => choice(
      $.binary_expression,
      $.unary_expression,
      $.is_expression,
      $.call_expression,
      $.member_expression,
      $.index_expression,
      $.parenthesized_expression,
      $.tuple_expression,
      $.array_literal,
      $.lambda_expression,
      $.heap_expression,
      $.rc_expression,
      $.comptime_expression,
      $.await_expression,
      $.spawn_expression,
      $.string_literal,
      $.integer_literal,
      $.float_literal,
      $.boolean_literal,
      $.null_literal,
      $.self_expression,
      $.identifier,
    ),

    binary_expression: $ => choice(
      prec.left(3,  seq($._expression, field('op', choice('or', '||')),  $._expression)),
      prec.left(4,  seq($._expression, field('op', choice('and', '&&')), $._expression)),
      prec.left(5,  seq($._expression, field('op', '|'),                 $._expression)),
      prec.left(6,  seq($._expression, field('op', '^'),                 $._expression)),
      prec.left(7,  seq($._expression, field('op', '&'),                 $._expression)),
      prec.left(8,  seq($._expression, field('op', choice('==', '!=')),  $._expression)),
      prec.left(9,  seq($._expression, field('op', choice('<', '>', '<=', '>=')), $._expression)),
      prec.left(10, seq($._expression, field('op', choice('<<', '>>')),  $._expression)),
      prec.left(11, seq($._expression, field('op', choice('+', '-')),    $._expression)),
      prec.left(12, seq($._expression, field('op', choice('*', '/', '%')), $._expression)),
    ),

    unary_expression: $ => prec.right(13, seq(
      field('op', choice('-', '!', '~', 'not')),
      $._expression,
    )),

    is_expression: $ => prec.left(2, seq(
      $._expression, 'is', field('type', $._type),
    )),

    call_expression: $ => prec.left(14, seq(
      field('function', $._expression),
      '(',
      optional(commaSep1($._expression)),
      ')',
    )),

    member_expression: $ => prec.left(14, seq(
      field('object', $._expression),
      '.',
      field('property', $.identifier),
    )),

    index_expression: $ => prec.left(14, seq(
      field('collection', $._expression),
      '[',
      field('index', $._expression),
      ']',
    )),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    // (a, b) or (a, b, c) — arity >= 2 (arity 1 == parenthesized_expression)
    tuple_expression: $ => seq(
      '(', $._expression, ',',
      $._expression,
      repeat(seq(',', $._expression)),
      optional(','), ')',
    ),

    array_literal: $ => seq(
      '[',
      optional(seq($._expression, repeat(seq(',', $._expression)), optional(','))),
      ']',
    ),

    lambda_expression: $ => seq(
      'func',
      field('parameters', $.parameter_list),
      optional(seq('->', field('return_type', $._type))),
      field('body', $.block),
    ),

    self_expression: $ => 'self',

    // Prefix-keyword expressions — all bind at UNARY precedence (13).
    heap_expression:     $ => prec.right(13, seq('heap',     $._expression)),
    rc_expression:       $ => prec.right(13, seq('rc',       $._expression)),
    comptime_expression: $ => prec.right(13, seq('comptime', $._expression)),
    await_expression:    $ => prec.right(13, seq('await',    $._expression)),

    // spawn("name" [, pool]) { body }
    spawn_expression: $ => seq(
      'spawn',
      '(',
      field('name', $.string_literal),
      optional(seq(',', field('pool', $._expression))),
      ')',
      field('body', $.block),
    ),

    // ── Patterns ───────────────────────────────────────────────────────

    _pattern: $ => choice(
      $.literal_pattern,
      $.wildcard_pattern,
      $.variant_pattern,
      $.identifier_pattern,
    ),

    literal_pattern: $ => choice(
      $.integer_literal, $.float_literal, $.string_literal,
      $.boolean_literal, $.null_literal,
    ),
    wildcard_pattern: $ => '_',
    identifier_pattern: $ => $.identifier,

    // EnumName.Variant or EnumName.Variant(args)
    variant_pattern: $ => prec(1, seq(
      field('enum_name', $.identifier),
      '.',
      field('variant', $.identifier),
      optional(seq(
        '(',
        optional(seq($._pattern, repeat(seq(',', $._pattern)))),
        ')',
      )),
    )),

    // ── Types ──────────────────────────────────────────────────────────

    _type: $ => choice(
      $.array_type,
      $.function_type,
      $.tuple_type,
      $.nullable_type,
      $.generic_type,
      $.type_identifier,
    ),

    type_identifier: $ => $.identifier,

    nullable_type: $ => prec(1, seq($.type_identifier, '?')),

    generic_type: $ => prec(2, seq(
      $.type_identifier,
      '[',
      seq($._type, repeat(seq(',', $._type))),
      ']',
    )),

    // [T] or [T; Size]
    array_type: $ => prec(3, seq(
      '[',
      $._type,
      optional(seq(';', $._expression)),
      // Phase 48 layout hints: , layout: soa / , unordered — tolerated.
      repeat(seq(',', choice(
        seq('layout', ':', $.identifier),
        'unordered',
      ))),
      ']',
    )),

    // func(T, U) -> R
    function_type: $ => prec(4, seq(
      'func', '(',
      optional(seq($._type, repeat(seq(',', $._type)))),
      ')',
      optional(seq('->', $._type)),
    )),

    // (T, U)  — tuple type, arity >= 2
    tuple_type: $ => prec(5, seq(
      '(', $._type, ',', $._type,
      repeat(seq(',', $._type)),
      ')',
    )),

    // ── Literals ───────────────────────────────────────────────────────

    integer_literal: $ => token(choice(
      /0[xX][0-9a-fA-F_]+/,  // hex
      /0[bB][01_]+/,         // binary
      /[0-9][0-9_]*/,        // decimal
    )),

    float_literal: $ => token(choice(
      /[0-9][0-9_]*\.[0-9][0-9_]*[eE][+-]?[0-9]+/,
      /[0-9][0-9_]*\.[0-9][0-9_]*/,
    )),

    boolean_literal: $ => choice('true', 'false'),
    null_literal: $ => 'null',

    // Strings. The lexer emits IRON_TOK_INTERP_STRING as one token whenever
    // it sees an unescaped '{' inside the quotes; tree-sitter has its own
    // scanner so we model strings as a single `string_literal` rule whose
    // interior may contain interpolations. The `interpolated_string` wrapper
    // rule (used by folds.scm + highlights.scm) is an alias that fires when
    // at least one `interpolation` child is present — that distinction is
    // not encoded in the grammar (tree-sitter does not support optional-
    // marker choice rules without conflicts) but downstream queries can
    // match `(string_literal (interpolation))` for the interpolated shape.
    string_literal: $ => seq(
      '"',
      repeat(choice(
        $._string_body,
        $.escape_sequence,
        $.interpolation,
      )),
      '"',
    ),

    _string_body: $ => token.immediate(prec(1, /[^"\\{}]+/)),

    interpolation: $ => seq('{', $._expression, '}'),

    escape_sequence: $ => token.immediate(/\\[nrt"\\{}]/),

    // NOTE: interpolated strings appear in the parse tree as
    // (string_literal (interpolation ...)) — queries/highlights.scm + folds.scm
    // target that shape rather than a dedicated `interpolated_string` node.

    // ── Comments (extras) ──────────────────────────────────────────────

    // Iron line comment: `-- ...` to end of line.
    line_comment: $ => token(seq('--', /[^\n]*/)),

    // Iron doc comment: `/// ...` to end of line (src/lexer/lexer.c:710).
    // Precedence 1 so `///` wins over any hypothetical `//` match.
    doc_comment: $ => token(prec(1, seq('///', /[^\n]*/))),

    // ── Identifiers ────────────────────────────────────────────────────
    // word: $ => $.identifier above routes reserved-keyword matching into
    // this rule; keyword literals ('func', 'object', ...) take precedence
    // over the identifier regex.
    identifier: $ => /[A-Za-z_][A-Za-z0-9_]*/,
  },
});
