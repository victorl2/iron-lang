# Iron TextMate Grammar

Phase 6 Plan 06-01 (EXT-01, EXT-03, CONTEXT D-02). The grammar in this
directory is consumed by VSCode (and any editor that reads TextMate
`.tmLanguage.json` files) to provide lexical syntax highlighting for
`.iron` source files.

The grammar **must not diverge** from the compiler's lexer. `configure_file`
extracts the keyword list from `src/lexer/lexer.c kw_table` at CMake
configure time and substitutes it into `iron.tmLanguage.json.in`; the
committed `iron.tmLanguage.json` is drift-gated by the
`test_grammar_keyword_drift_textmate` CTest (dual-labelled
`phase-m5-invariant` + `phase-m1-invariant`).

## Files

| File | Purpose |
|------|---------|
| `iron.tmLanguage.json.in` | Template consumed by `configure_file`; contains `@IRON_KEYWORD_LIST_JSON@` substitution |
| `iron.tmLanguage.json` | Committed generated grammar; drift-gated |
| `language-configuration.json` | VSCode comment markers + brackets + `onEnterRules` for `///` doc-comment continuation |
| `README.md` | This file |

## Regenerating

Run `scripts/regenerate-grammars.sh` after editing `src/lexer/lexer.c
kw_table` (adding/removing keywords). The committed
`iron.tmLanguage.json` is refreshed from `iron.tmLanguage.json.in`.

Equivalent CMake invocation:

```bash
cmake --build build --target regenerate-grammars
```

## Scope Map

The TextMate scope names follow UI-SPEC S6 (LOCKED 2026-04-19). Every
scope suffix ends with `.iron` so theme authors can override Iron-specific
highlights without affecting other languages.

| Iron token class | TextMate scope |
|------------------|----------------|
| Control-flow keywords (`if`, `else`, `elif`, `for`, `while`, `match`, `return`, `break`, `continue`, `defer`, `in`) | `keyword.control.iron` |
| Declaration keywords (`func`, `object`, `interface`, `enum`, `val`, `var`, `impl`, `import`) | `keyword.declaration.iron` |
| Boolean + null literals (`true`, `false`, `null`) | `constant.language.iron` |
| Other keywords (`comptime`, `extern`, `self`, `super`, `and`, `or`, `not`, `is`, `free`, `heap`, `leak`, `parallel`, `pool`, `private`, `rc`, `spawn`, `await`, `extends`) | `keyword.other.iron` |
| Generated keyword alternation (drift-guard target) | `keyword.iron` |
| Integer literal | `constant.numeric.integer.iron` |
| Float literal | `constant.numeric.float.iron` |
| Double-quoted string | `string.quoted.double.iron` |
| Interpolated string `$"..."` | `string.interpolated.iron` with inner `meta.interpolation.iron` |
| Escape sequence | `constant.character.escape.iron` |
| Line comment `//` | `comment.line.double-slash.iron` |
| Block comment `/* */` | `comment.block.iron` |
| Doc comment `///` | `comment.block.documentation.iron` |
| Operator | `keyword.operator.iron` |
| Section punctuation `{}[]()` | `punctuation.section.iron` |
| Separator punctuation `,;:` | `punctuation.separator.iron` |
| Type name (after `:`, `impl`, `interface`, `object`, `enum`) | `entity.name.type.iron` |
| Function name (at declaration) | `entity.name.function.iron` |

## ReDoS Discipline

Oniguruma (VSCode's regex engine) shares the catastrophic-backtracking
profile of PCRE. Regex authoring rules (CONTEXT D-02, PITFALLS §20):

1. **No nested unbounded quantifiers.** `(a+)+` and `(a|aa)+` are banned;
   they produce O(2^n) backtracking on pathological input. Substitute
   explicit anchor/delimiter pairs.
2. **No backreferences.** Oniguruma supports them, but they move the
   engine off the linear-time DFA path.
3. **Constrain `.*` / `.+` by anchors or delimiters.** Bare `.*` lets
   the engine consume the rest of the line, then backtrack token-by-token
   — always prefer a negated-character-class form (`[^\"\\n]*`).
4. **Prefer character classes over `.`.** `[A-Za-z0-9_]+` makes intent
   explicit and composes predictably.

The CTest `test_grammar_redos_clean` runs
[`safe-regex@^2.1.1`](https://www.npmjs.com/package/safe-regex) over
every `match` / `begin` / `end` string in the committed grammar and fails
on any flagged pattern. `tests/grammars/textmate/redos_check.js` is the
driver.

## Interpolated-String Pattern — Manual Review Required

Per CONTEXT D-02 specifics + PITFALLS §20, the interpolated-string
pattern `$"..."` is the highest-risk regex in the grammar and requires
manual review at every code-review boundary.

Current implementation (safe):

```json
{
  "name": "string.interpolated.iron",
  "begin": "\\$\"",
  "end": "\"",
  "patterns": [
    { "include": "#string-escape" },
    {
      "name": "meta.interpolation.iron",
      "begin": "\\$\\{",
      "end": "\\}",
      "patterns": [ { "include": "$self" } ]
    }
  ]
}
```

Why this is safe:
- `begin`/`end` delimiter pairs anchor Oniguruma at one byte and scan
  linearly to the end delimiter. There is no unbounded scan.
- The inner `meta.interpolation.iron` also uses `begin`/`end`, keeping
  the same linear-scan invariant.
- No nested unbounded quantifiers anywhere in the pattern tree.

What would make it unsafe (do NOT rewrite to):
- Single-line greedy form `\\$\"(.*?)\"` — fragile against multi-line
  strings and breaks on escaped quotes.
- Capturing group `(\\$\"[^\"]*\")` wrapped in a `+` quantifier — any
  outer `+` around a `[^\"]*` re-introduces the O(n²) backtracking
  profile.

## Snapshot Testing

`tests/grammars/textmate/snapshot.test.js` runs
[`vscode-tmgrammar-test@0.1.3`](https://www.npmjs.com/package/vscode-tmgrammar-test)
over `tests/grammars/textmate/fixtures/*.iron`. Each `.iron` fixture has
a sibling `.iron.expected-scopes` file asserting the scope name at each
column; drift between grammar behaviour and fixture expectations fails
the CTest `test_grammar_textmate_snapshot`.

To add a new fixture: write the `.iron` file, then run
`npx vscode-tmgrammar-snap` (requires npm install) to auto-generate the
`.expected-scopes` file, review it, and commit both.

## References

- `.planning/phases/06-m5-grammars-editor-extensions/06-CONTEXT.md` D-01/D-02
- `.planning/phases/06-m5-grammars-editor-extensions/06-UI-SPEC.md` §S6
- `src/lexer/lexer.c` — `kw_table` (single source of truth for keywords)
- Phase 4 Plan 04-02 — the `configure_file` pattern this grammar extractor reuses
