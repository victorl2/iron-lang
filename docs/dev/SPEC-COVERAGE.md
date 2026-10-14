# Spec coverage

`scripts/spec_coverage.py` measures how much of the manual
(`docs/language_definition.md`) the test suite exercises. Every unit it
counts is read from the manual itself, so the total is fixed by the manual
and only grows when the manual does; the tests can raise the covered count,
never the denominator.

```sh
scripts/spec_coverage.py              # summary per dimension and per section
scripts/spec_coverage.py --missing    # every uncovered unit: the to-do list
scripts/spec_coverage.py --json out.json
```

## Dimensions

| Dimension | Unit | Covered when |
|---|---|---|
| syntax | each alternative (`rule/a1.2`), optional part (`rule/opt1`) and repetition (`rule/rep1`) of each production of section 13, and each production | the derivation of a program of the positive corpus uses it, or of a negative fixture the grammar accepts (it fails in analysis, after its syntax was parsed; some forms, like a list extension with a body, are only ever rejected) |
| diagnostics | each error and warning code of the section 11 table | a `.expected` or `.expected_help` file of the negative corpus names it, a ctest regex matches it, a unit test names its `IRON_ERR_` / `IRON_WARN_` macro, or a manual example expects it (`doctest-expect-error`) |
| library | each function and method in the first column of the section 9 tables | a program of the positive corpus calls it |
| examples | each ```` ```iron ```` example of the manual | it is not marked `doctest-skip` (scripts/test_doc_examples.sh builds and runs it) |

The positive corpus is `tests/integration/v4`, `multi_file` and
`test_blocks` (parked `@expected-pass-after` fixtures excluded) and the
manual's examples that the doc-example test builds and runs; the
negative corpus is `tests/integration/v4-fail` and `diagnostics`. Every
program in them runs under ctest, so a unit counts only when a test that
actually runs exercises it.

### Multi-file fixtures and the file marker

A fixture of `tests/integration/multi_file` is a directory of `.iron`
files. `tests/run_tests.sh` packs them into one source, with a file marker
comment ahead of each file, and builds it in multi-file test mode:

```
-- @file: lib.iron
pub func ping() -> Int { return 1 }
-- @file: main.iron
func main() { println("{ping()}") }
```

A file marker is a line comment whose text starts with `@file:`, followed
by a file name (bare, or quoted when it holds spaces) and optionally
`@line: <n>` (the next line is line `n` of that file) and `@lines: <count>`
(the file is `count` lines long). The lexer tags the tokens after it with
that file, so cross-file visibility and diagnostics see separate files. It
is recognized by the comment's text, not by the characters that open the
comment (`iron_comment_intro_len` in `src/diagnostics/diagnostics.c` is the
one place that lists the comment forms).

The marker is honored in three places only, and is a plain comment
everywhere else, user source included:

| Where | How it is enabled |
|---|---|
| multi-file test mode: every marker of the source | `IRON_MULTI_FILE_TEST=1` in the environment of `iron` / `ironc` (unit tests call `iron_file_markers_set_test_mode`) |
| the prelude the compiler writes above the user's source (the standard library files it prepends, then one marker naming the user's file) | always; only the physical lines above the user's first line count (`iron_lexer_set_user_start_line`) |
| a source packed from several files: the `target/combined.iron` or `target/tests.iron` that `iron` writes for a package | `ironc --packed-source`, passed by `iron` (in process, as the language server packs a package's other files above a document: `iron_stdlib_prepend_packed`, whose prelude marker for the user's file says `@packed`); only markers between files count, each giving the length of its file with `@lines:`, so a marker written inside a package file is a comment |

Syntax coverage is exact: the grammar recognizer of
`scripts/grammar_check.py` accepts the program, and one derivation of it is
rebuilt from the recognizer's tables; only the alternatives on that
derivation count, not those tried and abandoned.

## What it does not measure

These dimensions check that each form of the language appears in a test.
They do not check the rules the prose states about those forms ("a binding
assigned in a loop body is not narrowed there by an outer check"): a
program can use `while` without testing that rule. Rule coverage needs the
rules of the manual enumerated with stable identifiers, which the tests then
cite; until then, a high percentage here means the surface is exercised, not
that every rule is.

## The ratchet

The `spec_coverage` ctest runs
`scripts/spec_coverage.py --check tests/spec_coverage_baseline.json` and
fails when a unit the baseline lists as covered is no longer covered (a
fixture removed, a production renamed without its tests). When coverage
grows, the check says so; keep the gain with

```sh
scripts/spec_coverage.py --update tests/spec_coverage_baseline.json
```

and commit the baseline with the tests that raised it.
