# Iron Diagnostic Code Registry

Authoritative table of all diagnostic codes emitted by `iron_compiler` and
surfaced through both `ironc` (CLI stderr) and `ironls` (LSP
`publishDiagnostics`). Both binaries link the same compiler frontend, so
every code listed here flows identically through the two surfaces — that
is the CORE-22 invariant.

Each row carries a Quickfix-Target column documenting the intended LSP
code-action wiring for Phase 34 (LSP-06). Phases 17 onward must add a row
here whenever a new code is allocated.

## Code-range conventions

- **Lexer errors:** 1–99 (`IRON_ERR_*`)
- **Parser errors:** 101–199 (`IRON_ERR_*`)
- **Semantic errors:** 200–299 (`IRON_ERR_*`); 290–293 reserved (Phase 8); 320–321 reserved (Phase 9)
- **LIR verifier errors:** 300–399 (`IRON_ERR_LIR_*`)
- **Lowering errors:** 400–499 (`IRON_ERR_*`)
- **HIR verifier errors:** 500–599 (`IRON_ERR_*`)
- **Warnings:** 600+ (`IRON_WARN_*`)
- **Web-target LIR errors:** 700–799 (`IRON_ERR_WEB_*`)

LSP wire format: codes are emitted as `E<NNN>` with three-digit zero-padded
numeric tail (e.g. `E0176`, `E0265`, `E0613`). Consumers comparing diagnostic
codes should accept either the bare integer or the `E<NNN>` string.

## Phase 17 — `val`/`var` field discipline (§5)

| Code | Symbol                       | Message (locked substring)                                       | Hint                                                                                | Quickfix-Target (Phase 34 LSP-06)                                | Phase | Spec § |
|-----:|------------------------------|------------------------------------------------------------------|-------------------------------------------------------------------------------------|------------------------------------------------------------------|------:|-------:|
| 176  | `IRON_ERR_MISSING_VAL_VAR`   | `must specify val or var`                                        | `insert 'val' for an immutable binding (or 'var' to allow reassignment)`            | Insert `val ` (preferred) or `var ` before the binding/field name | 17    | §5.1 + §5.2 |
| 265  | `IRON_ERR_VAL_FIELD_REASSIGN`| `cannot reassign 'val' field '<name>' after initialization`      | `declare field as 'var' to allow reassignment after init`                           | Replace `val ` with `var ` on the offending field declaration     | 17    | §5.2 + §5.3 |
| 613  | `IRON_WARN_UNUSED_VAR`       | `var binding never reassigned; declare as 'val'`                 | `change 'var' to 'val' for an immutable binding`                                    | Replace `var ` with `val ` on the binding (3-char keyword span)   | 17    | §5.5 |
| 614  | `IRON_WARN_UNUSED_VAR_PARAM` | `var parameter never mutated; remove 'var' modifier`             | `drop the 'var' modifier - parameters default to read-only`                         | Delete `var ` from the parameter declaration (3-char keyword span) | 17    | §5.5 |

### Notes

- Codes 176 and 265 are mutually exclusive sites: 176 fires at parser-time
  when val/var is missing entirely (field decl) or at resolver-time when an
  undefined IDENT is the LHS of an assign-stmt (local binding); 265 fires
  at analyzer-time when a non-pub val field is written outside `init`. The
  pre-existing code `IRON_ERR_VAL_REASSIGN=203` is intentionally retained
  for the pub-val branch so storage-class-distinct quickfix wording can
  route on three separate codes.
- Code 176 has a dual emission site by design (per Phase 17-01 CONTEXT.md
  amendment): `src/parser/parser.c:3702-3717` field-decl loop (VAL-02) and
  `src/analyzer/resolve.c` `emit_undefined` branch when
  `ResolveCtx.is_assign_lhs` is true (VAL-01). One code, two sites, single
  Phase 34 LSP-06 quickfix routing entry.
- Codes 613 and 614 anchor on the `var` keyword span (3 chars) rather than
  the binding name span — this gives Phase 34 LSP-06 a stable replacement
  target for the quickfix `TextEdit`.
- VAL-04 (var field reassignability) is deliberately code-less: it is the
  absence of an error, not the presence of one. The new VAL-03 branch in
  `src/analyzer/typecheck.c` is guarded by `!ff->is_var && !ff->is_pub` so
  var fields continue to compile cleanly.

## Adding new codes

1. Allocate the next free slot in the appropriate range. Verify uniqueness
   with `grep -nE '^#define IRON_(ERR|WARN)_' src/diagnostics/diagnostics.h | sort -k3 -n`.
2. Add a one-line `#define` plus a short comment block in
   `src/diagnostics/diagnostics.h`.
3. Append a row to the table in this file (group by phase under a new
   `## Phase NN — <subject>` heading).
4. Add a Wave 0 RED test referencing the symbol BEFORE the implementation
   lands (per the Phase 15 TDD-11 + Phase 16 TDD-10 atomic-commit
   discipline).
5. Document Quickfix-Target intent so Phase 34 LSP-06 (or any future
   quickfix-routing phase) has a stable contract to wire against.
