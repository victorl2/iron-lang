# Phase 33: Type Validation Checks - Research

**Researched:** 2026-04-02
**Domain:** Compiler type checker -- static validation of match exhaustiveness, cast safety, string interpolation stringability, and compound assignment overflow
**Confidence:** HIGH

## Summary

Phase 33 adds four categories of compile-time validation checks to the Iron compiler's type checker (`typecheck.c`). All four checks extend existing code paths -- match handling (line 1237), cast handling (line 451), interpolation string handling (line 305), and assignment handling (line 1031). The codebase already has well-established patterns for emitting diagnostics (`emit_error` helper + `iron_diag_emit`), accessing resolved types (`node->resolved_type`), iterating enum variants (`Iron_EnumDecl->variants` with `variant_count`), and scanning method declarations (linear scan of `ctx->program->decls`).

The implementation is entirely within the analyzer layer -- no parser changes, no HIR/LIR changes, no C emitter changes. Error codes will be added to `diagnostics.h` after the existing 600-range warning codes. The main complexity lies in match exhaustiveness (tracking covered enum variants across match cases) and cast literal range checking (parsing integer literal strings and comparing against type ranges). Both are straightforward algorithmic tasks with bounded memory usage.

**Primary recommendation:** Implement as four independent validation passes within `typecheck.c`, each extending the existing `check_expr` or `check_stmt` case handlers. Add new error/warning codes in `diagnostics.h`. Test each diagnostic with dedicated Unity tests in `test_typecheck.c`.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Match exhaustiveness checks that match on enum types covers all variants or has an else clause
- For non-enum types (Int, String, etc.): require else clause -- error if missing
- Check associated data patterns at full depth (not just variant names)
- Duplicate match arms (same variant covered twice) produce a compile error -- `IRON_ERR_DUPLICATE_MATCH_ARM`
- Emit `IRON_ERR_NONEXHAUSTIVE_MATCH` listing uncovered variants
- Validate source expression type is numeric or bool before allowing primitive cast
- Emit `IRON_ERR_INVALID_CAST` when source type is non-numeric, non-bool
- Narrowing warnings only fire when data loss is likely -- wider-to-narrower where source range exceeds target
- Emit `IRON_WARN_NARROWING_CAST` for risky narrowing
- Bool to Int is allowed (gives 0/1), Int to Bool is NOT allowed (must use explicit comparison)
- `Int8(300)` is a compile-time error (not warning) -- emit `IRON_ERR_CAST_OVERFLOW` when constant value provably doesn't fit
- Implicitly stringifiable types: primitives (Int, Float), String, Bool, enums, and types with `to_string()` method
- Non-stringifiable types fall back to address printing -- emit `IRON_WARN_NOT_STRINGABLE` as warning (not error)
- Compound assignments on narrow integer types emit `IRON_WARN_POSSIBLE_OVERFLOW` when target is narrower than platform int and RHS is not a fitting constant
- When RHS is a constant and provably fits: no warning
- Compound overflow check happens in `typecheck.c` at the assignment level

### Claude's Discretion
- Helper function organization for match exhaustiveness (whether to extract variant collection into a helper)
- Exact narrowing threshold logic (which type pairs trigger vs don't)
- How to detect `to_string()` method presence on user types

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| MATCH-01 | Compiler checks that match statements on enum types cover all variants or have an else clause | Extend `IRON_NODE_MATCH` case in `check_stmt` (typecheck.c:1237); access `Iron_EnumDecl->variants` via resolved subject type `enu.decl` |
| MATCH-02 | Compiler emits `IRON_ERR_NONEXHAUSTIVE_MATCH` listing uncovered variants when coverage is incomplete | New error code in diagnostics.h; format uncovered variant names into message string |
| MATCH-03 | Compiler requires else clause when match subject is not an enum type | Simple check: if subject resolved_type is not IRON_TYPE_ENUM and no else_body, emit error |
| CAST-01 | Compiler validates that source expression type is numeric or bool before allowing primitive cast | Extend the `is_primitive_cast` block at typecheck.c:477; check `arg_resolved_type` against `iron_type_is_numeric()` or `IRON_TYPE_BOOL` |
| CAST-02 | Compiler emits `IRON_ERR_INVALID_CAST` when source type is not castable | New error code; emit when source fails the numeric-or-bool check |
| CAST-03 | Compiler emits `IRON_WARN_NARROWING_CAST` for wider-to-narrower integer casts | New warning code; compare source/target type widths using a `type_bit_width()` helper |
| CAST-04 | Compiler validates compile-time constant values fit in target narrow type | Parse `Iron_IntLit->value` string as int64_t, compare against target type min/max range |
| STRN-01 | Compiler validates that interpolated expression types are stringifiable | Extend `IRON_NODE_INTERP_STRING` case at typecheck.c:305; check each part's resolved_type against stringifiable set |
| STRN-02 | Compiler emits `IRON_WARN_NOT_STRINGABLE` for types without string conversion capability | New warning code; emit for non-stringifiable expression parts (objects without `to_string()`, etc.) |
| OVFL-01 | Compiler detects compound assignments on narrow integer types | Extend `IRON_NODE_ASSIGN` case at typecheck.c:1031; check `as->op` against compound ops |
| OVFL-02 | Compiler emits `IRON_WARN_POSSIBLE_OVERFLOW` when target is narrow and RHS not a fitting constant | New warning code; check target type narrowness and RHS constantness |
| OVFL-03 | Compiler validates compile-time constant RHS values fit in narrow target type | Parse RHS literal value, compare against target type range; suppress warning when fits |
</phase_requirements>

## Architecture Patterns

### Where Each Check Integrates

```
src/analyzer/typecheck.c
    check_stmt()
        case IRON_NODE_MATCH:    +++ match exhaustiveness check
        case IRON_NODE_ASSIGN:   +++ compound overflow check
    check_expr()
        case IRON_NODE_CALL:     +++ cast safety check (in is_primitive_cast block)
        case IRON_NODE_INTERP_STRING: +++ stringability check

src/diagnostics/diagnostics.h
    +++ New error codes (308+) and warning codes (601+)
```

### Pattern 1: Match Exhaustiveness Algorithm

**What:** After checking all match cases and else body, verify coverage completeness.
**When to use:** When `ms->subject` has resolved type `IRON_TYPE_ENUM`.

The algorithm:
1. Get the subject's resolved type. If IRON_TYPE_ENUM, access `type->enu.decl` to get the `Iron_EnumDecl`.
2. Collect all variant names from `decl->variants[i]->name` (cast to `Iron_EnumVariant *`).
3. For each match case, extract the pattern's variant name. Patterns are parsed as expressions -- an enum variant reference resolves to an `IRON_NODE_IDENT` with `resolved_sym->sym_kind == IRON_SYM_ENUM_VARIANT`. The variant name is the ident's `name`.
4. Track seen variants in a boolean array (variant_count is bounded and small). If a variant is seen twice, emit `IRON_ERR_DUPLICATE_MATCH_ARM`.
5. After processing all cases: if `ms->else_body` is NULL, check for unseen variants. If any, emit `IRON_ERR_NONEXHAUSTIVE_MATCH` listing them.
6. For non-enum subject types: if `ms->else_body` is NULL, emit `IRON_ERR_NONEXHAUSTIVE_MATCH` with message "match on non-enum type requires else clause".

**Memory:** Use a stack-allocated `bool covered[variant_count]` array. Enum variant counts are small (typically < 50), so VLA or a fixed-size array with overflow to arena alloc is fine.

```c
// In check_stmt, case IRON_NODE_MATCH:
Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
Iron_Type *subj_type = check_expr(ctx, ms->subject);

// ... existing case/else checking ...

// NEW: Exhaustiveness check
if (subj_type && subj_type->kind == IRON_TYPE_ENUM) {
    Iron_EnumDecl *edecl = subj_type->enu.decl;
    bool *covered = /* stack or arena array of edecl->variant_count */;
    // For each case, match pattern ident name to variant index
    // Detect duplicates, then check for uncovered
} else if (!ms->else_body) {
    // Non-enum without else: error
}
```

### Pattern 2: Cast Safety Validation

**What:** In the `is_primitive_cast` code path (typecheck.c ~477), after verifying the target is numeric/bool, also verify the source argument type.
**When to use:** Every `is_primitive_cast == true` call expression.

The existing code at typecheck.c:455-486 only checks the TARGET type is numeric/bool. We extend to also check the SOURCE:

```c
// After check_expr(ctx, ce->args[0]):
Iron_Type *src_type = /* ce->args[0]->resolved_type */;

// 1. Source must be numeric or bool
if (!iron_type_is_numeric(src_type) && src_type->kind != IRON_TYPE_BOOL) {
    emit_error(ctx, IRON_ERR_INVALID_CAST, ...);
}

// 2. Int->Bool special case: disallowed
if (iron_type_is_integer(src_type) && target_t->kind == IRON_TYPE_BOOL) {
    emit_error(ctx, IRON_ERR_INVALID_CAST, ...);
}

// 3. Narrowing warning: wider source -> narrower target
if (type_bit_width(src_type) > type_bit_width(target_t) &&
    iron_type_is_integer(src_type) && iron_type_is_integer(target_t)) {
    // Check if source is a constant that fits
    if (ce->args[0]->kind == IRON_NODE_INT_LIT) {
        int64_t val = strtoll(((Iron_IntLit*)ce->args[0])->value, NULL, 10);
        if (!value_fits_type(val, target_t)) {
            emit_error(ctx, IRON_ERR_CAST_OVERFLOW, ...);  // Error, not warning
        }
        // else: constant fits, no warning
    } else {
        emit_warning(ctx, IRON_WARN_NARROWING_CAST, ...);
    }
}
```

### Pattern 3: Stringability Check

**What:** For each part of an interpolation string, verify the resolved type is "stringifiable".
**When to use:** Every `IRON_NODE_INTERP_STRING` expression.

Stringifiable types:
- All numeric types (`iron_type_is_numeric()`)
- `IRON_TYPE_BOOL`
- `IRON_TYPE_STRING`
- `IRON_TYPE_ENUM` (variant names)
- `IRON_TYPE_OBJECT` with a `to_string()` method declared

To detect `to_string()` on an object type:
1. Get the object type's `Iron_ObjectDecl *` via `type->object.decl`.
2. Scan `ctx->program->decls` for `IRON_NODE_METHOD_DECL` where `md->type_name` matches the object's name and `md->method_name` is `"to_string"`.

This matches the existing pattern used in `check_expr` for method call resolution (typecheck.c:654-666).

```c
// In check_expr, case IRON_NODE_INTERP_STRING:
for (int i = 0; i < n->part_count; i++) {
    Iron_Type *part_type = check_expr(ctx, n->parts[i]);  // already done
    if (n->parts[i]->kind != IRON_NODE_STRING_LIT) {
        // Check stringability
        if (!is_stringifiable(ctx, part_type)) {
            emit_warning(ctx, IRON_WARN_NOT_STRINGABLE, ...);
        }
    }
}
```

### Pattern 4: Compound Overflow Detection

**What:** When processing an assignment with a compound operator (`+=`, `-=`, `*=`, `/=`), check if the target type is a narrow integer.
**When to use:** Every `IRON_NODE_ASSIGN` where `as->op` is a compound operator.

```c
// In check_stmt, case IRON_NODE_ASSIGN, after existing checks:
if (is_compound_assign_op(as->op) && target_type) {
    if (is_narrow_integer(target_type)) {
        // Check if RHS is a constant that fits
        if (as->value->kind == IRON_NODE_INT_LIT) {
            int64_t val = strtoll(((Iron_IntLit*)as->value)->value, NULL, 10);
            if (value_fits_type(val, target_type)) {
                // No warning: constant fits
            } else {
                emit_warning(ctx, IRON_WARN_POSSIBLE_OVERFLOW, ...);
            }
        } else {
            emit_warning(ctx, IRON_WARN_POSSIBLE_OVERFLOW, ...);
        }
    }
}
```

### Anti-Patterns to Avoid
- **Over-allocating for variant tracking:** Do NOT use a hash map for the covered-variants set in match exhaustiveness. Use a simple boolean array indexed by variant position -- the `Iron_EnumDecl` already provides `variants` as an ordered array with `variant_count`.
- **Duplicating compound-assign detection:** The `is_compound_assign()` function already exists in `hir_lower.c`. Either declare a static equivalent in typecheck.c or extract to a shared header. Do NOT include hir_lower.c internals.
- **Modifying the cast code path structure:** The existing cast path at lines 451-486 has complex control flow for disambiguation. Add source validation AFTER the existing `check_expr(ctx, ce->args[0])` call at line 479, not before, to ensure the argument is typed first.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Type width comparison | Manual bit-width tables inline at each use site | A `static int type_bit_width(Iron_TypeKind kind)` helper | Reused across cast safety and overflow checks; centralizes the Int8=8, Int16=16, etc. mapping |
| Integer range bounds | Inline MIN/MAX constants at each check | A `static bool value_fits_type(int64_t val, Iron_Type *t)` helper | Reused by cast overflow (CAST-04) and compound overflow (OVFL-03); contains all range logic in one place |
| Numeric/bool type predicate | Duplicating the switch from line 457 | `iron_type_is_numeric()` from types.h (already exists) plus `kind == IRON_TYPE_BOOL` | Already in the codebase |
| Compound assign op check | Reimplementing the switch from hir_lower.c | Define `static bool is_compound_assign_op(Iron_OpKind op)` in typecheck.c | Same 4-way check; keep it local since typecheck.c doesn't include hir_lower internals |
| Enum variant name extraction | Casting through multiple intermediate types | Direct cast to `Iron_EnumVariant *` from `edecl->variants[i]` | The parser stores variants as `Iron_EnumVariant *` in the array |

**Key insight:** Most of the building blocks exist in the codebase. The pattern of `emit_error(ctx, code, span, msg, suggestion)` is established. Type predicates like `iron_type_is_numeric()`, `iron_type_is_integer()` exist. The only new helpers needed are `type_bit_width()`, `value_fits_type()`, `is_stringifiable()`, and `is_narrow_integer()`.

## Common Pitfalls

### Pitfall 1: Integer Literal Value Parsing Overflow
**What goes wrong:** `Iron_IntLit->value` is a `const char *` string. Parsing with `atoi()` silently overflows for large values. Using `strtol()` returns `LONG_MAX` on overflow.
**Why it happens:** Iron supports Int64 literals which can exceed 32-bit int range.
**How to avoid:** Use `strtoll()` with `errno` checking to parse into `int64_t`. For unsigned types, use `strtoull()`. Check `errno == ERANGE` after conversion.
**Warning signs:** Cast overflow tests passing for values near INT32_MAX but failing for values near INT64_MAX.

### Pitfall 2: Match Pattern is Not Always a Simple Ident
**What goes wrong:** Assuming match case patterns are always `IRON_NODE_IDENT`. They are parsed as arbitrary expressions -- could be `IRON_NODE_INT_LIT`, `IRON_NODE_FIELD_ACCESS` (for qualified `Enum.VARIANT`), or other expression types.
**Why it happens:** The parser calls `iron_parse_expr(p)` for match patterns (parser.c:987).
**How to avoid:** For enum exhaustiveness: check if pattern is an `IRON_NODE_IDENT` whose `resolved_sym->sym_kind == IRON_SYM_ENUM_VARIANT`. Enum variants are registered in global scope by the resolver (resolve.c:115-124) with `IRON_SYM_ENUM_VARIANT` kind and type set to the parent enum type. For non-ident patterns on enum match subjects, they cannot cover variants.
**Warning signs:** Crash when casting pattern to `Iron_Ident *` for a match on integer literal patterns.

### Pitfall 3: Bool Cast Asymmetry
**What goes wrong:** Allowing Int->Bool cast (should be disallowed) or disallowing Bool->Int cast (should be allowed).
**Why it happens:** The user decision is asymmetric: `Bool->Int` gives 0/1 (allowed), but `Int->Bool` requires explicit comparison `x != 0` (disallowed).
**How to avoid:** In cast validation: if target is `IRON_TYPE_BOOL` and source is any integer type, emit `IRON_ERR_INVALID_CAST` with suggestion "use `x != 0` instead". If source is `IRON_TYPE_BOOL` and target is integer, allow silently.
**Warning signs:** Test cases for `Bool(42)` passing when they should error, or `Int(true)` erroring when it should pass.

### Pitfall 4: Warning vs Error Confusion for String Interpolation
**What goes wrong:** Emitting `IRON_ERR_NOT_STRINGABLE` (error) instead of `IRON_WARN_NOT_STRINGABLE` (warning). The REQUIREMENTS.md says `IRON_ERR_NOT_STRINGABLE` but the CONTEXT.md user decision says warning.
**Why it happens:** Conflict between requirements doc naming convention and user's explicit discussion decision.
**How to avoid:** Follow CONTEXT.md (user decisions take priority). Use `IRON_WARN_NOT_STRINGABLE` as a warning with `IRON_DIAG_WARNING` level. This means non-stringifiable interpolations compile but produce a warning.
**Warning signs:** Valid programs with object interpolation failing to compile when they should only warn.

### Pitfall 5: Enum Variant Ownership Mismatch
**What goes wrong:** A match case uses a variant from a DIFFERENT enum type. The variant name resolves but belongs to another enum.
**Why it happens:** Enum variants are registered in global scope with their parent enum's type. A case pattern `Red` might resolve to `Color.Red` even when matching on a `Direction` enum.
**How to avoid:** When checking enum match cases, verify that the pattern's resolved type (the variant's parent enum type) equals the match subject's resolved type using `iron_type_equals()`.
**Warning signs:** Match exhaustiveness incorrectly passing when variants from different enums are mixed.

### Pitfall 6: Narrowing Cast False Positives
**What goes wrong:** Warning on `Int32->Int` (widening) or `Int->Int` (same width) as narrowing.
**Why it happens:** Not checking directionality of the width comparison.
**How to avoid:** Only warn when `type_bit_width(source) > type_bit_width(target)`. Treat `IRON_TYPE_INT` and `IRON_TYPE_UINT` as platform int width (typically 64 bits since that's the default Iron Int).
**Warning signs:** Warnings on `Int(some_int32_var)` which is widening and should be silent.

## Code Examples

### New Error/Warning Code Definitions (diagnostics.h)

```c
/* Type validation errors (308+ range, after LIR verifier) */
#define IRON_ERR_NONEXHAUSTIVE_MATCH    308
#define IRON_ERR_DUPLICATE_MATCH_ARM    309
#define IRON_ERR_INVALID_CAST           310
#define IRON_ERR_CAST_OVERFLOW          311

/* Type validation warnings (601+ range, after existing 600) */
#define IRON_WARN_NARROWING_CAST        601
#define IRON_WARN_NOT_STRINGABLE        602
#define IRON_WARN_POSSIBLE_OVERFLOW     603
```

### emit_warning Helper (typecheck.c)

```c
static void emit_warning(TypeCtx *ctx, int code, Iron_Span span,
                         const char *msg, const char *suggestion) {
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_WARNING, code, span,
                   iron_arena_strdup(ctx->arena, msg, strlen(msg)),
                   suggestion ? iron_arena_strdup(ctx->arena, suggestion,
                                                   strlen(suggestion)) : NULL);
}
```

### type_bit_width Helper

```c
static int type_bit_width(const Iron_Type *t) {
    if (!t) return 0;
    switch (t->kind) {
        case IRON_TYPE_INT8:   case IRON_TYPE_UINT8:   return 8;
        case IRON_TYPE_INT16:  case IRON_TYPE_UINT16:  return 16;
        case IRON_TYPE_INT32:  case IRON_TYPE_UINT32:  return 32;
        case IRON_TYPE_INT64:  case IRON_TYPE_UINT64:  return 64;
        case IRON_TYPE_INT:    case IRON_TYPE_UINT:    return 64; /* platform int */
        case IRON_TYPE_FLOAT32:                        return 32;
        case IRON_TYPE_FLOAT64: case IRON_TYPE_FLOAT:  return 64;
        case IRON_TYPE_BOOL:                           return 1;
        default:                                       return 0;
    }
}
```

### value_fits_type Helper

```c
static bool value_fits_type(int64_t val, const Iron_Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case IRON_TYPE_INT8:   return val >= -128 && val <= 127;
        case IRON_TYPE_INT16:  return val >= -32768 && val <= 32767;
        case IRON_TYPE_INT32:  return val >= INT32_MIN && val <= INT32_MAX;
        case IRON_TYPE_INT64:  return true; /* int64_t always fits int64_t */
        case IRON_TYPE_INT:    return true; /* platform int = 64-bit */
        case IRON_TYPE_UINT8:  return val >= 0 && val <= 255;
        case IRON_TYPE_UINT16: return val >= 0 && val <= 65535;
        case IRON_TYPE_UINT32: return val >= 0 && (uint64_t)val <= UINT32_MAX;
        case IRON_TYPE_UINT64: return val >= 0; /* negative won't fit unsigned */
        case IRON_TYPE_UINT:   return val >= 0;
        default:               return true;
    }
}
```

### is_narrow_integer Helper

```c
/* "Narrow" = any integer type smaller than platform int (64-bit) */
static bool is_narrow_integer(const Iron_Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case IRON_TYPE_INT8:  case IRON_TYPE_INT16:  case IRON_TYPE_INT32:
        case IRON_TYPE_UINT8: case IRON_TYPE_UINT16: case IRON_TYPE_UINT32:
            return true;
        default:
            return false;
    }
}
```

### is_stringifiable Helper

```c
static bool is_stringifiable(TypeCtx *ctx, const Iron_Type *t) {
    if (!t) return false;
    if (iron_type_is_numeric(t)) return true;
    if (t->kind == IRON_TYPE_BOOL) return true;
    if (t->kind == IRON_TYPE_STRING) return true;
    if (t->kind == IRON_TYPE_ENUM) return true;
    /* Check for to_string() method on object types */
    if (t->kind == IRON_TYPE_OBJECT && t->object.decl && ctx->program) {
        const char *type_name = t->object.decl->name;
        for (int i = 0; i < ctx->program->decl_count; i++) {
            Iron_Node *d = ctx->program->decls[i];
            if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
            Iron_MethodDecl *md = (Iron_MethodDecl *)d;
            if (strcmp(md->type_name, type_name) == 0 &&
                strcmp(md->method_name, "to_string") == 0) {
                return true;
            }
        }
    }
    return false;
}
```

### Test Pattern (Unity)

```c
void test_nonexhaustive_match_enum(void) {
    const char *src =
        "enum Color {\n"
        "  Red,\n"
        "  Green,\n"
        "  Blue\n"
        "}\n"
        "func main() {\n"
        "  val c = Red\n"
        "  match c {\n"
        "    Red { }\n"
        "    Green { }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_NONEXHAUSTIVE_MATCH));
}

void test_exhaustive_match_enum_with_else(void) {
    const char *src =
        "enum Color {\n"
        "  Red,\n"
        "  Green,\n"
        "  Blue\n"
        "}\n"
        "func main() {\n"
        "  val c = Red\n"
        "  match c {\n"
        "    Red { }\n"
        "    else { }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}
```

## Detailed Integration Map

### Match Exhaustiveness (typecheck.c, check_stmt, IRON_NODE_MATCH case ~line 1237)

Current code:
```c
case IRON_NODE_MATCH: {
    Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
    check_expr(ctx, ms->subject);
    for (int i = 0; i < ms->case_count; i++) {
        if (ms->cases[i]) check_stmt(ctx, ms->cases[i]);
    }
    if (ms->else_body) check_stmt(ctx, ms->else_body);
    break;
}
```

New code adds after the existing loop and before `break;`. The subject type is available from `check_expr` return value (need to capture it).

### Cast Safety (typecheck.c, check_expr, IRON_NODE_CALL case ~line 477)

Current code at line 477-485:
```c
if (is_numeric_or_bool) {
    check_expr(ctx, ce->args[0]);
    ce->is_primitive_cast = true;
    result = target_t;
    ce->resolved_type = result;
    callee_id->resolved_type = result;
    break;
}
```

Extend the block between `check_expr(ctx, ce->args[0])` and `break;` to add source validation, narrowing detection, and literal range checking.

### String Interpolation (typecheck.c, check_expr, IRON_NODE_INTERP_STRING case ~line 305)

Current code:
```c
case IRON_NODE_INTERP_STRING: {
    Iron_InterpString *n = (Iron_InterpString *)node;
    for (int i = 0; i < n->part_count; i++) {
        check_expr(ctx, n->parts[i]);
    }
    result = iron_type_make_primitive(IRON_TYPE_STRING);
    n->resolved_type = result;
    break;
}
```

Add stringability check inside the loop, after `check_expr` returns, for parts that are not string literals.

### Compound Overflow (typecheck.c, check_stmt, IRON_NODE_ASSIGN case ~line 1031)

Current code ends at line 1075 with `break;`. Add the compound overflow check before the break, after the existing type mismatch and literal narrowing checks. Access `as->op` to detect compound operators.

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| No match exhaustiveness | Add enum variant coverage check | Phase 33 | Catches missing match arms before they reach C backend |
| No cast source validation | Validate source is numeric/bool | Phase 33 | Prevents `String(x)` or `MyObj(x)` style invalid casts |
| No interpolation type check | Validate stringability | Phase 33 | Warns on non-printable types in string interpolation |
| No compound overflow detection | Warn on narrow-int compound ops | Phase 33 | Catches potential overflow in `Int8 += expr` patterns |

**Existing related work:**
- `is_int_literal_narrowing()` (typecheck.c:138) already handles Int->Int32 literal narrowing -- the cast safety work extends this concept more broadly
- `iron_type_is_numeric()`, `iron_type_is_integer()`, `iron_type_is_float()` (types.c:301-328) are already available
- `IRON_WARN_SPAWN_NO_HANDLE` (typecheck.c:1301) establishes the warning emission pattern

## Open Questions

1. **Enum variant patterns as FieldAccess vs bare Ident**
   - What we know: The resolver registers enum variants as bare names in global scope (`Red`, not `Color.Red`). See resolve.c:115-124.
   - What's unclear: Whether the parser ever produces `IRON_NODE_FIELD_ACCESS` for `Color.Red` in match patterns, or if it always resolves to `IRON_NODE_IDENT` for `Red`.
   - Recommendation: Handle both patterns defensively. For IDENT: check `resolved_sym->sym_kind == IRON_SYM_ENUM_VARIANT`. For FIELD_ACCESS: check if `object` is an IDENT resolving to an IRON_SYM_ENUM and `field` matches a variant name. This covers both syntaxes.

2. **CONTEXT.md mentions "associated data patterns at full depth"**
   - What we know: Current `Iron_EnumVariant` has no associated data fields (only `name`, `has_explicit_value`, `explicit_value`). There is no support for sum-type enum variants like `Color.RGB(r, g, b)` in the AST.
   - What's unclear: Whether this feature exists yet or is aspirational.
   - Recommendation: Implement what the current AST supports (simple variant name matching). Document that associated data pattern matching will need AST extensions when enum associated data is added. For now, each variant is identified purely by name.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Unity (C test framework, linked via `unity` target) |
| Config file | `tests/unit/CMakeLists.txt` |
| Quick run command | `cd /Users/victor/code/worker-1/iron-lang/build && make test_typecheck && ./tests/test_typecheck` |
| Full suite command | `cd /Users/victor/code/worker-1/iron-lang/build && make -j && ctest -L unit --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| MATCH-01 | Match on enum covers all variants or has else | unit | `./tests/test_typecheck` | Extend existing |
| MATCH-02 | Emit IRON_ERR_NONEXHAUSTIVE_MATCH listing uncovered | unit | `./tests/test_typecheck` | Extend existing |
| MATCH-03 | Non-enum match requires else | unit | `./tests/test_typecheck` | Extend existing |
| CAST-01 | Source must be numeric/bool for cast | unit | `./tests/test_typecheck` | Extend existing |
| CAST-02 | Emit IRON_ERR_INVALID_CAST for non-castable | unit | `./tests/test_typecheck` | Extend existing |
| CAST-03 | Emit IRON_WARN_NARROWING_CAST for wide-to-narrow | unit | `./tests/test_typecheck` | Extend existing |
| CAST-04 | Literal range check for cast overflow | unit | `./tests/test_typecheck` | Extend existing |
| STRN-01 | Validate interpolated types are stringifiable | unit | `./tests/test_typecheck` | Extend existing |
| STRN-02 | Emit IRON_WARN_NOT_STRINGABLE | unit | `./tests/test_typecheck` | Extend existing |
| OVFL-01 | Detect compound assign on narrow int | unit | `./tests/test_typecheck` | Extend existing |
| OVFL-02 | Emit IRON_WARN_POSSIBLE_OVERFLOW | unit | `./tests/test_typecheck` | Extend existing |
| OVFL-03 | Constant RHS fits check suppresses warning | unit | `./tests/test_typecheck` | Extend existing |

### Sampling Rate
- **Per task commit:** `cd /Users/victor/code/worker-1/iron-lang/build && make test_typecheck && ./tests/test_typecheck`
- **Per wave merge:** `cd /Users/victor/code/worker-1/iron-lang/build && make -j && ctest -L unit --output-on-failure`
- **Phase gate:** Full unit suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] Add new test functions to `tests/unit/test_typecheck.c` for each new diagnostic (12 requirements = minimum 24 tests: 12 trigger + 12 no-false-positive)
- [ ] Add new error/warning codes to `diagnostics.h` before test compilation
- [ ] Verify `has_error()` helper works for warning codes too (check `g_diags.items[i].code` matches warning codes -- confirmed: it checks all items regardless of level)

## Sources

### Primary (HIGH confidence)
- `src/analyzer/typecheck.c` -- read directly, all integration points verified at exact line numbers
- `src/analyzer/types.h` / `types.c` -- type system API, predicates, enum type structure
- `src/parser/ast.h` -- AST node structures for match, call (cast), interp string, assign
- `src/diagnostics/diagnostics.h` -- existing error codes, diagnostic emission API
- `src/analyzer/scope.h` -- symbol kinds including IRON_SYM_ENUM_VARIANT
- `src/analyzer/resolve.c` -- enum variant registration in global scope
- `src/hir/hir_lower.c` -- compound assignment detection pattern
- `src/parser/parser.c` -- match case pattern parsing (expression-based)
- `tests/unit/test_typecheck.c` -- existing test patterns, parse_and_resolve helper

### Secondary (MEDIUM confidence)
- `tests/unit/test_resolver.c` -- enum resolution test patterns

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all code is C within existing codebase, no external libraries
- Architecture: HIGH -- all integration points verified by reading exact source lines
- Pitfalls: HIGH -- identified from actual code structures (string-based int literals, expression-based match patterns, asymmetric bool casting)

**Research date:** 2026-04-02
**Valid until:** Indefinite (compiler internals, not external dependencies)
