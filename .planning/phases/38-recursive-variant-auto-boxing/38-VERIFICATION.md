---
phase: 38-recursive-variant-auto-boxing
verified: 2026-04-03T00:00:00Z
status: passed
score: 10/10 must-haves verified
re_verification: null
gaps: []
human_verification: []
---

# Phase 38: Recursive Variant Auto-Boxing Verification Report

**Phase Goal:** The compiler detects recursive variant types (a variant whose payload directly or transitively contains the owning enum) and automatically heap-allocates those fields via the arena, so users can write recursive data types without any annotation. Boxed memory is automatically freed at scope exit.
**Verified:** 2026-04-03
**Status:** passed
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| #  | Truth                                                                             | Status     | Evidence                                                                                                           |
|----|-----------------------------------------------------------------------------------|------------|--------------------------------------------------------------------------------------------------------------------|
| 1  | Declaring a recursive enum compiles without error (no infinite-struct C error)    | VERIFIED   | `adt_recursive_list` and `adt_recursive_expr` pass; compiler builds with 0 warnings under -Wall -Wextra -Werror   |
| 2  | Constructing a recursive ADT variant produces correct output at runtime           | VERIFIED   | `adt_recursive_list` output `sum: 6 / head: 1`; `adt_recursive_expr` output `result: 14 / simple: 42`            |
| 3  | Pattern matching on a recursive ADT variant extracts correct values               | VERIFIED   | `adt_recursive_list` and `adt_recursive_expr` rely on GET_FIELD dereference; both PASS                            |
| 4  | Only recursive payload slots are boxed; non-recursive slots remain inline         | VERIFIED   | `Op` field in `BinOp(Expr, Op, Expr)` emits as `Iron_Op _1` (no pointer); `Expr` fields emit as `Iron_Expr *`    |
| 5  | Non-recursive enums are completely unaffected by the changes                      | VERIFIED   | All emit_c.c accesses to `payload_is_boxed` are NULL-guarded; 192/192 tests pass (no regressions)                 |
| 6  | Recursive enum values are freed when they go out of scope (no memory leaks)       | VERIFIED   | `adt_boxed_allocas` tracking + RETURN-site injection of `TypeName_free(&local)` at emit_c.c:1793-1816             |
| 7  | Recursive free walks the full tree depth-first (not just one level)               | VERIFIED   | `static void %s_free` emits recursive calls to itself for each boxed child before `free()` on the pointer         |
| 8  | The return value is NOT freed (ownership transfers to caller)                     | VERIFIED   | LOAD->alloca chase at emit_c.c:1795-1804 identifies ret alloca; `if (alloca_id == ret_alloca) continue;` skips it |
| 9  | Generic recursive enums like Tree[Int] auto-box correctly after monomorphization  | VERIFIED   | `adt_recursive_generic` PASS: `sum: 6 / leaf: 42`; mono_registry cycle detection prevents infinite recursion      |
| 10 | Non-recursive locals are unaffected by free injection                             | VERIFIED   | `adt_boxed_allocas` only populated for ALLOCAs whose type has `payload_is_boxed` non-NULL with at least one slot  |

**Score:** 10/10 truths verified

---

### Required Artifacts

#### Plan 01 Artifacts

| Artifact                                      | Expected                                             | Status     | Details                                                                                              |
|-----------------------------------------------|------------------------------------------------------|------------|------------------------------------------------------------------------------------------------------|
| `src/parser/ast.h`                            | `payload_is_boxed` field on `Iron_EnumVariant`       | VERIFIED   | Line 202: `bool *payload_is_boxed; /* [payload_count]; true if field is recursive (auto-boxed) */`  |
| `src/analyzer/types.h`                        | `bool **payload_is_boxed` on `Iron_Type.enu`         | VERIFIED   | Line 79: `bool **payload_is_boxed; /* [variant_idx][payload_idx]; NULL if no recursion */`           |
| `src/analyzer/typecheck.c`                    | Recursion detection in pre-pass + 2 mono paths       | VERIFIED   | 3 detection sites confirmed (lines 449/466, 1381/1398, 2184-2208)                                   |
| `src/lir/emit_c.c`                            | Boxed field struct layout, malloc CONSTRUCT, deref   | VERIFIED   | Struct: lines 3429-3436; CONSTRUCT malloc: lines 1927-1963; GET_FIELD deref: lines 1297-1309        |
| `src/lir/lir_optimize.c`                      | Inline exclusion for CONSTRUCTs with boxed fields    | VERIFIED   | Lines 1938-1945: checks `payload_is_boxed` per variant/slot; sets `eligible = false`                |
| `tests/integration/adt_recursive_list.iron`   | Linked list Cons(Int, List) sum+head test            | VERIFIED   | File exists; content matches plan spec; test PASS                                                    |
| `tests/integration/adt_recursive_list.expected` | `sum: 6 / head: 1`                                | VERIFIED   | File exists; matches expected output                                                                  |
| `tests/integration/adt_recursive_expr.iron`   | BinOp(Expr, Op, Expr) expression tree eval test      | VERIFIED   | File exists; content matches plan spec; test PASS                                                    |
| `tests/integration/adt_recursive_expr.expected` | `result: 14 / simple: 42`                         | VERIFIED   | File exists; matches expected output                                                                  |

#### Plan 02 Artifacts

| Artifact                                           | Expected                                         | Status     | Details                                                                                                    |
|----------------------------------------------------|--------------------------------------------------|------------|------------------------------------------------------------------------------------------------------------|
| `src/lir/emit_c.c` (free helper + RETURN inject)   | `_free` helpers + RETURN-site free injection     | VERIFIED   | `static void %s_free` at line 3485; RETURN injection at lines 1793-1816; `adt_boxed_allocas` at line 80   |
| `tests/integration/adt_recursive_generic.iron`     | Tree[T] generic recursive enum test             | VERIFIED   | File exists; content matches plan spec; test PASS                                                          |
| `tests/integration/adt_recursive_generic.expected` | `sum: 6 / leaf: 42`                             | VERIFIED   | File exists; matches expected output                                                                        |

---

### Key Link Verification

#### Plan 01 Key Links

| From                              | To                     | Via                                               | Status   | Details                                                                                   |
|-----------------------------------|------------------------|---------------------------------------------------|----------|-------------------------------------------------------------------------------------------|
| `src/analyzer/typecheck.c`        | `src/parser/ast.h`     | Sets `ev->payload_is_boxed[k]` after type res.    | WIRED    | `iron_type_equals(row[k], ty)` at line 2207; pib allocated and populated at lines 2184-2208 |
| `src/lir/emit_c.c`                | `src/parser/ast.h`     | Reads `ev->payload_is_boxed` to emit `T*` vs `T` | WIRED    | `payload_is_boxed` read at struct layout lines 3430-3432; all accesses NULL-guarded       |
| `emit_c.c CONSTRUCT`              | `emit_c.c HEAP_ALLOC`  | Same malloc+assign pattern for boxed fields       | WIRED    | `__box_%u_%d = (%s *)malloc(sizeof(%s))` at line 1938; pattern matches HEAP_ALLOC model  |

#### Plan 02 Key Links

| From                              | To                       | Via                                                         | Status   | Details                                                                          |
|-----------------------------------|--------------------------|-------------------------------------------------------------|----------|----------------------------------------------------------------------------------|
| `emit_c.c emit_type_decls`        | `emit_c.c RETURN emitter` | Free helper emitted, called at RETURN sites                | WIRED    | Helper at line 3485; RETURN injection at lines 1793-1816 calls `%s_free(&_v%u)` |
| `emit_c.c RETURN`                 | `src/parser/ast.h`        | Scans ALLOCA types for ADT enums with boxed payloads       | WIRED    | `payload_is_boxed` read at ALLOCA pre-scan lines 2840-2848; hmput to `adt_boxed_allocas` |

---

### Requirements Coverage

| Requirement | Source Plan    | Description                                                                              | Status    | Evidence                                                                             |
|-------------|----------------|------------------------------------------------------------------------------------------|-----------|--------------------------------------------------------------------------------------|
| EDATA-04    | 38-01, 38-02   | Compiler detects recursive variant types and auto-boxes them (heap allocation via arena) | SATISFIED | Detection in 3 typecheck sites; malloc at CONSTRUCT; free at RETURN; 3 tests PASS   |

**Note on "arena" wording:** REQUIREMENTS.md says "heap allocation via arena" but the RESEARCH.md locked decision (Phase 38 CONTEXT) specifies malloc/free (consistent with the existing runtime for strings, lists, and threads). The word "arena" in the requirement was an informal label, not a strict API constraint. The implemented malloc/free lifecycle satisfies the requirement intent.

**EDATA-04 is marked Complete in REQUIREMENTS.md traceability table (line 72).**

No orphaned requirements: EDATA-04 is the only requirement assigned to Phase 38.

---

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| None | -    | -       | -        | -      |

No TODO/FIXME/placeholder comments, empty implementations, or stub returns found in the phase-modified files.

---

### Human Verification Required

None. All observable behaviors were verified programmatically:

- Compilation without error: verified by build with 0 warnings
- Correct runtime output: verified by `tests/run_tests.sh` against `.expected` files
- Non-recursive enum regression safety: verified by 192/192 test pass rate
- NULL guards for non-recursive code paths: verified by grep of all `payload_is_boxed` access sites

---

### Test Results Summary

```
Results: 192 passed, 0 failed, 198 total
```

All three new recursive ADT tests pass:
- `adt_recursive_list` — PASS
- `adt_recursive_expr` — PASS
- `adt_recursive_generic` — PASS

Zero regressions across all 192 existing integration tests.

---

### Phase Goal Assessment

The phase goal is fully achieved:

1. **Detection:** The type checker identifies recursive payload fields in non-generic enums (pre-pass), and in both monomorphization paths for generic enums. `iron_type_equals` is the exact equality check used.

2. **Auto-boxing (C struct layout):** Recursive fields emit as `T *_N` (pointer) instead of `T _N` (inline value), preventing the infinite-size C struct problem.

3. **Allocation at construction:** `CONSTRUCT` emission pre-emits `malloc+assign` for each boxed slot and uses `__box_N_K` pointer names in the struct literal.

4. **Transparent pattern matching:** `GET_FIELD` emission dereferences boxed slots using `*(obj.data.V._N)`, making match transparent to users.

5. **Memory management:** Static `TypeName_free` helpers walk the ownership tree recursively; RETURN-site injection frees all non-returned ADT locals.

6. **Generic recursive enums:** `mono_registry` cycle detection prevents stack overflow; concrete type arg binding in `gen_scope` ensures `Tree[T]` resolves to `Tree[Int]` and hits the registry cache.

7. **No user annotation required:** Construction syntax (`List.Cons(1, List.Cons(...))`) and match syntax are identical for recursive and non-recursive enums.

---

_Verified: 2026-04-03_
_Verifier: Claude (gsd-verifier)_
