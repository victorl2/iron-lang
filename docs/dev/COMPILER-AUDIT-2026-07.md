# Iron Compiler — Correctness, Robustness & Documentation Audit

**Date:** 2026-07-17
**Version audited:** `iron 4.0.0-alpha (982f6c04)`, `main` branch
**Method:** Latest compiler built and installed inside an `ubuntu:24.04` Docker container capped at **8 GB RAM** (`--memory=8g --memory-swap=8g`). All Iron programs were compiled and executed only inside that container. Findings combine (a) hands-on execution of ~35 Iron programs, (b) the full `ctest` suite, and (c) deep static review of every compiler layer by independent reviewers.

> **Scope note on platform.** The sandbox is arm64 (Apple Silicon). The v4 release notes flag a pre-existing "arm64 user-method-call codegen loop"; in practice user method calls **did** run correctly here (see §2), so that caveat appears stale or narrower than described. Where a finding could be platform-specific it is marked; the great majority are platform-independent (confirmed by source review).

---

## 0. Executive summary

Iron v4.0.0-alpha has a **solid, working core** — primitives, control flow, objects/methods/tiers, inheritance, interfaces, enums+ADTs+`match`, generic *enums*, `comptime`, `heap`/`defer free`/`drop`, `rc`, bounded vectors, and the runtime safety guards (use-after-free trap, bounded-vector OOB panic) all execute correctly. The memory-model machinery that is the headline of v4 genuinely functions for the straight-line cases.

However, the audit surfaced **serious correctness and safety defects**, **broken documented features**, and **documentation that is a full major version out of date**. The most important issues:

| # | Severity | Area | One-line |
|---|----------|------|----------|
| F1 | **Critical** | Runtime safety | Integer `/` and `%` by zero are unguarded → silent UB, wrong result, exit 0 |
| F2 | **Critical** | Codegen (HIR) | `return` runs `defer`/`drop`/`rc_release` **before** evaluating the return expression → use-after-free / use-after-destruct on `return rc x` and `return <droppable>` |
| F3 | **Critical** | Codegen (HIR) | `match`-arm `defer`/`drop` register in the *enclosing* scope → cleanup runs unconditionally, destructors fire on uninitialized memory |
| F4 | **Critical** | Codegen (HIR) | Reference-count retain/release is imbalanced across calls/returns/reassignment → leaks and destructors that never run |
| F5 | **Critical** | Frontend robustness | `import <256-char-segment>` stack-overflows; self-referential `object A { val x: A }` stack-overflows the compiler (no cycle detection) |
| F6 | **High** | Language usability | Generic **objects** (`object Pair[T]`), nullable widening (`Enemy` → `Enemy?`), multi-return, and `List`/`Map`/`Set` are documented but broken/unusable |
| F7 | **High** | Codegen | `s[i]` indexing, `for c in string`, `x is T`, and `arena` allocation all emit C that does not compile |
| F8 | **High** | Type soundness | Whole classes of checks are missing (unknown method → Void, `1 < "x"` ok, null-narrowing leaks across functions) |
| F9 | **High** | Tests | `IRON_CURRENT_PHASE=15` freezes ~194 v4 fixtures as permanent XFAIL; the "sanitizer" CI job runs no sanitizers; `compile_fail/` is never executed |
| F10 | **High** | Docs | `language_definition.md` + entire website describe v3 (deny pointers exist, promise auto-free, omit `drop`/`nocopy`/`weak`/`arena`/`Box`); INSTALL tells v4 users their checkout is stale |

**Bottom line for action:** the language is a promising alpha with a working spine, but (1) a handful of codegen-ordering and RC-balance bugs make `rc`/`drop`/`return` unsound for real programs, (2) several flagship documented features don't work, and (3) the docs must not ship as-is. None of these are architectural dead-ends; they are concrete, localized, and fixable.

---

## 1. How the evaluation was run

```bash
# 8 GB-capped sandbox, repo mounted read-only-ish
docker run -d --name iron-sbx --memory=8g --memory-swap=8g -v <repo>:/work iron-sandbox
docker exec iron-sbx cmake -S /work -B /build -DCMAKE_BUILD_TYPE=Release -G Ninja
docker exec iron-sbx cmake --build /build          # ~2800 targets
docker exec iron-sbx cmake --install /build --prefix /opt/iron
/opt/iron/bin/iron --version   # iron 4.0.0-alpha (982f6c04 2026-07-17)
```

Build note: the Release build **fails** out-of-the-box on a clean Ubuntu because `test_string_intern_race` links `-fsanitize=thread` but `libclang_rt.tsan` is not present in the base `clang` package; installing `libclang-rt-18-dev` fixed it. INSTALL.md does not mention this dependency. *(Low, but a first-run papercut.)*

All programs were run with `iron run` (and `--no-optimize` / default `-O3` for A/B checks).

---

## 2. Hands-on execution results

35 programs were written to exercise the documented surface. Summary matrix:

### Works correctly ✅
- Primitives, integer/float arithmetic, `%`, comparisons, `and`/`or`/`not`
- `if`/`elif`/`else`, `while`, `for i in range(n)`, nested loops
- Functions, recursion, multiple params, return
- `object` + `init` + instance methods + `readonly`/`pure` tiers; **user method calls execute fine on arm64** (contra the release-note caveat)
- Single inheritance (`extends`), `super`, method override
- Interfaces via **`impl`** (not `implements`) + polymorphism
- `enum` plain + ADT payloads + `match ... ->` + exhaustiveness; **generic enums** (`Option[T]`)
- `comptime fib(20)` baked at compile time
- `heap` + `defer free` + `drop` (correct LIFO teardown), `rc` + `drop` (correct single drop at refcount 0)
- Bounded vector `[Int; <=N]` incl. **runtime OOB panic**; **use-after-free runtime trap** fires on `free` then deref
- `nocopy` rejects aliasing (E0286); `readonly` rejects mutation (E0238/E0266)
- `Mutex[T]` scoped guard, bounded `Channel[T]`
- `Math.*` and `IO.*` stdlib; string methods (`.upper`/`.split`/`.contains`/`.to_int`/`.repeat` …)
- `defer` LIFO ordering; lambdas + `var`-capture-by-reference

### Broken / does not work as documented ❌
| Program | Documented? | Result |
|---|---|---|
| `10 / 0`, `10 % 0` | safety pitch | **`z=2`, exit 0** — silent UB (F1) |
| `object Pair[T] { val a: T }` | §Generics | `E0202 unknown type 'T'` — generic **objects** unusable (F6) |
| `var t: Enemy? = null; t = Enemy(50)` | §Nullable | `E0202 expected 'Enemy?', got 'Enemy'` — no `T`→`T?` widening (F6) |
| `func f() -> Float, Err?` | §Multiple Return | `E0101 unexpected token` — multi-return doesn't parse (F6) |
| `List[Int]()` / `List.new()` | §Collections | `undefined identifier 'List'` (F6) |
| `Map[String,Int]()` | §Collections | `E0101 unexpected token` at the comma (F6) |
| `Set.new()` | §Collections | types as `Void` → codegen `Iron_unknown_add` (F6) |
| `s[0]`, `for c in "abc"` | §Strings | generated C: `no member 'count'`, `Iron_String_get` undeclared (F7) |
| `out = out + c` (c from `for c in s`) | §Strings | `E0202 operator + requires … two String` — loop char not typed `String` |
| `range(2, 5)` | §Control Flow | `E0216 expected 1 argument(s), got 2` — 2-arg range rejected |
| `match x { A { ... } }` (brace arms) | §Control Flow | removed; only `->` arms parse (doc self-inconsistent) |
| `arena { heap(in: a) T() }` | §Arena / release notes | generated C: `Iron_Arena` vs `Iron_Arena_RT *` → won't compile (F7) |
| `x is Circle` | §`is` | lowered to LIR **poison** → undeclared C identifier (F7) |
| `import math; math.PI` | §math | doc says `math.`; only `Math.PI` works (doc mismatch) |
| `import io; io.read_file` | §io | doc says `io.`; only `IO.read_file` works (doc mismatch) |

**Takeaway:** the *value-type* core is strong; the *collection*, *generic-object*, *nullable*, *multi-return*, and *string-indexing* surfaces — all prominently documented — range from partly-broken to entirely non-functional.

---

## 3. Test-suite results

### 3.1 `ctest` — parallel vs serial
- `ctest -j8`: **87% pass, 53 fail / 412.**
- `ctest -j1` (serial): **99% pass, 1 fail / 412 — the only failure is `benchmark_smoke`** (a timeout on slow/constrained runners, already documented as a known non-defect in the v4 release notes).

The 53 parallel failures are therefore **not** real regressions: run serially (or individually) they pass — e.g. `v4_7_drop_happy` produces byte-identical expected output. The cause is **resource pressure** — up to 8 concurrent `clang -O3` compilations of generated C under the 8 GB cap trigger OOM kills / transient failures. This is itself an **infrastructure finding**: the suite is flaky under memory-constrained parallelism, and CI green depends on runner size. (Temp files use `mkstemp`, so collisions are ruled out; the cause is memory, not path clashes.)

### 3.2 Structural gaps (from test-coverage review)
- **`IRON_CURRENT_PHASE=15` is frozen.** ~194 of ~218 hand-written v4 memory-model fixtures carry `@expected-pass-after: phase-16..37`, so every failure mode is classified **XFAIL** and cannot fail CI — even though those phases shipped. The "v4-acceptance gate" effectively guards only the migrated-v3 corpus. **Highest-ROI fix in the repo: advance the baseline or strip stale markers.**
- **`tests/compile_fail/` is never executed** by any runner/CMake/workflow, yet still receives new fixtures.
- **The PR "sanitizer" job runs no sanitizers** (`ci.yml` names the step "Debug with ASan/UBSan" but passes only `-DCMAKE_BUILD_TYPE=Debug`; `IRON_ENABLE_SANITIZERS` defaults OFF). The compiled Iron runtime (rc atomics, channels, thread pool, arena) **never** runs under ASan/UBSan/TSan.
- Diagnostics asserted mostly by **error code only** (no span/message/line checks). Web target verified to **"artifact exists,"** never executed. No runtime overflow / div-zero / Unicode-iteration / generics-negative fixtures in any runnable corpus.

---

## 4. Compiler implementation review (static)

Independent reviewers covered the frontend (lexer/parser/analyzer/comptime/diagnostics/util), the HIR lowering layer, and the runtime + stdlib C. The LIR/codegen-optimizer layer was only partially covered (review interrupted) — **see §7 open items.**

### 4.1 Critical — codegen / lowering (HIR)
- **F2 — return-before-cleanup ordering.** `hir_to_lir.c` (`IRON_HIR_STMT_RETURN`) emits scope cleanup (`defer`, `rc_release`, `drop`) and *then* lowers the return expression. `return rc x` releases `x` (refcount → 0, freed) and returns the freed pointer; `return <droppable>` destructs before the caller reads it; `defer { x = 99 }; return x` returns 99. **Use-after-free / wrong value. Certain.**
- **F3 — match-arm cleanup leaks into the enclosing scope.** `MATCH` arms are lowered without a defer/scope push, so a `defer` or droppable `val` inside one arm registers on the enclosing scope and runs **regardless of which arm executed** — including running a destructor on a never-initialized alloca. **Certain.**
- **F4 — RC retain/release imbalance.** Multiple paths: rc call-arguments are retained but callee params never released (+1/call); rc return values double-retained; `var r = rc T()` never released at scope exit; rc reassignment never releases the old value. **Long-running programs leak unboundedly and `drop` side effects (files/locks) never finalize. Certain.**
- **Wrong-result lowering (High):** match scrutinee re-evaluated once per binding (side effects duplicated, payloads read from a different value); **bool/string `match` collapses to the switch default so the last arm always wins**; compound assignment (`a[f()] += 1`) evaluates the target twice; range-for end bound re-evaluated every iteration (side-effecting/`len()` bounds → wrong count or infinite loop); lazy global lowering makes multi-function programs that share a global fail to compile or silently reinitialize the global each loop iteration.

### 4.2 Critical/High — frontend robustness
- **F5 — unbounded/again-recursive inputs crash the compiler.** `parser.c` copies the first `import` path segment into a fixed `char[256]` with an unchecked `memcpy` (stack smash on a long segment). `typecheck.c compute_has_user_copy_transitive` sets its memo flag *after* recursing, so `object A { val x: A }` (or mutual recursion) infinitely recurses — `IRON_ERR_CIRCULAR_TYPE` (223) is defined but **never emitted**. Tuple-destructure `val (a,b) = e` sets `name = NULL`, which three analyzer passes feed to `shput`/`strcmp` → segfault. `snprintf`-accumulation in type-stringification (`types.c`, `typecheck.c` exhaustiveness messages) overflows fixed buffers when the printed type is long. **All certain, all reachable from small programs.**
- **F8 — type-system soundness holes (High).** Comparison/logical operands unvalidated (`1 < "hello"`, `obj == 3.5` typecheck); unknown method calls accepted **silently as `Void`** (`IRON_ERR_NO_SUCH_METHOD` never emitted); top-level `val g: Int = "hello"` not checked; **flow-narrowing is name-keyed, persists across function boundaries, and is never invalidated on reassignment** → a nullable can read as non-null in a different function or after `x = null`, defeating the null-safety guarantee; generic-enum payloads unchecked; `elif` branches skipped by narrowing (leaves `resolved_type == NULL` → downstream crash risk). Lambda capture-set computation misses several node kinds → miscompiled captures.

### 4.3 High/Medium — runtime & stdlib C
- **Dynamic `List[T]` indexing/`set`/`pop` are unchecked** (`iron_runtime.h`): `_get` returns `items[index]` with no bounds check, `_pop` does `items[--count]` with no emptiness guard (empty pop reads `items[-1]`, corrupts `count` to −1, next `push` writes OOB). Bounded vectors panic on OOB but dynamic lists do not — inconsistent with the safety posture. **High.** (Note: this compounds F6 — the collections are both hard to *use* and unsafe when used.)
- `Iron_string_repeat` size math (`len*n`, `total+1`) is unguarded → heap overflow for very large `n`. String search/split/replace use `strstr` and single-byte `char_at`/`substring`, so embedded-NUL and multi-byte UTF-8 content is silently mis-processed (correctness, not memory-safety). `count_codepoints` miscounts malformed UTF-8.
- **Positives:** container growth is overflow-guarded (`iron_oom_abort`), the generational-pointer machinery, debug-allocator quarantine, string interning, and the networking/raylib copy-shims are all sound and unusually well-commented.

### 4.4 Codegen build-flags (High)
- Emitted C is compiled with **`-O3` by default** and **`-O2` only under `--release`** (an inversion — `build.c` last-wins argv). Neither passes **`-fwrapv`**, so signed-overflow and the div-by-zero above are **UB the optimizer is free to exploit**. For a language marketed on predictability this is a defensible-but-surprising posture that should be documented and, ideally, hardened (`-fwrapv` + a div-by-zero guard or a defined trap).

---

## 5. Documentation audit

**The user-facing documentation is a full major version behind the compiler.**

- **`docs/language_definition.md` is the v3 spec, unchanged for v4.** It states *"There are no pointer types in the language"* (v4 has `*T`/`*var T`/`*unchecked T`), promises *"heap values … are automatically freed at block exit"* (v4 requires explicit `free`/`defer free`), and **omits every headline v4 feature**: `drop`/`copy`/`nocopy`, `weak rc`, `arena`, `Box[T]`, `*unchecked`, bounded vectors. It uses **`implements`** (real keyword is `impl`), documents removed brace-arm `match`, and cites non-existent error codes `E03F1`/`E03F2`. 10 implemented keywords (`copy`, `drop`, `nocopy`, `unchecked`, `weak`, `impl`, `extern`, `in`, `mut`, `private`) are undocumented.
- **`INSTALL.md` actively misleads:** it claims `iron --version` prints `3.1.1-alpha` and that "if the version does not start with `3.1.`, your checkout is out of date" — so it tells every current (4.0.0) user their checkout is stale.
- **The website (`docs/site/`) has zero v4 content.** Hero says "Latest: v3.2.0-alpha" (and the pages.yml version-injection is a silent no-op — the placeholder is absent from the HTML). The site's language reference still teaches `implements`, "No Pointers," and auto-free.
- **The flagship v4 migration guide is self-defeating:** it invents a v3 that never existed (`let`, `let mut`, `try/finally` — none are Iron keywords in any version) and demonstrates v4 with `Type(field: value)` **named-argument construction that the release notes themselves call a compile error**. It is also unreachable from the site nav.
- **`docs/examples/game.iron` compiles under no released version** (removed `func Type.method` form, missing `init`, non-existent raylib free-functions, unimplemented `draw {}` block).
- All three **editor READMEs document the inverse of their shipped compat range** (`>= 3.0.0, < 4.0.0` in prose vs `>= 4.0.0, < 5.0.0` in the actual configs).
- **`CHANGELOG.md` has no v4 entry** despite an auto-generation claim; `docs/versioning.md`'s milestone table is five majors stale.
- **Packaging risk (verify):** `release.yml` stages only `runtime/util/stdlib/vendor` into the install tarball — **not `src/diagnostics/`**, which `iron_runtime.h` includes on every user build. This is the exact `#47`/`v3.1.1` "missing `lib/diagnostics/`" bug, and `install-smoke.yml` tests `cmake --install` (which *is* fixed), so the canary cannot catch the divergence. **Confirm the actual v4.0.0-alpha release asset.**

Orphaned/internal docs shipping in-repo: `docs/v005-ir-optimization-spec.md`, `benchmark_report.md` (v0.0.7 snapshot), `suggested_performance_improvements.md`, `implementation_plan.md`, `static-interface-dispatch-spec.md`, plus ~15 `docs/dev/PHASE-*` closeouts.

---

## 6. Prioritized action list

### P0 — correctness/safety (block a beta)
1. **Guard integer `/` and `%` by zero** (runtime trap or defined behavior); add `-fwrapv` to the clang invocation. *(F1, §4.4)*
2. **Fix `return` cleanup ordering** — evaluate the return expression, retain/copy out, *then* run `defer`/`drop`/`rc_release`. *(F2)*
3. **Scope `match`-arm `defer`/`drop`** like `if`/`while`/`for` bodies. *(F3)*
4. **Audit and balance rc retain/release** across call args, returns, `var` bindings, and reassignment; add a leak/`drop`-count test that runs under a sanitizer. *(F4)*
5. **Bound the parser `import` buffer; add recursive-type cycle detection (emit E223); NULL-guard tuple-destructure names; fix `snprintf` accumulation.** *(F5)*
6. **Bounds-check dynamic `List` index/`set`/`pop`.** *(§4.3)*

### P1 — make documented features real (or cut them from docs)
7. Generic **objects**; nullable `T`→`T?` widening; multi-return parsing; `List`/`Map`/`Set` usability; `s[i]`/`for c in string`/`is`/`arena` codegen. Any that won't make the milestone should be **removed from the language reference** so the docs stop describing vaporware. *(F6, F7)*
8. Close the type-soundness holes: emit `NO_SUCH_METHOD`, validate comparison/logical operands, check top-level initializers, make null-narrowing symbol-keyed + function-local + assignment-invalidated. *(F8)*

### P2 — test integrity
9. **Advance `IRON_CURRENT_PHASE`** (or delete stale `@expected-pass-after`), **register `tests/compile_fail/`**, and **actually enable sanitizers** in the PR job (and run the Iron runtime under ASan/UBSan/TSan). Three small changes, large coverage gain. *(F9)*
10. Make the suite robust under memory-capped parallelism (bound `ctest` concurrency to available RAM, or cap concurrent `clang`). *(§3.1)*

### P3 — documentation
11. Rewrite `language_definition.md` for v4; fix `INSTALL.md`/site version strings and editor READMEs; regenerate `CHANGELOG`; rewrite the migration guide's examples against real fixtures; **verify the release tarball ships `lib/diagnostics/`.** *(§5)*
12. Prune orphaned/internal docs from the shipping set.

---

## 7. Open items (not fully covered)
- **LIR / `emit_c.c` / optimizer soundness** (value-range narrowing, RC-pair elimination, pointer-check elision, loop fusion, layout analysis, name-mangling collisions, switch case-width truncation, string-literal escaping) was only reviewed indirectly. A dedicated pass is recommended before trusting `--release` optimizations. Known signals from adjacent layers: `case` values are truncated `int64`→`int` (`match` on labels ≥ 2³²), and poison/undef can reach emitted C as undeclared identifiers.
- The **arm64 build+run loop** described in the release notes did not reproduce for user method calls here; worth reconciling the note with current behavior.
