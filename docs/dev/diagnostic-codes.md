# Iron diagnostic code registry

This is the complete registry for diagnostics produced by the compiler and
language server. The definitions in `src/diagnostics/diagnostics.h` are the
source of truth for allocation. `scripts/check_diagnostic_docs.py` requires
each definition to have exactly one row here, rejects duplicate numbers, and
rejects documentation references to unknown codes.

`emitted` means a current compiler path can produce the code. `reserved`
keeps a number allocated for an internal check that is not currently reached.
`retired` keeps compatibility with an older diagnostic and must not be reused.
The example column gives a minimal trigger or, for compiler-bug diagnostics,
the invalid internal state being reported.

| Code | Source symbol | Meaning | Example | Status |
|---|---|---|---|---|
| E0001 | `IRON_ERR_UNTERMINATED_STRING` | String literal has no closing quote. | `val s = "open` | emitted |
| E0002 | `IRON_ERR_INVALID_CHAR` | Source contains an invalid character or a C-style logical operator. | `true && false` | emitted |
| E0003 | `IRON_ERR_INVALID_NUMBER` | Numeric literal has invalid digits or a suffix. | `0xGG` | emitted |
| E0004 | `IRON_ERR_LEXER_OOM` | Lexer could not allocate memory. | Lexing when the arena allocator fails. | emitted |
| E0005 | `IRON_ERR_STRING_TOO_LONG` | One string literal exceeds the lexer limit. | A string literal longer than 4095 bytes. | emitted |
| E0101 | `IRON_ERR_UNEXPECTED_TOKEN` | Token is not valid in its grammar position. | `}` at top level. | emitted |
| E0102 | `IRON_ERR_EXPECTED_EXPR` | Parser expected an expression. | `val x =` | emitted |
| E0103 | `IRON_ERR_EXPECTED_RBRACE` | Block is missing `}`. | `func main() {` | emitted |
| E0104 | `IRON_ERR_EXPECTED_RPAREN` | Parenthesized form is missing `)`. | `println("x"` | emitted |
| E0105 | `IRON_ERR_EXPECTED_COLON` | A required colon is missing. | `val x Int = 1` | emitted |
| E0106 | `IRON_ERR_EXPECTED_ARROW` | A match arm is missing `->`. | `match x { A 1 }` | emitted |
| E0107 | `IRON_ERR_PARSE_DEPTH_EXCEEDED` | Syntax nesting exceeds the parser recursion limit. | Thousands of nested parentheses. | emitted |
| E0175 | `IRON_ERR_KEYWORD_NOT_BINDING_NAME` | Reserved keyword used as a binding name. | `val copy = 1` | emitted |
| E0176 | `IRON_ERR_MISSING_VAL_VAR` | Binding or field omits `val` or `var`. | `count: Int = 0` in an object. | emitted |
| E0200 | `IRON_ERR_UNDEFINED_VAR` | Identifier has no declaration in scope. | `println(missing)` | emitted |
| E0201 | `IRON_ERR_DUPLICATE_DECL` | Name is declared more than once in the package namespace. | Two files both declare `func run() {}`. | emitted |
| E0202 | `IRON_ERR_TYPE_MISMATCH` | Expression does not have the required type. | `val n: Int = "x"` | emitted |
| E0203 | `IRON_ERR_VAL_REASSIGN` | Immutable binding is reassigned. | `val n = 1` followed by `n = 2`. | emitted |
| E0204 | `IRON_ERR_NULLABLE_ACCESS` | Nullable value is used without narrowing. | Read `p.x` where `p: Point?`. | emitted |
| E0205 | `IRON_ERR_MISSING_IFACE_METHOD` | Interface implementation omits a required method. | `object P impl Printable` without `to_string`. | emitted |
| E0206 | `IRON_ERR_GENERIC_CONSTRAINT` | Type argument does not satisfy its constraint. | Use a non-Hashable object as a map key. | emitted |
| E0207 | `IRON_ERR_ESCAPE_NO_FREE` | Heap allocation escapes without explicit ownership action. | Return `heap P()` without `leak`. | emitted |
| E0208 | `IRON_ERR_PARALLEL_MUTATION` | Parallel loop mutates an outer binding. | Increment an outer `var` in `for ... parallel`. | emitted |
| E0209 | `IRON_ERR_IMPORT_NOT_FOUND` | Imported module or package path does not exist. | `import missing_module` | emitted |
| E0210 | `IRON_ERR_SELF_OUTSIDE_METHOD` | `self` is used outside a method. | `func f() { println(self) }` | emitted |
| E0211 | `IRON_ERR_SUPER_NO_PARENT` | `super` is used where there is no parent object. | Use `super.f()` in a base object. | emitted |
| E0212 | `IRON_ERR_FREE_NON_HEAP` | `free` targets a value not allocated with `heap`. | `val p = P()` then `free p`. | emitted |
| E0213 | `IRON_ERR_LEAK_NON_HEAP` | `leak` targets a non-heap value. | `val p = P()` then `leak p`. | emitted |
| E0214 | `IRON_ERR_LEAK_RC` | `leak` targets an `rc` value. | `val p = rc P()` then `leak p`. | emitted |
| E0215 | `IRON_ERR_RETURN_TYPE` | Return expression has the wrong type. | Return `"x"` from `func f() -> Int`. | emitted |
| E0216 | `IRON_ERR_ARG_COUNT` | Call has the wrong number of arguments. | Call `f()` when `f` requires one argument. | emitted |
| E0217 | `IRON_ERR_ARG_TYPE` | Call argument has the wrong type. | Pass `String` to an `Int` parameter. | emitted |
| E0218 | `IRON_ERR_NOT_CALLABLE` | Expression cannot be called. | `val n = 1` then `n()`. | emitted |
| E0219 | `IRON_ERR_NO_SUCH_FIELD` | Type has no requested field. | Read `p.missing`. | emitted |
| E0220 | `IRON_ERR_NO_SUCH_METHOD` | Type has no requested method. | Call `p.missing()`, or index a collection through a pointer. | emitted |
| E0221 | `IRON_ERR_PRIVATE_ACCESS` | Legacy private-access diagnostic. | No current trigger; cross-file access uses E0320. | retired |
| E0222 | `IRON_ERR_NUMERIC_CONVERSION` | Numeric kinds cannot be mixed implicitly. | Add an `Int` and a `Float`. | emitted |
| E0223 | `IRON_ERR_CIRCULAR_TYPE` | By-value type recursively contains itself. | `object Node { val next: Node }` | emitted |
| E0224 | `IRON_ERR_NONEXHAUSTIVE_MATCH` | Match does not cover every variant. | Match one case of a two-case enum without `else`. | emitted |
| E0225 | `IRON_ERR_PATTERN_ARITY` | Pattern payload count differs from the variant. | Match `Some(a, b)` when `Some` has one payload. | emitted |
| E0226 | `IRON_ERR_UNREACHABLE_ARM` | Earlier match arm already covers this arm. | Put a variant arm after `else`. | emitted |
| E0227 | `IRON_ERR_BINDING_SHADOWS` | Pattern binding would shadow an existing name. | Bind `x` in a pattern when `x` is already in scope. | emitted |
| E0228 | `IRON_ERR_UNKNOWN_VARIANT` | Enum variant name does not exist. | Construct `Color.Purple` when it is undeclared. | emitted |
| E0229 | `IRON_ERR_EMPTY_LITERAL_NO_TYPE` | Empty list has no element type from context. | `val xs = []` | emitted |
| E0230 | `IRON_ERR_COMPTIME_STEP_LIMIT` | Compile-time evaluation exceeds its step budget. | An unbounded comptime loop. | emitted |
| E0231 | `IRON_ERR_COMPTIME_RESTRICTION` | Construct is unavailable during comptime evaluation. | Call `.push()` in a comptime function. | emitted |
| E0232 | `IRON_ERR_COMPTIME_ERROR` | Compile-time evaluation failed. | Divide by zero in a comptime expression. | emitted |
| E0233 | `IRON_ERR_BITWISE_NON_INT` | Bitwise operator receives a non-integer. | `true & false` | emitted |
| E0234 | `IRON_ERR_MUT_FIELD_IMMUT_RECV` | Field write uses an immutable receiver. | Assign `p.x` where `p` is `val`. | emitted |
| E0235 | `IRON_ERR_MUT_CALL_ON_VAL` | Mutating method is called on an immutable value. | Call `p.move()` where `p` is `val`. | emitted |
| E0236 | `IRON_ERR_MUT_ON_PRIMITIVE` | `mut` receiver form is invalid for a primitive. | Declare a mutating receiver on `Int`. | emitted |
| E0237 | `IRON_ERR_ACCESSOR_NAME_RESERVED` | Method collides with a generated accessor or destructor name. | Declare `drop` as an ordinary method. | emitted |
| E0238 | `IRON_ERR_READONLY_WRITE_SELF` | Readonly method writes `self`. | Assign `self.x` in a `readonly func`. | emitted |
| E0239 | `IRON_ERR_READONLY_CALLS_MUTATING` | Readonly method calls a mutating method. | Call `self.reset()` from a readonly method. | emitted |
| E0240 | `IRON_ERR_PURE_IO` | Pure method performs I/O. | Call `println` in a `pure func`. | emitted |
| E0241 | `IRON_ERR_PURE_MUTABLE_GLOBAL` | Pure method reads mutable global state. | Read a top-level `var` in a pure method. | emitted |
| E0242 | `IRON_ERR_PURE_NON_PURE_CALL` | Pure method calls a non-pure function. | Call an ordinary method from a pure method. | emitted |
| E0243 | `IRON_ERR_PURE_PARAM_WRITE` | Pure method writes a parameter. | Assign a `var` parameter in a pure method. | emitted |
| E0244 | `IRON_ERR_PURE_WRITE_SELF` | Pure method writes `self`. | Assign `self.x` in a pure method. | emitted |
| E0245 | `IRON_ERR_TIER_MODIFIER_PLACEMENT` | `static`, `readonly` or `pure` is misplaced or combined illegally. | Write `readonly func` at top level. | emitted |
| E0246 | `IRON_ERR_INIT_READ_BEFORE_ASSIGN` | `init` reads a field before assigning it. | Read `self.x` before its initialization. | emitted |
| E0247 | `IRON_ERR_INIT_UNASSIGNED_EXIT` | `init` can exit with an unassigned field. | Omit assignment to a required field. | emitted |
| E0248 | `IRON_ERR_INIT_VAL_DOUBLE_ASSIGN` | `init` assigns a `val` field twice. | Set `self.id` on two paths that both run. | emitted |
| E0249 | `IRON_ERR_INIT_METHOD_ON_PARTIAL` | Method is called on partially initialized `self`. | Call `self.check()` before all fields are assigned. | emitted |
| E0250 | `IRON_ERR_INIT_EARLY_RETURN` | `init` returns before initialization completes. | `return` before assigning all fields. | emitted |
| E0251 | `IRON_ERR_INIT_DELEGATION` | Initializer delegation form is invalid. | Delegate after assigning a field. | emitted |
| E0252 | `IRON_ERR_INIT_RETURN_VALUE` | Initializer returns a value. | `return 1` in `init`. | emitted |
| E0253 | `IRON_ERR_PATCH_ADDS_FIELD` | Patch attempts to add storage. | Put `var x: Int` in `patch object P`. | emitted |
| E0254 | `IRON_ERR_PATCH_TARGET_NOT_FOUND` | Patch names no object. | `patch object Missing {}` | emitted |
| E0255 | `IRON_ERR_PATCH_CONFLICT` | Patch redeclares an existing member. | Two patches define the same method. | emitted |
| E0256 | `IRON_ERR_IFACE_CANNOT_DECLARE_INIT` | Interface declares an initializer. | Put `init()` in an interface. | emitted |
| E0257 | `IRON_ERR_IFACE_METHOD_TIER_MISMATCH` | Implementor weakens an interface method tier. | Ordinary implementation of a pure interface method. | emitted |
| E0258 | `IRON_ERR_IFACE_CONFORMANCE_MISSING` | Superseded conformance diagnostic. | No current trigger; missing methods use E0205. | retired |
| E0259 | `IRON_ERR_SELF_OUTSIDE_CONTEXT` | `Self` is used outside an object or interface context. | Return `Self` from a top-level function. | emitted |
| E0260 | `IRON_ERR_V3_RECEIVER_SYNTAX` | Removed v3 receiver syntax was used. | Use the old standalone receiver form. | emitted |
| E0261 | `IRON_ERR_V3_MUT_RECEIVER` | Removed `mut` receiver spelling was used. | Write an old `mut self` declaration. | emitted |
| E0262 | `IRON_ERR_V3_INLINE_DEFAULT` | Object field has a removed inline default. | `var x: Int = 0` inside an object. | emitted |
| E0263 | `IRON_ERR_V3_MUT_KEYWORD` | Removed `mut` binding keyword was used. | `mut x = 1` | emitted |
| E0264 | `IRON_ERR_V3_NO_INIT` | Object requiring initialization has no `init`. | Declare fields but no initializer. | emitted |
| E0265 | `IRON_ERR_VAL_FIELD_REASSIGN` | Immutable field is assigned after initialization. | Assign a `val` field in an ordinary method. | emitted |
| E0266 | `IRON_ERR_PARM_READ_ONLY` | Read-only parameter is mutated. | Assign an ordinary parameter in the callee. | emitted |
| E0267 | `IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT` | `var` parameter receives a read-only place. | Pass a `val` binding to a `var` parameter. | emitted |
| E0268 | `IRON_ERR_PTR_NO_ARITH` | Checked pointer arithmetic is forbidden. | Add one to `*Int`. | emitted |
| E0269 | `IRON_ERR_PTR_CAST_SIZE_MISMATCH` | Pointer cast changes pointee size. | Cast raw `Int` storage to a larger object. | emitted |
| E0270 | `IRON_ERR_PTR_AMP_ON_RVALUE` | Address is taken from a temporary. | `sink(&make())` | emitted |
| E0271 | `IRON_ERR_PTR_ESCAPE_STACK_REF` | Pointer to a stack local escapes its frame. | Return `&local`. | emitted |
| E0272 | `IRON_ERR_PTR_NULL_DEREF` | Null is assigned to a non-null pointer or dereferenced. | `val p: *Int = null` | emitted |
| E0273 | `IRON_ERR_HEAP_BAD_POSITION` | `heap` appears outside an allocation expression. | `val p: heap P = heap P()` | emitted |
| E0274 | `IRON_ERR_FREE_NOT_BINDING` | `free` target is not a binding name. | `free holder.item` | emitted |
| E0275 | `IRON_ERR_LEAK_NOT_BINDING` | `leak` target is not a binding name. | `leak holder.item` | emitted |
| E0276 | `IRON_ERR_DEFER_FORM_UNSUPPORTED` | Old restriction on general `defer`. | No current trigger; general defer is implemented. | retired |
| E0277 | `IRON_ERR_READONLY_PARAM_MUTATION` | Readonly method assigns a parameter. | Assign a parameter inside a readonly method. | emitted |
| E0278 | `IRON_ERR_READONLY_IO` | Readonly method performs I/O. | Call `println` inside a readonly method. | emitted |
| E0279 | `IRON_ERR_READONLY_HEAP_ESCAPE` | Readonly method allocates owning storage. | Use `heap P()` or `rc P()` in a readonly method. | emitted |
| E0280 | `IRON_ERR_READONLY_RETURN_TYPE` | Readonly method returns a non-readonly-compatible type. | Return an owning list from a readonly method. | emitted |
| E0281 | `IRON_ERR_READONLY_IFACE_CONFORMANCE` | Interface readonly method has a mutating implementation. | Implement readonly interface method as ordinary. | emitted |
| E0282 | `IRON_ERR_VEC_STRICT_LENGTH_MISMATCH` | Fixed array literal has the wrong length. | Assign two elements to `[Int; 3]`. | emitted |
| E0283 | `IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN` | Fixed and bounded arrays require explicit conversion. | Assign `[Int; <=3]` to `[Int; 3]`. | emitted |
| E0284 | `IRON_ERR_DROP_DUPLICATE` | Object declares more than one `drop`. | Two `drop` blocks in one object. | emitted |
| E0285 | `IRON_ERR_COPY_DUPLICATE` | Object declares more than one `copy`. | Two `copy` blocks in one object. | emitted |
| E0286 | `IRON_ERR_COPY_OF_NOCOPY_TYPE` | Operation would copy a `nocopy` value. | Store a closure that captures a nocopy handle. | emitted |
| E0287 | `IRON_ERR_DROP_NOT_READONLY` | `drop` carries the forbidden readonly modifier. | `readonly drop {}` | emitted |
| E0288 | `IRON_ERR_DROP_NO_EARLY_RETURN` | `drop` returns before cleanup finishes. | `return` inside `drop`. | emitted |
| E0289 | `IRON_ERR_PTR_REGIME_MISMATCH` | Checked and unchecked pointer regimes are mixed. | Assign `*T` to `*unchecked T`. | emitted |
| E0290 | `IRON_ERR_CANCELLED` | Analysis was cancelled by the language-server client. | Cancel an in-flight analysis request. | emitted |
| E0291 | `IRON_ERR_COMPTIME_FS_DISABLED_IN_LSP_MODE` | LSP analysis refuses compile-time filesystem access. | Call `read_file` during editor analysis. | emitted |
| E0292 | `IRON_ERR_TYPE_MISMATCH_LITERAL` | Literal cannot be adapted to the required type. | Assign `300` to `Int8`. | emitted |
| E0293 | `IRON_ERR_MISSING_RETURN` | Value-returning function has a path with no return. | End `func f() -> Int` without returning. | emitted |
| E0294 | `IRON_ERR_PTR_AMP_NOT_UNCHECKED` | Address-of cannot create an unchecked pointer. | Initialize `*unchecked Int` with `&x`. | emitted |
| E0295 | `IRON_ERR_PTR_ARITH_CHECKED` | Pointer arithmetic helper requires an unchecked pointer. | Pass `*Int` to `Ptr.offset`. | emitted |
| E0296 | `IRON_ERR_PTR_AMP_ON_RC` | Address is taken from an `rc` or `weak rc` handle. | `&shared` where `shared` is `rc P`. | emitted |
| E0297 | `IRON_ERR_RC_BAD_POSITION` | `rc` appears outside an allocation expression or weak type. | `val p: rc P = rc P()` | emitted |
| E0298 | `IRON_ERR_CLOSED_POLICY_KEYWORD` | Unsupported lifecycle keyword is used. | `pool P()` | emitted |
| E0299 | `IRON_ERR_WEAK_RC_DEREF` | Weak reference is used without upgrade. | Read `weak_handle.x`. | emitted |
| E0300 | `IRON_ERR_WEAK_RC_DOWNGRADE_NOT_RC` | `.downgrade()` receiver is not an `rc` value. | `P().downgrade()` | emitted |
| E0301 | `IRON_ERR_RC_IN_ARENA` | Reference-counted allocation appears in an arena block. | Use `rc P()` inside `in arena`. | emitted |
| E0309 | `IRON_ERR_DUPLICATE_MATCH_ARM` | Match repeats an already listed arm. | Two `Color.Red` arms. | emitted |
| E0310 | `IRON_ERR_INVALID_CAST` | Requested conversion is not supported. | Convert an object with `Int(p)`. | emitted |
| E0311 | `IRON_ERR_CAST_OVERFLOW` | Constant conversion is out of range. | `Int8(300)`. | emitted |
| E0312 | `IRON_ERR_INDEX_OUT_OF_BOUNDS` | Constant index is outside a fixed collection. | Read `[1, 2][3]`. | emitted |
| E0313 | `IRON_ERR_INVALID_SLICE_BOUNDS` | Constant slice bounds are invalid. | Slice `xs[3..1]`. | emitted |
| E0314 | `IRON_ERR_POSSIBLY_UNINITIALIZED` | `var` may be read before assignment. | `var n: Int` then print `n`. | emitted |
| E0320 | `IRON_ERR_CROSS_MODULE_PRIVATE` | Another file uses a non-public declaration. | Call a private function from a sibling file. | emitted |
| E0321 | `IRON_ERR_STANDALONE_METHOD_FORM` | Removed `func Type.method` form is used in program code. | `func Point.move() {}` | emitted |
| E0322 | `IRON_ERR_UNSUPPORTED_TYPE_TEST` | `is` test uses an unsupported type pair. | Test a primitive against an unrelated type. | emitted |
| E0323 | `IRON_ERR_MATCH_SUBJECT_UNSUPPORTED` | Match subject type is unsupported. | Match on `String`. | emitted |
| E0324 | `IRON_ERR_LAMBDA_PARAM_TYPE` | Lambda parameter type cannot be inferred here. | Store `func(x) {}` without an expected function type. | emitted |
| E0325 | `IRON_ERR_AWAIT_TWICE` | Spawn handle is awaited more than once. | `await task` twice. | emitted |
| E0326 | `IRON_ERR_POOL_UNSUPPORTED` | Thread-pool syntax is not implemented. | `spawn("x", pool) {}` | emitted |
| E0327 | `IRON_ERR_DROP_BINDING_UNINIT` | Drop-capable binding may be uninitialized at scope exit. | Conditionally initialize a value with `drop`. | emitted |
| E0328 | `IRON_ERR_LIST_IMPLICIT_COPY` | List operation would implicitly copy owned elements. | Assign a list containing nocopy elements. | emitted |
| E0329 | `IRON_ERR_UNORDERED_POSITION` | Unordered list is indexed by position. | Read `xs[0]` from `[I, unordered]`. | emitted |
| E0330 | `IRON_ERR_PTR_INTO_LIST` | Address is taken into movable list storage. | `&xs[i]` for a growable list. | emitted |
| E0331 | `IRON_ERR_ARRAY_EXT_BODY` | User list extension has a body where only intrinsic stubs are allowed. | `func [T].extra() {}` | emitted |
| E0332 | `IRON_ERR_NESTED_PATTERN` | Nested refutable enum pattern is unsupported. | `Outer.A(Inner.B(x))`. | emitted |
| E0333 | `IRON_ERR_CTOR_SPELLING` | Removed `.new` constructor spelling is used. | `Channel.new(4)`. | emitted |
| E0334 | `IRON_ERR_NOT_STRINGABLE` | Interpolated value has no text representation. | Interpolate an object without readonly `to_string`. | emitted |
| E0335 | `IRON_ERR_LOOP_JUMP_OUTSIDE` | `break` or `continue` has no legal enclosing loop. | Put `break` in a lambda nested in a loop. | emitted |
| E0336 | `IRON_ERR_RETURN_IN_DEFER` | `return` appears directly in deferred work. | `defer { return }` | emitted |
| E0345 | `IRON_ERR_GLOBAL_INIT_CYCLE` | Global initialization dependency graph has a cycle. | `val a = b` and `val b = a`. | emitted |
| E0350 | `IRON_ERR_STATIC_ON_VALUE` | Static function is called through a value. | `value.parse(text)` for a static `parse`. | emitted |
| E0351 | `IRON_ERR_METHOD_ON_TYPE` | Instance method is called through a type. | `Point.move()` when `move` uses `self`. | emitted |
| E0352 | `IRON_ERR_IFACE_STATIC_MISMATCH` | Interface and implementor disagree about `static`. | Static interface function implemented as a method. | emitted |
| E0353 | `IRON_ERR_IFACE_SIGNATURE` | Interface and implementation signatures differ. | Implement `f(Int)` as `f(String)`. | emitted |
| E0354 | `IRON_ERR_IFACE_VALUE_CALL` | Interface value cannot dispatch a static or Self-dependent function. | Call a static interface function through `i: I`. | emitted |
| E0355 | `IRON_ERR_CONSTRAINT_NO_STATIC` | Generic constraint declares no requested static function. | Call `T.parse()` without such a constrained member. | emitted |
| E0356 | `IRON_ERR_GENERATED_ONLY` | Compiler-only member name is used by source code. | Call or declare `__fields`. | emitted |
| E0400 | `IRON_ERR_LOWER_UNSUPPORTED` | Lowering received an unsupported analyzed construct. | Compiler bug while lowering a valid AST node. | emitted |
| E0401 | `IRON_ERR_LOWER_UNRESOLVED_IDENT` | Reserved lowering check for unresolved identifiers. | No current trigger. | reserved |
| E0402 | `IRON_ERR_LOWER_INVALID_ASSIGN` | Reserved lowering check for invalid assignment targets. | No current trigger. | reserved |
| E0403 | `IRON_ERR_LOWER_INVALID_MATCH` | Reserved lowering check for invalid match forms. | No current trigger. | reserved |
| E0501 | `IRON_ERR_WEB_AWAIT` | Web target reaches an `await`. | Build a program that awaits a task with `--target=web`. | emitted |
| E0502 | `IRON_ERR_WEB_TOP_LEVEL_LOADER` | Web target loads a raylib asset during global initialization. | Initialize a global with `LoadTexture` for the web target. | emitted |
| W0600 | `IRON_WARN_SPAWN_NO_HANDLE` | Old warning for detached spawn. | No current trigger; detached spawn is intentional. | retired |
| W0601 | `IRON_WARN_NARROWING_CAST` | Conversion narrows the source range. | Convert a run-time `Int` to `Int8`. | emitted |
| W0603 | `IRON_WARN_POSSIBLE_OVERFLOW` | Arithmetic may overflow the destination type. | Add run-time `Int8` values. | emitted |
| W0604 | `IRON_WARN_SPAWN_DATA_RACE` | Spawn may share mutable data without synchronization. | Capture a mutable outer binding in a spawn. | emitted |
| W0605 | `IRON_WARN_ARENA_NONTRIVIAL_DTOR` | Arena allocation skips a non-trivial destructor. | Arena-allocate an object owning a String. | emitted |
| W0606 | `IRON_WARN_FORGOTTEN_FREE` | Heap allocation is never freed or leaked. | Bind `heap P()` and let the binding leave scope. | emitted |
| W0607 | `IRON_WARN_UNREACHABLE_FREE` | `free` cannot be reached. | Put `free p` after an unconditional return. | emitted |
| W0610 | `IRON_WARN_DOC_COMMENT_TRUNCATED` | Documentation comment exceeds the stored line limit. | A `///` line longer than 8192 bytes. | emitted |
| W0611 | `IRON_WARN_UNUSED_IMPORT` | Import is never referenced. | `import math` without using `Math`. | emitted |
| W0612 | `IRON_WARN_REDUNDANT_CAST` | Conversion leaves the type unchanged. | Convert an `Int` to `Int`. | emitted |
| W0613 | `IRON_WARN_UNUSED_VAR` | Mutable local is never reassigned. | `var n = 1` followed only by reads. | emitted |
| W0614 | `IRON_WARN_UNUSED_VAR_PARAM` | `var` parameter is never mutated. | Declare `func f(var n: Int)` and only read `n`. | emitted |
| W0615 | `IRON_WARN_CONSTANT_TYPE_TEST` | Type test result is known statically. | Test an `Int` binding with `is Int`. | emitted |
| W0616 | `IRON_WARN_NULL_COMPARE_NEVER` | Null comparison on a non-nullable value is constant. | Compare `n: Int` with `null`. | emitted |
| E0700 | `IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS` | Web build has more than one main-loop registration. | Call the raylib frame-loop form twice. | emitted |
| E0701 | `IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP` | Web main loop does not use the required canonical form. | Put the frame loop in an unsupported shape. | emitted |
| E0702 | `IRON_ERR_WEB_NESTED_MAIN_LOOP` | Web main loop appears inside another control-flow construct. | Put it inside an `if`. | emitted |
| E0703 | `IRON_ERR_WEB_MAIN_LOOP_WRONG_FUNCTION` | Web main loop appears outside `main`. | Register it from a helper function. | emitted |
| E0820 | `IRON_ERR_READONLY_MEMORY` | Readonly-memory rule is violated. | Mutate memory reachable only through a readonly path. | emitted |
| E0900 | `IRON_ERR_LIR_MISSING_TERMINATOR` | LIR block has no terminating instruction. | Compiler bug: block ends without return or branch. | emitted |
| E0901 | `IRON_ERR_LIR_INVALID_BRANCH_TARGET` | LIR branch names an invalid block. | Compiler bug: branch target is out of range. | emitted |
| E0902 | `IRON_ERR_LIR_USE_BEFORE_DEF` | LIR value is used before definition. | Compiler bug: instruction reads a later temporary. | emitted |
| E0903 | `IRON_ERR_LIR_INSTR_AFTER_TERMINATOR` | LIR instruction follows a terminator. | Compiler bug: instruction follows return. | emitted |
| E0904 | `IRON_ERR_LIR_NO_ENTRY_BLOCK` | LIR function has no entry block. | Compiler bug: function contains zero blocks. | emitted |
| E0905 | `IRON_ERR_LIR_RETURN_TYPE_MISMATCH` | LIR return value has the wrong type. | Compiler bug: String returned from an Int function. | emitted |
| E0906 | `IRON_ERR_LIR_PHI_TYPE_MISMATCH` | LIR phi inputs have incompatible types. | Compiler bug: phi combines Int and String. | emitted |
| E0907 | `IRON_ERR_LIR_CALL_TYPE_MISMATCH` | LIR call arguments disagree with the callee. | Compiler bug: LIR call passes wrong argument type. | emitted |
| E0950 | `IRON_ERR_HIR_NULL_POINTER` | HIR contains a required null node or pointer. | Compiler bug: null expression in a statement. | emitted |
| E0951 | `IRON_ERR_HIR_USE_BEFORE_DEF` | HIR value is used before its definition. | Compiler bug: HIR identifier precedes its binding. | emitted |
| E0952 | `IRON_ERR_HIR_DUPLICATE_BINDING` | HIR scope contains duplicate bindings. | Compiler bug: lowering inserts one binding twice. | emitted |
| E0953 | `IRON_ERR_HIR_TYPE_MISMATCH` | Reserved HIR type consistency check. | No current trigger. | reserved |
| E0954 | `IRON_ERR_HIR_ARG_COUNT_MISMATCH` | Reserved HIR call-arity check. | No current trigger. | reserved |
| E0955 | `IRON_ERR_HIR_INVALID_SCOPE` | Reserved HIR scope consistency check. | No current trigger. | reserved |
| E0956 | `IRON_ERR_HIR_MISSING_RETURN_VALUE` | Reserved HIR return-value check. | No current trigger. | reserved |
| E0957 | `IRON_ERR_HIR_STRUCTURAL` | HIR has another structural invariant violation. | Compiler bug: malformed HIR node shape. | emitted |

## Allocating a code

Before adding a diagnostic, choose an unused number in the range map in
`src/diagnostics/diagnostics.h`, add its definition there, add exactly one row
above, and add a regression test that triggers it. Do not recycle retired or
reserved numbers.
