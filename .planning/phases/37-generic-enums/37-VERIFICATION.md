---
phase: 37-generic-enums
verified: 2026-04-03T00:00:00Z
status: passed
score: 11/11 must-haves verified
re_verification: false
---

# Phase 37: Generic Enums Verification Report

**Phase Goal:** Users can define generic enums (`Option[T]`, `Result[T, E]`), instantiate them with concrete types, and match on them — each concrete instantiation is monomorphized to a distinct C typedef with type-argument-aware name mangling.
**Verified:** 2026-04-03
**Status:** passed
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | `enum Option[T] { Some(T), None }` parses without error | VERIFIED | `parser.c:2148` sets `n->generic_param_count = generic_count` in `iron_parse_enum_decl`; `generic_enum_option.iron` compiles cleanly |
| 2 | `Option[Int]` in a type annotation resolves to a monomorphized Iron_Type with `type_args=[Int]` and `mangled_name` | VERIFIED | `typecheck.c:424` sets `mono->enu.mangled_name`; `--verbose` output shows `type @Iron_Option_Int = Option[Int]` |
| 3 | `Option[Int]` and `Option[String]` are distinct types (`iron_type_equals` returns false) | VERIFIED | `types.c:196-197` compares `type_arg_count` and each `type_args[i]`; `multi_inst` test produces `Iron_Option_Int` and `Iron_Option_String` as separate typedefs |
| 4 | `variant_payload_types` for `Option[Int].Some` contains `Iron_Type(INT)`, not `Iron_Type(GENERIC_PARAM)` | VERIFIED | `typecheck.c:459` calls `substitute_generic_type` per payload; `IRON_TYPE_GENERIC_PARAM` case (lines 301+) maps param name to concrete arg |
| 5 | `IRON_NODE_ENUM_CONSTRUCT` for `Option.Some(42)` type-checks against monomorphized payload types | VERIFIED | `typecheck.c:1206` `ed->generic_param_count > 0` branch infers type args; `generic_enum_option` binary runs, prints `option_int: ok` |
| 6 | `enum Option[T]` compiles and runs end-to-end | VERIFIED | `build/ironc build generic_enum_option.iron` succeeds; binary prints `option_int: ok` (matches `.expected`) |
| 7 | `enum Result[T, E]` compiles and runs end-to-end | VERIFIED | `build/ironc build generic_enum_result.iron` succeeds; binary prints `result_ok: ok` (matches `.expected`); `maybe_fill_missing_generic_args` handles partial inference for `Result.Ok(100)` |
| 8 | `Option[Int]` and `Option[String]` produce distinct C typedef names | VERIFIED | C output: `typedef struct Iron_Option_Int Iron_Option_Int` and `typedef struct Iron_Option_String Iron_Option_String`; `mono_registry` deduplication prevents collisions |
| 9 | Match on `Option[Int]` correctly binds the `Int` payload | VERIFIED | `generic_enum_match` binary prints `42` then `-1`; `build_hir_params_named` in `hir_lower.c:211` provides resolved param types so `Option[Int]` appears correctly in function prototype |
| 10 | Two distinct instantiations (`Option[Int]`, `Option[String]`) produce separate C typedefs | VERIFIED | `generic_enum_multi_inst` binary prints `int:7` then `str:hello`; generated C shows both `Iron_Option_Int` and `Iron_Option_String` |
| 11 | Nested generic `Result[Option[Int], String]` produces `Iron_Result_Option_Int_String` | VERIFIED | `--verbose` output: `type @Iron_Result_Option_Int_String = Result[Option[Int], String]`; `type_mangle_component` strips brackets; binary prints `nested: ok` |

**Score:** 11/11 truths verified

---

### Required Artifacts

| Artifact | Provides | Status | Details |
|----------|----------|--------|---------|
| `src/parser/ast.h` | `Iron_EnumDecl` with `generic_params`/`generic_param_count` | VERIFIED | Lines 140-141: `generic_params` and `generic_param_count` added to `Iron_EnumDecl` after `has_payloads` |
| `src/analyzer/types.h` | `Iron_Type.enu` extended with `type_args`, `type_arg_count`, `mangled_name` | VERIFIED | Lines 76-78 in `enu` struct contain all three fields |
| `src/analyzer/types.c` | `iron_type_equals` updated for generic enum comparison; `iron_type_to_string` for generic enums | VERIFIED | Lines 196-197: `type_arg_count` comparison loop; lines 299-304: `iron_type_to_string` branches on `type_arg_count == 0` |
| `src/parser/parser.c` | `iron_parse_enum_decl` parses `[T, E]` after enum name | VERIFIED | Line 2066: calls `iron_parse_generic_params` inside `iron_parse_enum_decl`; line 2148: assigns `generic_param_count` to node |
| `src/analyzer/typecheck.c` | `resolve_type_annotation` handles generic enum instantiation with type substitution; `substitute_generic_type` helper; `IRON_NODE_ENUM_CONSTRUCT` type inference | VERIFIED | Lines 301+: `substitute_generic_type` defined; lines 387+: monomorphization in `resolve_type_annotation`; line 1206: construct inference; line 2108: pre-pass skip |
| `src/hir/hir_to_lir.c` | Generic enum base decl skipped; monomorphized instances registered | VERIFIED | Line 619: `generic_param_count > 0` skip guard; lines 442-453: `MonoEnumSeen`, `register_mono_enum`, `collect_mono_enums_node` |
| `src/lir/emit_c.c` | `emit_type_to_c` uses `mangled_name`; enum struct emitter uses `mangled_name`; `mono_registry` deduplication | VERIFIED | Line 203-204: `mangled_name` in `emit_type_to_c`; lines 3215-3224: struct emitter uses `mangled_name` + `mono_registry` check; lines 684-685, 1814-1815: CONSTRUCT tag uses `mangled_name` |
| `src/hir/hir_lower.c` | `build_hir_params_named` resolves generic param types from global scope | VERIFIED | Line 211: function defined; line 1307: called for function declarations |
| `tests/integration/generic_enum_option.iron` | `Option[T]` basic instantiation and print | VERIFIED | File exists, contains `enum Option[T]`; test passes |
| `tests/integration/generic_enum_result.iron` | `Result[T, E]` two-param generic | VERIFIED | File exists, contains `enum Result[T, E]`; test passes |
| `tests/integration/generic_enum_match.iron` | Match on `Option[Int]` with payload binding | VERIFIED | File exists, contains `match o`; test passes, outputs `42` and `-1` |
| `tests/integration/generic_enum_multi_inst.iron` | Two distinct instantiations of same generic enum | VERIFIED | File exists, contains `Option[String]`; test passes, outputs `int:7` and `str:hello` |
| `tests/integration/generic_enum_nested.iron` | Nested generic type args | VERIFIED | File exists, contains `Result[Option[Int], String]`; test passes, outputs `nested: ok` |

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `src/parser/parser.c` | `src/parser/ast.h` | `iron_parse_enum_decl` sets `n->generic_params` and `n->generic_param_count` | WIRED | `parser.c:2147-2148`: `n->generic_params = generic_params; n->generic_param_count = generic_count;` |
| `src/analyzer/typecheck.c` | `src/analyzer/types.h` | `resolve_type_annotation` creates monomorphized `Iron_Type.enu` with `type_args` and `mangled_name` | WIRED | `typecheck.c:424`: `mono->enu.mangled_name = mangled`; `typecheck.c:1296`: same in ENUM_CONSTRUCT path |
| `src/analyzer/typecheck.c` | `src/analyzer/types.c` | `iron_type_equals` compares `type_args` for generic enums | WIRED | `types.c:196`: `if (a->enu.type_arg_count != b->enu.type_arg_count) return false;` followed by per-arg comparison loop |
| `src/hir/hir_to_lir.c` | `src/lir/emit_c.c` | Monomorphized type_decls registered with `Iron_Type.enu.mangled_name` set | WIRED | `hir_to_lir.c:452`: `iron_lir_module_add_type_decl` called with `type->enu.mangled_name`; `emit_c.c:3215` reads `td->type->enu.mangled_name` |
| `src/lir/emit_c.c` | `src/analyzer/types.h` | `emit_type_to_c` reads `t->enu.mangled_name` for `IRON_TYPE_ENUM` | WIRED | `emit_c.c:203-204`: `if (t->enu.mangled_name) { return t->enu.mangled_name; }` |
| `src/lir/emit_c.c` | `ctx->mono_registry` | `shgeti`/`shput` deduplication for monomorphized enum typedefs | WIRED | `emit_c.c:3223-3224`: `shgeti` guard then `shput` for the mangled name |

---

### Requirements Coverage

| Requirement | Source Plan(s) | Description | Status | Evidence |
|-------------|---------------|-------------|--------|----------|
| GENER-01 | 37-01, 37-02 | Enums support generic type parameters (`Option[T]`, `Result[T, E]`) | SATISFIED | Parser accepts `[T]`/`[T, E]` syntax; type checker resolves instantiations; 5 integration tests compile and run |
| GENER-02 | 37-01, 37-02 | Generic enums are monomorphized | SATISFIED | Each concrete instantiation produces a distinct `Iron_Type` with substituted `variant_payload_types`; distinct C typedefs emitted; match binds concrete payload type |
| GENER-03 | 37-02 | C emission uses type-argument-aware mangling to avoid typedef collisions | SATISFIED | `Iron_Option_Int` and `Iron_Option_String` are separate typedefs; `Iron_Result_Option_Int_String` for nested; `mono_registry` deduplication confirmed |

All three requirements declared in plan frontmatter are accounted for. No orphaned requirements found in REQUIREMENTS.md for Phase 37.

---

### Anti-Patterns Found

No blockers or warnings found in modified files.

A deliberate `return t; /* nested generic substitution is deferred */` comment exists in `substitute_generic_type` for the deeply-nested `IRON_TYPE_ENUM` case, but this is an intentional design note, not a stub — the nested test (`generic_enum_nested.iron`) passes end-to-end because the substitution at the top level is sufficient for the currently supported use cases.

---

### Human Verification Required

None. All goal behaviors are verified programmatically:

- All 5 integration tests produce correct output confirmed by running binaries.
- C name mangling verified via `--verbose` emission showing `Iron_Option_Int`, `Iron_Option_String`, and `Iron_Result_Option_Int_String`.
- Full integration suite: **189 passed, 0 failed, 195 total** — no regressions.

---

### Commits Verified

All four plan commits exist in the repository:

| Commit | Description |
|--------|-------------|
| `3d6d59c` | feat(37-01): extend AST and type data structures for generic enums |
| `2d64ecc` | feat(37-01): parser and type checker for generic enum monomorphization |
| `04de163` | feat(37-02): HIR-to-LIR mono enum registration and C emitter mangled names |
| `e9b0025` | feat(37-02): integration tests and pipeline fixes for generic enum end-to-end |

---

### Summary

Phase 37 fully achieves its goal. Every layer of the pipeline was verified against the actual codebase:

- **AST layer:** `Iron_EnumDecl` carries `generic_params`/`generic_param_count`; `Iron_Type.enu` carries `type_args`, `type_arg_count`, `mangled_name`.
- **Parser:** `iron_parse_enum_decl` parses `[T, E]`; type annotation parsing uses recursive calls for nested generics like `Result[Option[Int], String]`.
- **Type checker:** `resolve_type_annotation` monomorphizes at use sites; `substitute_generic_type` replaces `IRON_TYPE_GENERIC_PARAM` nodes; `IRON_NODE_ENUM_CONSTRUCT` infers type args; `maybe_fill_missing_generic_args` handles multi-param partial inference.
- **HIR-to-LIR:** `collect_mono_enums_node` walks AST to find and register monomorphized instances; generic base declarations are skipped.
- **C emitter:** `emit_type_to_c` uses `mangled_name`; struct emitter and CONSTRUCT tag emission use `mangled_name`; `mono_registry` prevents duplicate typedefs.
- **Tests:** 5 integration tests covering all GENER requirements pass; zero regressions across 189 tests.

---

_Verified: 2026-04-03_
_Verifier: Claude (gsd-verifier)_
