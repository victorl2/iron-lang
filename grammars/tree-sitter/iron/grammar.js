// GENERATED at configure time from src/lexer/lexer.c kw_table.
// DO NOT EDIT. Edit src/lexer/lexer.c + this .in template, then run
// scripts/regenerate-grammars.sh. The committed grammar.js is drift-gated
// via CTest test_grammar_keyword_drift_tree_sitter (dual-labelled
// phase-m5-invariant + phase-m1-invariant).
//
// Plan 06-01 v0.1 grammar shape — MINIMAL. Plan 06-02 replaces _top with
// full _declaration rules (import/object/interface/enum/func/val/impl) +
// full statement/expression grammar + a precedence table transcribed from
// src/parser/parser.c. The permissive _top here is intentional: it lets
// tree-sitter generate + smoke-corpus-parse succeed without blocking on
// the full grammar, while still exercising the keyword-drift wiring.
//
// Interpolated-string shape (Assumption A2 in 06-RESEARCH.md): the lexer
// emits IRON_TOK_INTERP_STRING as a SINGLE token (src/lexer/lexer.c:417),
// but tree-sitter has its own scanner — it does not consume lexer tokens.
// The grammar defines its own interpolated_string rule with a minimal
// ${identifier} interpolation; Plan 06-02 expands to full expressions.

module.exports = grammar({
  name: 'iron',

  extras: $ => [/\s/, $.line_comment, $.block_comment, $.doc_comment],

  word: $ => $.identifier,

  rules: {
    source_file: $ => repeat($._top),

    // _top is the v0.1 permissive choice; Plan 06-02 replaces with
    // _declaration + proper statement/expression nesting.
    _top: $ => choice(
      $.line_comment,
      $.block_comment,
      $.doc_comment,
      $.integer_literal,
      $.float_literal,
      $.string_literal,
      $.interpolated_string,
      $._keyword,
      $.identifier,
    ),

    // Keywords generated from src/lexer/lexer.c kw_table at configure time.
    // 'and', 'await', 'comptime', 'defer', 'elif', 'else', 'enum', 'extends', 'extern', 'false', 'for', 'free', 'func', 'heap', 'if', 'impl', 'import', 'in', 'interface', 'is', 'leak', 'match', 'not', 'null', 'object', 'or', 'parallel', 'pool', 'private', 'rc', 'return', 'self', 'spawn', 'super', 'true', 'val', 'var', 'while' expands to 'and', 'await', ..., 'while'.
    _keyword: $ => choice('and', 'await', 'comptime', 'defer', 'elif', 'else', 'enum', 'extends', 'extern', 'false', 'for', 'free', 'func', 'heap', 'if', 'impl', 'import', 'in', 'interface', 'is', 'leak', 'match', 'not', 'null', 'object', 'or', 'parallel', 'pool', 'private', 'rc', 'return', 'self', 'spawn', 'super', 'true', 'val', 'var', 'while'),

    // --- literals ---
    integer_literal: $ => /\d+/,
    float_literal: $ => /\d+\.\d+([eE][+-]?\d+)?/,

    string_literal: $ => seq(
      '"',
      repeat(choice(/[^"\\]+/, $.escape_sequence)),
      '"',
    ),

    escape_sequence: $ => /\\[nrt"\\]/,

    interpolated_string: $ => seq(
      '$"',
      repeat(choice(/[^"$\\]+/, $.escape_sequence, $.interpolation)),
      '"',
    ),

    interpolation: $ => seq('${', $.identifier, '}'),

    // --- comments ---
    line_comment: $ => token(prec(1, /\/\/[^/\n][^\n]*|\/\//)),
    doc_comment: $ => token(prec(2, /\/\/\/[^\n]*/)),
    block_comment: $ => token(seq(
      '/*',
      /[^*]*\*+([^/*][^*]*\*+)*/,
      '/',
    )),

    // --- identifiers ---
    identifier: $ => /[A-Za-z_][A-Za-z0-9_]*/,
  },
});
