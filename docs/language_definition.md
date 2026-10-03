# Iron Reference Manual

This manual describes the Iron programming language as implemented by the
`iron` compiler that ships with this repository (version 4.5). It is a
reference, not a tutorial: each section states what the compiler accepts and
what the program does, with a small complete example. Every `iron` code block
in this document is compiled and executed by `scripts/test_doc_examples.sh`,
and blocks that show a compile error are checked to fail with the stated
diagnostic code, so the manual cannot drift from the compiler without the test
failing.

Iron is a general-purpose native programming language. It is statically typed
and compiles to C, then to a native binary. It has no garbage collector, no
exceptions, no operator overloading and no implicit numeric conversions.
Values live on the stack by default; explicit `heap`, `rc` and arena
allocation cover the other cases.

Contents:

1. [Lexical conventions](#1-lexical-conventions)
2. [Types](#2-types)
3. [Expressions](#3-expressions)
4. [Statements](#4-statements)
5. [Declarations](#5-declarations)
6. [Memory](#6-memory)
7. [Concurrency](#7-concurrency)
8. [Compile-time evaluation](#8-compile-time-evaluation)
9. [The standard library](#9-the-standard-library)
10. [Programs, projects and the command line](#10-programs-projects-and-the-command-line)
11. [Diagnostics](#11-diagnostics)
12. [Not yet implemented](#12-not-yet-implemented)
13. [Complete syntax of Iron](#13-complete-syntax-of-iron)

---

## 1. Lexical conventions

### 1.1 Source text

Iron source files use the `.iron` extension and are UTF-8 text. Spaces,
tabs and carriage returns separate tokens. Newlines are tokens for the
lexer, but the parser skips them wherever they appear, so a newline never
ends a statement by itself: statements are delimited by the grammar (section
1.6). Semicolons are not statement terminators; `;` appears only inside list
types and list literals (`[Int; 4]`).

### 1.2 Comments

A line comment starts with `--` and runs to the end of the line. There are
no block comments, and `//` and `#` are not comments.

A line starting with `///` is a documentation comment. A run of `///`
lines directly above a declaration, field, enum variant or interface method
(no blank line in between) is attached to it and shown by the language
server; a blank line breaks the association. The compiler otherwise ignores
doc comments.

```iron
/// Doubles its argument.
/// Attached to `twice` because no blank line separates them.
func twice(x: Int) -> Int {
    return x * 2   -- a line comment
}

func main() {
    println("{twice(21)}")
}
```

```output
42
```

### 1.3 Identifiers and keywords

An identifier is a letter or `_` followed by letters, digits and `_`
(ASCII only). A lone `_` is the wildcard, usable as a binding name in
`val`, `var`, tuple destructuring and `match` patterns; it is not an
expression. Identifiers are case sensitive. By convention types and enum
variants are `CapitalCase`; functions, variables and fields are
`snake_case`. Capitalization is also significant in one place: in
`X.Y(args)` and `X.Y`, when both `X` and `Y` start with an uppercase letter
the expression is an enum variant construction rather than a method call or
field access (section 3.6).

The following words are keywords and cannot be used as identifiers:

```text
and       await     comptime  copy      defer     drop      elif      else
enum      extends   extern    false     for       free      func      heap
if        impl      import    in        init      interface is        leak
match     mut       nocopy    not       null      object    or        parallel
patch     pool      private   pub       pure      rc        readonly  return
self      spawn     super     true      unchecked val       var       weak
while
```

`extends`, `super`, `mut`, `private` and `pool` are reserved but have no
valid use: the compiler rejects them with an explanation (`extends` and
`super` because Iron has no inheritance, `mut` and `private` because `var`
and default privacy replaced them, `pool` because thread pools are not
implemented). `init`, `copy`, `drop`, `null` and `free` may additionally
appear as method names after a `.` or after `func` inside an object body,
which is how `Box.null()` and `b.free()` are spelled.

The words `as`, `fusible`, `layout`, `soa`, `aos`, `unordered`,
`allow_drop_skip` and `Self` are ordinary identifiers with a special meaning
in specific positions (import aliases, `@fusible`, list attributes, `heap`
options and the receiver type).

### 1.4 Literals

**Integers** are decimal (`42`), hexadecimal (`0xFF`, `0Xff`) or binary
(`0b1010`). Digits may not be separated by `_`, and a letter directly after a
number is an error. An integer literal has type `Int` (64-bit signed) unless
it initializes a binding or field of another integer type, in which case it
takes that type. Hexadecimal and binary literals must fit in 64 bits.

**Floats** are a digit sequence, a `.` and a digit sequence: `3.25`,
`0.5`. There is no exponent form and no trailing-dot form (`1.` and `.5`
are not float literals). A float literal has type `Float` (64-bit) unless
the context asks for `Float32`.

**Booleans** are `true` and `false`. **`null`** is the value of every
nullable type (section 2.2).

**Strings** are written between double quotes on one line, or between
`"""` and `"""` when they span lines (the newlines are part of the value).
A string literal is at most 4095 bytes. The escape sequences are:

| Escape | Meaning |
|---|---|
| `\n` | newline |
| `\t` | tab |
| `\\` | backslash |
| `\"` | double quote |
| `\{` and `\}` | literal braces (not interpolation) |
| `\u{H}` | the Unicode scalar `H` (1 to 6 hex digits), encoded as UTF-8 |

Any other character after a backslash is kept together with the backslash.

A string literal that contains an unescaped `{` is an **interpolated
string**: each `{expression}` is evaluated and its text spliced in (section
3.3). Interpolated expressions may themselves contain string literals.

```iron
func main() {
    val h = 0xFF
    val b = 0b1010
    val f = 3.25
    val s = "tab\there \"quoted\" braces \{x\} unicode \u{48}\u{49}"
    val m = """first line
second line"""
    val name = "iron"
    println("{h} {b} {f}")
    println(s)
    println(m)
    println("nested {name.upper()} and {"lit".len()} and {1 + 2 * 3}")
}
```

```output
255 10 3.25
tab	here "quoted" braces {x} unicode HI
first line
second line
nested IRON and 3 and 7
```

### 1.5 Operators and punctuation

```text
+    -    *    /    %    ==   !=   <    >    <=   >=
and  or   not  &    |    ^    ~    <<   >>   is
=    +=   -=   *=   /=   &=   |=   ^=   <<=  >>=
.    ..   ,    :    ;    ->   ?    @    &(address-of)
(    )    [    ]    {    }
```

`!` on its own is not a token (`not` is the logical negation).
Section 3.1 gives the precedence of the operators.

### 1.6 Newlines and statement boundaries

Because the parser skips newlines everywhere, an expression continues onto
the next line whenever that line starts with something that can extend it: a
binary operator, `(`, `[`, `.`, or `is`. This is what allows long calls and
list literals to be split across lines, but it also means that a line that
begins with `-`, `(` or `[` continues the previous statement. A bare
`return` must be the last statement of its block, since an expression on the
following line would be taken as its value.

```iron
func f(x: Int) -> Int {
    return x
}

func main() {
    val a = 10
    val b = a
    - 3              -- continues the previous line: b is 7
    val c = f(
        1,
    )
    val d = 1 +
        2
    println("{b} {c} {d}")
}
```

```output
7 1 3
```

---

## 2. Types

Iron is statically typed. Every binding, field and parameter has a type that
is either written after a colon or inferred from its initializer. There are
no implicit conversions between numeric types, and none between numbers and
strings.

### 2.1 Primitive types

| Type | Description |
|---|---|
| `Int` | 64-bit signed integer (the default integer type) |
| `Int8`, `Int16`, `Int32`, `Int64` | signed integers of the given width |
| `UInt`, `UInt8`, `UInt16`, `UInt32`, `UInt64` | unsigned integers (`UInt` is 64-bit) |
| `Float` | 64-bit IEEE double (the default float type) |
| `Float32`, `Float64` | floats of the given width |
| `Bool` | `true` or `false` |
| `String` | an immutable sequence of Unicode characters, stored as UTF-8 |
| `Void` | the result type of a function without `-> T` (never written) |

Arithmetic on the same integer type wraps on overflow (two's complement).
Integer division truncates toward zero and division by zero panics at run
time. Mixing `Int` with `Float`, or two different integer widths, in one
expression is an error (`E0222`, `E0202`); convert explicitly (section 2.9).

Strings are values: copying a string copies a reference to shared,
reference-counted characters and never mutates them. `len(s)` and
`s.len()` count characters (code points); `s.byte_len()` counts bytes.
Iterating a string with `for` yields one-character strings.

### 2.2 Nullable types

`T?` is a type whose values are either a `T` or `null`. A nullable type can
be written for any non-pointer type (`Int?`, `String?`, `Node?`,
`Result[Int, String]?`). A plain `T` converts to `T?` implicitly, so a
function declared `-> Int?` may `return i` or `return null`. Reading a
field or calling a method through a nullable value without checking it
first is an error (`E0204`); compare with `null` first.

```iron
func find(xs: [Int], target: Int) -> Int? {
    var i = 0
    while i < len(xs) {
        if xs[i] == target {
            return i
        }
        i += 1
    }
    return null
}

func main() {
    val idx = find([4, 5, 6], 6)
    if idx != null {
        println("found at {idx}")
    }
    val missing = find([4, 5, 6], 9)
    if missing == null {
        println("missing")
    }
    var s: String? = null
    s = "now"
    println("{s}")
}
```

```output
found at 2
missing
now
```

Nullable pointers are written `?*T` (section 6.4), and a nullable weak
reference `weak rc T?` (section 6.3).

### 2.3 Lists, fixed arrays and bounded vectors

`[T]` is a growable list of `T`. `[T; N]` is a fixed-size array of exactly
`N` elements, where `N` is an integer constant. `[T; <=N]` is a bounded
vector: a list that can hold at most `N` elements and whose storage is
inline. All three are indexed with `xs[i]` from 0, checked at run time
(an out-of-range index panics; a constant out-of-range index on a fixed
array is a compile error, `E0312`), and iterated with `for`.

A list literal `[a, b, c]` has the type of its elements; an empty literal
`[]` needs a type annotation on the binding (`E0229`). `fill(n, v)` builds a
list of `n` copies of `v`.

A list has exactly one owner. Assigning a list binding to another binding,
storing it in a field or another list, or returning a parameter list is an
error (`E0328`): write `xs.copy()` for an independent copy or `xs.take()`
to move the contents out and leave `xs` empty. Passing a list to a
function lends it for the duration of the call. Growing or shrinking a
list and writing an element require a `var` binding (`E0235`).
`rc [T]` is a shared list (section 6.3). The list methods are listed in
section 9.3.

```iron
func main() {
    var xs: [Int] = []
    xs.push(3)
    xs.push(1)
    xs.insert(0, 9)
    xs[1] = 30
    println("{xs.len()} {xs[0]} {xs[1]} {xs.contains(1)}")
    val fixed: [Int; 3] = [7, 8, 9]
    var bounded: [Int; <=4] = [1, 2]
    bounded.push(3)
    val zeros = fill(4, 0)
    println("{fixed[1]} {len(fixed)} {bounded.len()} {zeros.len()}")
    val dup = xs.copy()
    val moved = xs.take()
    println("{dup.len()} {moved.len()} {xs.len()}")
    val grid: [[Int]] = [[1, 2], [3]]
    println("{grid[0][1]} {grid.len()}")
}
```

```output
3 9 30 true
8 3 3 4
3 3 0
2 2
```

A list type may carry attributes after the element type:
`[T, layout: soa]` stores an object list field by field (structure of
arrays) and `[T, layout: aos]` element by element (the default); either
is used the same way. `[I, unordered]`, for an interface type `I`, keeps
the elements grouped by concrete type instead of in insertion order and
cannot be indexed (`E0329`); iteration visits every element.

```iron
object Particle {
    var x: Float
    var y: Float
    init(x: Float, y: Float) {
        self.x = x
        self.y = y
    }
}

interface Drawable {
    readonly func draw() -> String
}

object Dot impl Drawable {
    val id: Int
    readonly func draw() -> String {
        return "dot{self.id}"
    }
}

object Square impl Drawable {
    val id: Int
    readonly func draw() -> String {
        return "square{self.id}"
    }
}

func main() {
    var ps: [Particle, layout: soa] = []
    ps.push(Particle(1.0, 2.0))
    ps.push(Particle(3.0, 4.0))
    var sum = 0.0
    for p in ps {
        sum += p.x
    }
    println("{sum} {ps.len()}")
    val shapes: [Drawable, unordered] = [Dot(1), Square(2), Dot(3)]
    for s in shapes {
        print("{s.draw()} ")
    }
    println("")
}
```

```output
4 2
dot1 dot3 square2 
```

### 2.4 Tuples

`(T1, T2, ...)` with at least two element types is a tuple type. A tuple
value is written `(a, b)`. Tuples are used to return several values; they
are taken apart with a destructuring `val` (section 4.2) and have no other
operations (there is no positional access).

```iron
func divmod(a: Int, b: Int) -> (Int, Int) {
    return (a / b, a % b)
}

func main() {
    val (q, r) = divmod(17, 5)
    val (_, only_r) = divmod(9, 4)
    println("{q} {r} {only_r}")
}
```

```output
3 2 1
```

### 2.5 Function types

`func(T1, T2) -> R` is the type of functions and lambdas taking `T1` and
`T2` and returning `R`; omit `-> R` for a function that returns nothing.
Function values are created from lambda expressions (section 3.7); they
can be stored in bindings, fields and lists and passed as arguments. The
name of a top-level function is not usable as a value today (section 11):
wrap it in a lambda.

### 2.6 Objects, enums and interfaces

Named types are declared with `object` (section 5.3), `enum` (5.6) and
`interface` (5.5). An object is a value type: assigning it or passing it by
value copies its fields. An interface type holds any object that implements
the interface and dispatches method calls to that object.

### 2.7 Pointer and lifecycle types

These types are part of the memory model and are described in section 6:

| Type | Meaning |
|---|---|
| `*T`, `*var T` | a checked pointer to a `T` (writable through `*var T`) |
| `?*T` | a nullable checked pointer |
| `*unchecked T`, `*var unchecked T` | an unchecked (raw) pointer for FFI and `Box` |
| `rc T` | a strong reference-counted handle to a shared `T` |
| `weak rc T`, `weak rc T?` | a weak handle that does not keep the `T` alive |
| `rc [T]` | a shared, reference-counted list |
| `Box[T]` | an owned heap cell that hands out unchecked pointers |

`heap` is not a type: a `heap T(...)` expression produces a value of type
`T` whose storage is on the heap and whose binding must be freed
(section 6.2). Writing `heap` in a type annotation is an error (`E0273`).

### 2.8 Generics

Functions, objects and enums may take type parameters in square brackets:
`func largest[T](xs: [T]) -> T`, `object Pair[A, B]`,
`enum Result[T, E]`. A parameter may carry an interface constraint,
`[T: Hashable]`, which every type argument must satisfy (`E0206`). Type
arguments are inferred from the arguments of a call or construction
(`Pair(1, "one")`) or written on the type (`Stack[Int]()`,
`val r: Result[Int, String]`). Generic code is instantiated per set of type
arguments at compile time.

```iron
func largest[T](xs: [T], less: func(T, T) -> Bool) -> T {
    var best = xs[0]
    for x in xs {
        if less(best, x) {
            best = x
        }
    }
    return best
}

object Pair[A, B] {
    val first: A
    val second: B
}

object Stack[T] {
    var items: [T]
    init() {
        self.items = []
    }
    func push(x: T) {
        self.items.push(x)
    }
    func pop() -> T {
        return self.items.pop()
    }
    readonly func size() -> Int {
        return len(self.items)
    }
}

func main() {
    val big = largest([3, 9, 4], func(a: Int, b: Int) -> Bool { return a < b })
    val p = Pair(1, "one")
    var st = Stack[Int]()
    st.push(1)
    st.push(2)
    println("{big} {p.first} {p.second} {st.pop()} {st.size()}")
}
```

```output
9 1 one 2 1
```

Two limits apply to generics today. A value of a generic enum must be bound
to a binding with a written type before it is returned or matched, and a
payload bound by a `match` on a generic enum must be copied into an
annotated local before it is interpolated (section 5.6). The result of a
generic method of the standard library containers (`Channel[T].recv`,
`MutexGuard[T].get`, ...) must likewise be bound with a written type
(section 7.2).

### 2.9 Type conversions

Calling a primitive type name like a function converts a value:
`Int(x)`, `Float(x)`, `Int32(x)`, `UInt8(x)` and so on. The source must be
a numeric type or `Bool` (`E0310`; `Bool` itself is not a conversion
target); float to integer truncates toward zero, and `Int(true)` is 1. A
conversion to a narrower integer type warns (`W0601`) and a constant that
does not fit is an error (`E0311`). Numbers are converted to strings with
`n.to_string()` or by interpolation; strings are parsed with `s.to_int()`
and `s.to_float()`. There is no `String(x)`.

```iron
func main() {
    val f: Float = 3.99
    val n = Int(f)
    val small: Int8 = 100
    val u: UInt8 = 250
    val total = Int(small) + Int(u)
    println("{n} {Int(-3.99)} {Int(true)} {Float(n) / 2.0} {total}")
    println("{"42".to_int() + 1} {"2.5".to_float() * 2.0} {n.to_string()}")
}
```

```output
3 -3 1 1.5 350
43 5 3
```

---

## 3. Expressions

### 3.1 Operators and precedence

Binary operators are left associative. From lowest to highest precedence:

| Level | Operators |
|---|---|
| 1 | `is` |
| 2 | `or` |
| 3 | `and` |
| 4 | `\|` |
| 5 | `^` |
| 6 | `&` |
| 7 | `==` `!=` |
| 8 | `<` `>` `<=` `>=` |
| 9 | `<<` `>>` |
| 10 | `+` `-` |
| 11 | `*` `/` `%` |
| 12 | unary `-` `not` `~` `&` `heap` `rc` `weak rc` `comptime` `await` |
| 13 | postfix `.name` `[i]` `[a..b]` `(args)` |

`is` binds loosest of all: `not s is Circle` parses as `(not s) is Circle`
and is a type error; write `not (s is Circle)`. Comparison binds tighter
than equality, so `1 <= 2 == true` is `(1 <= 2) == true`. `and` and `or`
short-circuit.

```iron
func main() {
    println("{2 + 3 * 4 - 8 / 2 % 3}")
    println("{1 << 4 | 1} {6 & 3 ^ 1} {~0} {-7 / 2} {-7 % 3}")
    println("{1 < 2 and 2 < 3 or false} {not true} {1 <= 2 == true}")
}
```

```output
13
17 3 -1 -3 -1
true false true
```

### 3.2 Arithmetic, comparison, logical and bitwise operators

`+ - * / %` apply to two operands of the same numeric type. `+` also
concatenates two strings. `== !=` compare numbers, booleans, strings (by
content), enum values and `null`. `< > <= >=` compare numbers. `and`,
`or`, `not` take `Bool` operands. `& | ^ ~ << >>` take integer operands
(`E0233`). Compound assignments `+= -= *= /= &= |= ^= <<= >>=` are
statements (section 4.3).

### 3.3 String interpolation and concatenation

Inside a string literal, `{e}` evaluates `e` and inserts its text. Any
value that has a text form may be interpolated: numbers, booleans,
strings, enums (the variant name), objects with a
`readonly func to_string() -> String` method (through that method, also
behind `rc`), the nullable forms of all of these (`null` when there is no
value), and the results of calls and field accesses. Interpolating a
value with no text form, such as an object without `to_string()`, is an
error (`E0334`). Floats print the shortest
decimal that reads back to the same value, without a trailing `.0`
(`3.0` prints as `3`). Two strings are joined with `+`; the compound form
`s += t` is not supported (see section 11).

```iron
func main() {
    val f = 3.0
    val name = "Iron"
    val s = "a" + "b" + "c"
    println("{name}: {f} {f / 4.0} {s} {s == "abc"} {true}")
}
```

```output
Iron: 3 0.75 abc true true
```

### 3.4 Calls, methods, fields and indexing

`f(a, b)` calls a function or a function value; arguments are positional
(`E0101` names named arguments as unsupported) and a trailing comma is
allowed. `x.field` reads a field, `x.method(args)` calls a method,
`Type.method(args)` calls a named init or a standard library function, and
`Type(args)` constructs an object (section 5.3). `xs[i]` indexes a list,
array or string (a string index yields a one-character string), and
`xs[a..b]` takes the characters `a` (inclusive) to `b` (exclusive) of a
string; `xs[a..]` runs to the end. Slicing a list compiles but fails to
generate C today (section 11); use `copy()` and the list methods instead.

A pointer, `rc` handle, `heap` value or `Box` cell is accessed with the
same `.` syntax as the value it refers to (auto-dereference); there is no
prefix `*` operator.

### 3.5 List and tuple literals

`[a, b, c]` is a list literal (a trailing comma is allowed and elements may
be spread over lines); `[T; n]` is a typed array literal used to declare
fixed arrays; `(a, b)` with two or more elements is a tuple literal.

### 3.6 Enum values

`Color.Red` names a unit variant and `Shape.Circle(1.0)` constructs a
variant with a payload. Enum values are compared with `==` and taken apart
with `match` (section 4.7).

### 3.7 Lambdas and closures

`func(params) [-> T] { body }` in expression position is a lambda. When
the lambda is passed directly to a parameter of function type, its
parameter and return types are taken from that parameter and may be
omitted; everywhere else the parameter types must be written (`E0324`) and
a lambda without `-> T` returns nothing, whatever its body does. A lambda
may refer to bindings of the enclosing function. A
`val` binding is captured by value. A `var` binding that a lambda
captures moves into a reference counted cell shared by the enclosing
function and every closure that captured it: mutations inside the lambda
are visible outside, and the closure may outlive the function that
declared the `var` (a counter closure keeps its count). A lambda may
capture a list only when it is passed directly as a call argument
(`E0328` otherwise); to share a list with a stored closure use `rc [T]`.
The environment of a closure is reference counted, so a closure may be
returned, stored in a field or list, and copied.

```iron
func make_adder(n: Int) -> func(Int) -> Int {
    return func(x: Int) -> Int { return x + n }
}

func apply_twice(f: func(Int) -> Int, x: Int) -> Int {
    return f(f(x))
}

object Button {
    val on_click: func(Int) -> String
}

func main() {
    val add5 = make_adder(5)
    println("{add5(1)} {apply_twice(add5, 0)}")
    println("{apply_twice(func(x) { return x * 3 }, 2)}")
    val handlers: [func(Int) -> Int] = [add5, add5]
    println("{handlers[0](4)}")
    val b = Button(func(n: Int) -> String { return "clicked {n}" })
    println(b.on_click(3))
    var local = 1
    val bump = func() { local += 10 }
    bump()
    println("{local}")
    val xs = [1, 2, 3]
    val ys = [2]
    val common = xs.filter(func(x: Int) -> Bool { return ys.contains(x) })
    println("{common.len()}")
}
```

```output
6 10
18
9
clicked 3
11
1
```

### 3.8 Allocation expressions

`heap T(args)`, `heap(in: arena) T(args)`, `rc T(args)`, `rc [a, b]`,
`weak rc null` and `x.downgrade()` create values with an explicit
lifecycle; they are described in section 6. `heap` and `rc` may only
appear in front of an allocation expression, not in a type annotation, a
parameter or a binding keyword position (`E0273`, `E0297`); the reserved
`pool` keyword is rejected with `E0298`.

### 3.9 Type tests

`x is T` is `true` when the interface value `x` currently holds an object
of type `T`. Inside the `if` block guarded by `x is T` (and after an early
return in the other branch), `x` is narrowed and the fields and methods of
`T` are available. The operand must be an interface or object value
(`E0322`).

```iron
interface Shape {
    readonly func area() -> Float
}

object Circle impl Shape {
    val r: Float
    readonly func area() -> Float {
        return 3.0 * self.r * self.r
    }
}

object Square impl Shape {
    val side: Float
    readonly func area() -> Float {
        return self.side * self.side
    }
}

func describe(s: Shape) -> String {
    if s is Circle {
        return "circle of radius {s.r}"
    }
    if not (s is Square) {
        return "unknown"
    }
    return "square with area {s.area()}"
}

func main() {
    println(describe(Circle(1.0)))
    println(describe(Square(2.0)))
}
```

```output
circle of radius 1
square with area 4
```

### 3.10 `comptime`, `spawn` and `await`

`comptime e` evaluates `e` at compile time (section 8). `spawn("name") {
body }` starts a thread and yields a handle; `await h` waits for it and
yields the value the body returned (section 7).

---

## 4. Statements

### 4.1 Blocks and scope

A block is `{ statement* }`. Blocks are the bodies of functions, methods,
control statements and lambdas, and a bare block may appear as a statement.
A binding is visible from its declaration to the end of the enclosing block.
Stack values declared in a block are destroyed when the block exits, in
reverse declaration order (section 6.1). A binding may shadow a binding of
an enclosing scope, except that a `match` pattern binding may not shadow an
existing name (`E0227`).

### 4.2 Bindings

`val name = expr` declares an immutable binding and `var name = expr` a
mutable one. Either may carry a type (`val speed: Float = 2.5`), which is
required when the initializer does not determine it (`val xs: [Int] = []`,
`val h = Channel[Int](4)`) and lets a literal take another
numeric type (`val small: Int8 = 100`). A `var` binding may be declared
without an initializer and assigned later; reading it before every path has
assigned it is an error (`E0314`).

A `val` cannot be reassigned (`E0203`), have a field written (`E0234`) or
have a mutating method called on it (`E0235`); lists follow the same rule
for their mutating methods and index writes. `_` as a binding name
evaluates the initializer and discards it.

`val (a, b) = expr` destructures a tuple; each position is a name or `_`,
at least two positions are required, an initializer is mandatory and a type
may be written after the pattern (`val (a, b): (Int, Int) = pair()`).

The initializer of a `val` or `var` may be a `spawn` expression (section
7.1); `spawn` cannot appear anywhere else inside an expression.

### 4.3 Assignment

`target = expr` stores into a `var` binding, a field of a `var` object or
of a `var` parameter, an element `xs[i]` of a `var` list, or a field
reached through a pointer, `rc` handle or `heap` binding. The compound
forms `+= -= *= /= &= |= ^= <<= >>=` compute `target op expr` and store
it; they are statements, not expressions, and are only defined for numeric
operands. Assignment to a `val` parameter is an error (`E0266`); declare
the parameter `var` to mutate it (section 5.2).

### 4.4 `if`

```text
if cond { ... } elif cond { ... } else { ... }
```

Conditions must be `Bool`; the braces are mandatory; `elif` and `else`
are optional. `if x is T` narrows `x` inside the block (section 3.9).

### 4.5 `while`

`while cond { ... }` repeats its block while the condition holds. There
are no `break` and `continue` statements: leave a loop by returning from the
function or by making the condition false.

### 4.6 `for`

`for name in iterable { ... }` binds `name` to each element in turn. The
iterable may be a list, fixed array, bounded vector or `rc [T]` (elements),
a string (one-character strings), a `Set[T]` (items) or `range(n)` (the
integers `0` to `n - 1`); a `Map[K, V]` is iterated with two names, `for
(key, value) in m` (section 9.10). The loop variable is immutable inside
the body. `range` takes exactly one argument. Appending `parallel` after the iterable runs the
iterations on several threads (section 7.4).

```iron
func main() {
    var total = 0
    for n in [1, 2, 3] {
        total += n
    }
    for i in range(3) {
        total += i
    }
    var k = 0
    while k < 3 {
        k += 1
    }
    for c in "héy" {
        print("[{c}]")
    }
    println("")
    if total > 100 {
        println("big")
    } elif total > 5 {
        println("medium: {total} {k}")
    } else {
        println("small")
    }
}
```

```output
[h][é][y]
medium: 9 3
```

### 4.7 `match`

```text
match subject {
    pattern -> statement
    pattern -> { statements }
    else -> statement
}
```

The subject must be an enum value, an integer, or an interface value
(`E0323`; `String`, `Bool` and `Float` subjects are rejected). Each arm is
a pattern, `->`, and either a single statement or a block. The optional
`else` arm must come last. `match` is a statement; arms usually `return` or
print.

Patterns:

- `Enum.Variant` matches a unit variant; `Enum.Variant(a, b)` matches a
  payload variant and binds its fields to new immutable names. `_` skips a
  field. The `Enum.` qualifier may be omitted when the variant name is
  unambiguous. A pattern may not bind a name that already exists in scope
  (`E0227`), and the number of bindings must equal the payload arity
  (`E0225`).
- An integer literal (or constant expression) matches that value of an
  integer subject; an `else` arm is then required (`E0224`).
- For an interface subject, `Type(name)` matches when the value holds a
  `Type` and binds it as `name`, giving access to the object's fields.

A match on an enum or interface must cover every variant or implementor,
or have an `else` arm (`E0224`); an arm that can never match is an error
(`E0226`). Nested payload patterns such as `Outer.Some(Inner.Circle(r))`
parse, but the inner pattern is not checked at run time (section 11), so
match the inner value in a second `match`.

```iron
enum Shape {
    Circle(Float),
    Rect(Float, Float),
    Empty,
}

interface Animal {
    readonly func sound() -> String
}

object Dog impl Animal {
    val name: String
    readonly func sound() -> String {
        return "woof"
    }
}

object Cat impl Animal {
    val lives: Int
    readonly func sound() -> String {
        return "meow"
    }
}

func area(s: Shape) -> Float {
    match s {
        Shape.Circle(r) -> return 3.0 * r * r
        Shape.Rect(w, h) -> return w * h
        Shape.Empty -> return 0.0
    }
    return 0.0
}

func size(n: Int) -> String {
    match n {
        0 -> return "zero"
        1 -> return "one"
        else -> return "many"
    }
    return "unreachable"
}

func main() {
    println("{area(Shape.Rect(2.0, 3.0))} {area(Shape.Empty)} {area(Shape.Circle(1.0))}")
    println("{size(1)} {size(7)}")
    val pets: [Animal] = [Dog("Rex"), Cat(9)]
    for a in pets {
        match a {
            Dog(d) -> println("{d.name} says {d.sound()}")
            Cat(c) -> {
                val lives = c.lives
                println("cat with {lives} lives says {c.sound()}")
            }
        }
    }
}
```

```output
6 0 3
one many
Rex says woof
cat with 9 lives says meow
```

Generic enums work the same way, with the two extra steps noted in section
2.8:

```iron
enum Result[T, E] {
    Ok(T),
    Err(E),
}

func parse_positive(n: Int) -> Result[Int, String] {
    if n > 0 {
        val ok: Result[Int, String] = Result.Ok(n)
        return ok
    }
    val err: Result[Int, String] = Result.Err("not positive")
    return err
}

func main() {
    val r: Result[Int, String] = parse_positive(-1)
    match r {
        Result.Ok(v) -> {
            val n: Int = v
            println("ok {n}")
        }
        Result.Err(e) -> {
            val msg: String = e
            println("error: {msg}")
        }
    }
}
```

```output
error: not positive
```

### 4.8 `return`

`return expr` leaves the function with a value; `return` alone leaves a
function without a result type. Every path of a function with a result type
must return (`E0293`), and the value must have the declared type (`E0215`).
A `return` inside an `init` may not carry a value (`E0252`) and may not
leave fields unassigned (`E0250`).

### 4.9 `defer`

`defer statement` and `defer { block }` schedule the statement to run when
the enclosing block exits, whether by falling off its end or by `return`.
Deferred statements run in reverse order of registration, after the
block's own statements and before its stack values are destroyed. Inside a
loop body, deferred statements run at the end of each iteration. The most
common form is `defer free x` for a heap value (section 6.2).

```iron
func main() {
    {
        defer {
            println("cleanup 2")
        }
        defer println("cleanup 1")
        println("body")
    }
    var i = 0
    while i < 2 {
        defer println("end of iteration {i}")
        i += 1
    }
    println("done")
}
```

```output
body
cleanup 1
cleanup 2
end of iteration 1
end of iteration 2
done
```

### 4.10 `free` and `leak`

`free x` releases the heap value bound to `x` and `leak x` marks it as
intentionally never freed; both are described in section 6.2.

### 4.11 `in arena { ... }`

`in a { ... }` makes `a` (an `Arena`) the default allocator for every
`heap` expression inside the block (section 6.6).

### 4.12 Expression statements

A call, method call or `spawn` expression may stand as a statement. Any
other expression on its own is accepted by the parser but has no effect.

---

## 5. Declarations

A program is a sequence of top-level declarations: imports, functions,
objects, patches, interfaces, enums and global bindings. Declarations may
appear in any order and are visible throughout the file (and, when `pub`,
throughout the package, section 10.3).

### 5.1 The entry point

A binary program has exactly one `func main()` with no parameters and no
result type. Execution starts there and the process exits with status 0
when `main` returns. A run-time failure (index out of range, division by
zero, failed `assert`, stale pointer) prints a message to standard error and
aborts.

```iron
func main() {
    val samples = [10, 20, 30, 40, 50]
    var total: Int = 0
    for sample in samples {
        total += sample
    }
    println("processed={len(samples)} total={total}")
}
```

```output
processed=5 total=150
```

### 5.2 Functions

```text
func name[T, U](p1: T1, var p2: T2) -> R { body }
```

Parameters are `val` by default: a parameter cannot be reassigned or
mutated (`E0266`, `E0234`). A parameter declared `var` is a copy that the
body may assign to and mutate; when the function returns, the final value
is written back to the caller's `var` binding, so `var` parameters behave
like in-out arguments. A list parameter is lent (never copied); a `var`
list parameter lets the body grow or shrink the caller's list. Objects are
passed by value. The result type follows `->`; omit it for a function that
returns nothing. Argument count and types must match exactly (`E0216`,
`E0217`). Functions cannot be overloaded (`E0201`) and there are no default
or named arguments. Type parameters are written after the name (section
2.8).

```iron
object Point {
    var x: Int
    init(x: Int) {
        self.x = x
    }
    func bump() {
        self.x += 1
    }
}

func by_value(p: Point) -> Int {
    return p.x + 100
}

func by_var(var p: Point) {
    p.x = 99
    p.bump()
}

func set_seven(var n: Int) {
    n = 7
}

func grow(var xs: [Int]) {
    xs.push(4)
}

func main() {
    var a = Point(1)
    println("{by_value(a)} {a.x}")
    by_var(a)
    var n = 1
    set_seven(n)
    var xs = [1, 2, 3]
    grow(xs)
    println("{a.x} {n} {xs.len()}")
}
```

```output
101 1
100 7 4
```

A top-level function name cannot be used as a value (`val f = twice`,
`apply(twice, 4)`); wrap it in a lambda (`func(x: Int) -> Int { return
twice(x) }`). See section 11.

`@fusible` before a `func` marks it as eligible for loop fusion of chained
list operations; it changes nothing else about the function.

### 5.3 Objects

```text
object Name[T] impl Interface1, Interface2 {
    val immutable_field: Type
    var mutable_field: Type
    pub var exported_field: Type

    init(params) { self.field = ... }
    init named(params) { ... }

    func mutating(...) -> R { ... }
    readonly func observer(...) -> R { ... }
    pure func computation(...) -> R { ... }

    copy { ... }
    drop { ... }
}
```

An object is a value type made of named fields. Every field is declared
with `val` or `var` and a type (`E0176`); a field cannot have an inline
default value (`E0262`). An object is constructed by calling its name with
the arguments of its anonymous `init`: `Name(args)`. When an object declares
no `init`, it must have only `val` fields (`E0264`) and is constructed
positionally, one argument per field in declaration order.

**Initializers.** `init(params) { ... }` is the anonymous initializer and
`init name(params) { ... }` a named one, called as `Name.name(args)`. The
body assigns every field through `self` exactly once for `val` fields
(`E0248`) and at least once for `var` fields on every path (`E0247`); it
may not read a field before assigning it (`E0246`), call a method on the
partially built `self` (`E0249`), return early (`E0250`), delegate to
another `init` (`E0251`) or return a value (`E0252`). An object has at
most one anonymous `init` (`E0201`). `Name.init(args)` is an explicit
spelling of `Name(args)`.

**Methods** are declared inside the object body with `func`, take an
implicit receiver `self` of the object type, and are called as
`value.method(args)`. A method has one of three tiers:

| Tier | May write `self` | May call | I/O |
|---|---|---|---|
| `func` (default, mutating) | yes | anything | yes |
| `readonly func` | no (`E0238`) | `readonly` and `pure` methods (`E0239`) | no (`E0278`) |
| `pure func` | no (`E0244`) | `pure` methods only (`E0242`) | no (`E0240`) |

`readonly` and `pure` methods may be called on `val` bindings; a mutating
method needs a `var` binding (`E0235`). A `pure` method may also not write
its parameters (`E0243`) or read a global `var` (`E0241`). The modifiers
are only valid inside object, patch and interface bodies (`E0245`), and
`init` takes none.

**`self` and `Self`.** Inside a method `self` is the receiver;
outside a method it is an error (`E0210`). The identifier `Self` names the
enclosing object type and may be used as a result type.

**Visibility.** Fields and methods are private to the file that declares
the object by default. `pub` on a field synthesizes a getter named after the
field and, for `pub var`, a setter `set_<field>`; call sites keep using
`obj.field` and `obj.field = v`, and a user method with one of those names
is an error (`E0237`). `pub` on a method or `init` exports it; `pub init`
is only allowed inside a `pub object`. See section 5.10 for cross-file
rules.

**`copy` and `drop` blocks** run when a value is copied or destroyed
(section 6.7); `nocopy object` forbids copying (section 6.7).

```iron
import math

object Vec2 {
    val x: Float
    val y: Float

    init(x: Float, y: Float) {
        self.x = x
        self.y = y
    }

    init zero() {
        self.x = 0.0
        self.y = 0.0
    }

    readonly func length() -> Float {
        return Math.sqrt(self.x * self.x + self.y * self.y)
    }

    readonly func scaled(k: Float) -> Self {
        return Vec2(self.x * k, self.y * k)
    }
}

object Account {
    pub var balance: Int
    pub val id: Int
    var history: Int

    init(id: Int) {
        self.id = id
        self.balance = 0
        self.history = 0
    }

    func deposit(amount: Int) {
        self.balance += amount
        self.history += 1
    }

    pure func fee(amount: Int) -> Int {
        return amount / 100
    }
}

object Point {
    val x: Int
    val y: Int
}

func main() {
    val v = Vec2(3.0, 4.0)
    val origin = Vec2.zero()
    val p = Point(1, 2)
    println("{v.length()} {v.scaled(2.0).x} {origin.y} {p.x},{p.y}")
    var acc = Account(7)
    acc.deposit(50)
    acc.balance = 60
    acc.set_balance(acc.balance + 5)
    println("{acc.id} {acc.balance} {acc.fee(250)}")
    var copy_of_acc = acc
    copy_of_acc.deposit(1)
    println("{acc.balance} {copy_of_acc.balance}")
}
```

```output
5 6 0 1,2
7 65 2
65 66
```

### 5.4 `patch object`

`patch object Name { ... }` adds methods and initializers to an existing
object declared in the same package or in the standard library. A patch may
declare `impl Interface` to make the type conform; it may not add fields
(`E0253`), patch an unknown type (`E0254`), redefine an existing method
(`E0255`) or take type parameters. Methods added by a patch have the same
tiers and visibility rules as methods declared in the object body.

```iron
object Celsius {
    val degrees: Float
}

interface Describable {
    readonly func describe() -> String
}

patch object Celsius impl Describable {
    readonly func describe() -> String {
        return "{self.degrees} C"
    }

    init freezing() {
        self.degrees = 0.0
    }
}

patch object String {
    readonly func shout() -> String {
        return self.upper() + "!"
    }
}

func main() {
    val c = Celsius.freezing()
    println("{c.describe()} {"hey".shout()}")
}
```

```output
0 C HEY!
```

### 5.5 Interfaces

```text
interface Name {
    readonly func required() -> R
    pure func also_required(x: T) -> R
    func with_default() -> R { body }
}
```

An interface lists method signatures, each with a tier modifier. An object
conforms by naming the interface after `impl` and defining every listed
method without a default body (`E0205`) with a tier at least as strict as
the interface's (`E0257`; a `pure` method satisfies a `readonly`
signature). A signature with a body is a default: an implementor that does
not define the method inherits it, and may override it. Interfaces may not
declare `init` (`E0256`), and the keyword is `impl`, not `implements`. An
interface may have any number of implementors; a value of interface type
can be built from any of them and dispatches calls at run time. Interfaces
may be the element type of lists and the type of fields and parameters,
and are the subjects of `is` (section 3.9) and type `match` (section 4.7).
A `var` binding of interface type mutates the object it holds in place.

```iron
interface Shape {
    readonly func area() -> Float
    readonly func name() -> String {
        return "shape"
    }
}

object Square impl Shape {
    val side: Float
    readonly func area() -> Float {
        return self.side * self.side
    }
}

object Circle impl Shape {
    val r: Float
    readonly func area() -> Float {
        return 3.0 * self.r * self.r
    }
    readonly func name() -> String {
        return "circle"
    }
}

func total(shapes: [Shape]) -> Float {
    var t = 0.0
    for s in shapes {
        t += s.area()
    }
    return t
}

func main() {
    val shapes: [Shape] = [Square(2.0), Circle(1.0)]
    println("{total(shapes)}")
    for s in shapes {
        println("{s.name()} {s.area()}")
    }
}
```

```output
7
shape 4
circle 3
```

`Hashable` is the one interface the standard library defines
(`pure func hash() -> Int` and `pure func equals(other: Hashable) -> Bool`);
the integer types, `Bool` and `String` satisfy it without declaring `impl`.

### 5.6 Enums

```text
enum Name[T] {
    Unit,
    WithValue = 5,
    Payload(T, Int),
}
```

An enum lists variants separated by commas (a trailing comma is allowed).
A variant may carry a payload of one or more types, or an explicit integer
value (`= 5`). Unit variants are written `Name.Variant`, payload variants
are constructed with `Name.Variant(args)`. Enum values are compared with
`==` and inspected with `match`. Enums have no methods and cannot be
converted to integers; put behavior in functions that take the enum. Enums
may be generic, and a payload may be the enum type itself (recursive
enums).

```iron
enum Color {
    Red,
    Green = 5,
    Blue,
}

enum Token {
    Number(Int),
    Word(String),
    End,
}

func show(t: Token) -> String {
    match t {
        Token.Number(n) -> return "number {n}"
        Token.Word(w) -> return "word {w}"
        Token.End -> return "end"
    }
    return ""
}

func main() {
    val c = Color.Green
    if c == Color.Green and c != Color.Blue {
        println("green")
    }
    println("{show(Token.Number(4))} {show(Token.Word("hi"))} {show(Token.End)}")
}
```

```output
green
number 4 word hi end
```

### 5.7 Global bindings

`val` and `var` declarations may appear at the top level. A global `val`
is a constant initialized before `main` runs (its initializer may call
functions); a global `var` is a mutable variable shared by the whole
program (and by all threads, without synchronization). Globals cannot be
`pub`. A `pure` method may not read a global `var` (`E0241`).

```iron
val LIMIT = 10
val ANSWER = compute()
var counter = 0

func compute() -> Int {
    return 6 * 7
}

func bump() {
    counter += 1
}

func main() {
    bump()
    bump()
    println("{LIMIT} {ANSWER} {counter}")
}
```

```output
10 42 2
```

### 5.8 `extern func`

`extern func name(params) -> R` declares a C function without a body. The
call compiles to a direct C call; the C name is the Iron name with each
`snake_case` segment capitalized and the underscores removed
(`init_window` becomes `InitWindow`), and a name without underscores is
used unchanged. Only functions whose declarations the generated C already
includes (the C standard library, and raylib when it is imported) can be
called; there is no way to name a header. `Int` maps to `int64_t`,
`Float` to `double`, `Bool` to `bool`, `String` to the runtime string
struct and `*unchecked T` to a raw pointer. Declaring an extern with the
name of an Iron built-in is a duplicate declaration (`E0201`).

```iron
extern func labs(x: Int) -> Int
extern func srand(seed: Int)
extern func rand() -> Int

func main() {
    srand(1)
    val r = rand()
    println("{labs(-9)} {r >= 0}")
}
```

```output
9 true
```

### 5.9 Imports

`import name` makes a standard library module available (section 9), or
documents a dependency on a source file of the package: `import util`
matches `src/util.iron` or any file under a directory named `util`, and a
dotted path `import a.b` matches consecutive directory components. Importing
an unknown module is an error (`E0209`); an unused import warns (`W0611`).
`import x as y` is rejected: Iron has no module namespaces, so imported
declarations are used by their own names.

The modules that require an import are `math`, `io`, `time`, `log`,
`hint`, `net`, `http`, `websocket`, `url` and `raylib`; their functions are
called on the capitalized module object (`import math`, then
`Math.sqrt(x)`). Everything else in section 9 (strings, lists, `Box`,
`Arena`, `Channel`, `Mutex`, `RWLock`, `FileHandle`, `Hashable`, `RawPtr`)
is available without an import.

### 5.10 Visibility

Every top-level declaration is private to its file unless marked `pub`.
`pub` may prefix a function, object, patch, interface or enum (not a
global binding). Using a private declaration from another file of the
package is an error (`E0320`). Object members are private to the declaring
file unless marked `pub` (section 5.3). The word `private` is not accepted
(`E0101`).

---

## 6. Memory

Iron has no garbage collector. Every value has one of five lifecycles,
chosen at the point where the value is created: stack (the default),
`heap`, `rc`, `weak rc`, and arena. Pointers (`*T`) and `Box[T]` refer to
values without owning them. The compiler and runtime check the common
mistakes: freeing a non-heap value, forgetting to free, escaping a heap
value, dereferencing a stale checked pointer.

### 6.1 Stack values

A binding initialized with a plain expression holds its value directly.
Objects are copied on assignment, when passed by value and when stored in
lists or fields; each copy is destroyed independently. A value is destroyed
when its block exits (in reverse declaration order) or, for a temporary,
as soon as the expression that used it is done: a field read off a
temporary (`make().name`) copies the field out and then drops the
temporary. Destruction runs the object's `drop` block, if any (section
6.7), then the drops of its fields in reverse declaration order.

```iron
object Res {
    val id: Int
    drop {
        println("drop {self.id}")
    }
}

func main() {
    val a = Res(1)
    val b = Res(2)
    {
        val c = Res(3)
        println("inner")
    }
    println("end of main")
}
```

```output
inner
drop 3
end of main
drop 2
drop 1
```

### 6.2 `heap`, `free` and `leak`

`heap T(args)` allocates the object on the heap and yields a binding that
is used exactly like a stack value (fields through `.`, mutation only when
the binding is `var`). The program must release it with `free x`, usually
scheduled with `defer free x`, or declare that it is never released with
`leak x`. Heap values are not freed automatically: a heap binding that is
neither freed nor leaked warns (`W0606`) and the memory and the `drop`
block are lost. A heap value cannot leave the function that allocated it:
returning it or storing it where it outlives the scope is an error
(`E0207`); to share ownership use `rc`. `free` and `leak` apply only to
heap bindings (`E0212`, `E0213`, `E0274`, `E0275`), and an `rc` handle
cannot be leaked (`E0214`). Freeing runs `drop`. Using a value after it was
freed, including a second `free`, is caught at run time by a generation
check and aborts the program with a "stale pointer dereference" message.

```iron
object World {
    var seed: Int
    init(seed: Int) {
        self.seed = seed
    }
    drop {
        println("drop world {self.seed}")
    }
}

func main() {
    var world = heap World(42)
    defer free world
    world.seed = 43
    println("seed {world.seed}")
    val forever = heap World(7)
    leak forever
    println("leaked {forever.seed}")
}
```

```output
seed 43
leaked 7
drop world 43
```

<!-- doctest-expect-error: E0207 -->
```iron
object Node {
    val value: Int
}

func make() -> Node {
    val n = heap Node(1)
    return n
}

func main() {
    val m = make()
}
```

Inside `in arena { ... }` (section 6.6) `heap` allocates from the arena
instead and needs no `free`.

### 6.3 `rc` and `weak rc`

`rc T(args)` allocates a shared, reference-counted object and yields a
handle of type `rc T`. Copying the handle (assignment, passing, storing in
a field or list, capturing in a closure) increments the count; destroying a
copy decrements it, and when the last handle goes away the object's `drop`
runs and the memory is released. All handles see the same object, and a
field may be written through any handle, including a `val` one. Handles
are not nullable (`?rc T` is rejected, `E0297`) and cannot be `leak`ed
(`E0214`). The type `rc T` may be used for fields, parameters and results.

`weak rc T` is a handle that does not keep the object alive. It is obtained
with `handle.downgrade()` on an `rc` value or written as the constant
`weak rc null`, and turned back into a usable handle with `w.upgrade()`,
which yields a nullable strong handle, written `rc T?`, that is `null` when
the object has already been destroyed. A weak handle cannot be
dereferenced directly (`E0299`). Weak handles break reference cycles, for
example a child that points back to its parent.

`rc [a, b, c]` is a shared list: every copy of the handle reaches the same
list, it can be grown and indexed through any copy, and it is freed with the
last copy. It is the way to share a list with a closure or between fields.

```iron
object Player {
    val name: String
    drop {
        println("drop {self.name}")
    }
}

object Owner {
    val name: String
    var pet: weak rc Pet
    init(name: String) {
        self.name = name
        self.pet = weak rc null
    }
}

object Pet {
    val owner: rc Owner
    val nick: String
}

func make_counter() -> func() -> Int {
    val seen: rc [Int] = rc [1, 2, 3]
    return func() -> Int { return len(seen) }
}

func main() {
    val strong = rc Player("Alice")
    val weak_ref = strong.downgrade()
    {
        val another = strong
        val maybe = weak_ref.upgrade()
        if maybe != null {
            println("alive: {maybe.name}")
        }
    }
    val owner = rc Owner("Ann")
    val pet = rc Pet(owner, "Rex")
    owner.pet = pet.downgrade()
    val link: weak rc Pet = owner.pet
    val back = link.upgrade()
    if back != null {
        println("{back.owner.name} owns {back.nick}")
    }
    val shared: rc [Int] = rc [1, 2]
    val alias = shared
    alias.push(3)
    println("{shared.len()} {shared[2]} {make_counter()()}")
}
```

```output
alive: Alice
Ann owns Rex
3 3 3
drop Alice
```

### 6.4 Checked pointers

`&x` takes the address of a binding, field or fixed array element and
yields a checked pointer: `*T` when `x` is a `val` and `*var T` when `x`
is a `var`. `?*T` is a nullable pointer that may hold `null`. Fields are
read and, through `*var T`, written with `p.field`; a pointer to a
primitive prints its pointee when interpolated into a string, but there is
no prefix `*` operator, so it cannot be assigned through. Passing a binding
to a parameter of type `*T` takes its address automatically. A checked
pointer carries the generation of the allocation it points to, and every
dereference verifies it: reading through a pointer after the target was
freed aborts with a "stale pointer dereference" message instead of reading
garbage, even when the allocator has reused the address. Checked pointers
support no arithmetic (`E0268`, `E0295`); `&` cannot be applied to a
temporary (`E0270`), a pointer to a local cannot be returned (`E0271`), `&`
on an `rc` handle is rejected (`E0296`), and `&list[i]` on a growable list
is rejected (`E0330`) unless it is written directly as a call argument,
because the list may move its elements when it grows.

```iron
object Player {
    var hp: Int
    init(hp: Int) {
        self.hp = hp
    }
}

func observe(p: *Player) -> Int {
    return p.hp
}

func heal(p: *var Player) {
    p.hp += 10
}

func maybe(p: ?*Player) -> Bool {
    return p != null
}

func main() {
    var player = Player(80)
    val view: *Player = &player
    heal(&player)
    println("{observe(player)} {view.hp} {maybe(&player)}")
}
```

```output
90 90 true
```

<!-- doctest-expect-error: E0271 -->
```iron
object Res {
    val id: Int
}

func escape() -> *Res {
    val local = Res(1)
    return &local
}

func main() {
    val p = escape()
}
```

### 6.5 Unchecked pointers, `Box` and `RawPtr`

`*unchecked T` and `*var unchecked T` are plain C pointers with no
generation check; they exist for the FFI boundary and for `Box`. Iron code
obtains them from `Box[T]`: `Box(value)` moves a value into an owned
heap cell (`Box` is `nocopy`), `b.unwrap()` returns a
`*var unchecked T` to the contents, `b.is_null()` tests the cell,
`b.free()` releases it, and `Box.null()` is an empty cell. Fields are
accessed through the pointer with `.` as usual. Checked and unchecked
pointers are distinct types (`E0289`, `E0294`) and `&` never produces an
unchecked pointer. `RawPtr.of(x)` produces a type-erased `RawPtr` and
`Ptr.cast[T](raw)` turns it back into a `*unchecked T`.

```iron
object Config {
    var width: Int
    var title: String
    init(width: Int, title: String) {
        self.width = width
        self.title = title
    }
}

func main() {
    val boxed = Box(Config(640, "Iron App"))
    val cfg: *var unchecked Config = boxed.unwrap()
    cfg.width = 800
    println("{cfg.width} {cfg.title} {boxed.is_null()}")
    boxed.free()
    val empty: Box[Config] = Box.null()
    println("{boxed.is_null()} {empty.is_null()}")
    var n: Int = 7
    val raw: RawPtr = RawPtr.of(n)
    val p: *unchecked Int = Ptr.cast[Int](raw)
    println("{p}")
}
```

```output
800 Iron App false
true true
7
```

### 6.6 Arenas

`Arena(bytes)` creates a bump
allocator. `heap(in: a) T(args)` allocates from arena `a`, and inside
`in a { ... }` every plain `heap T(args)` does. Arena values are never
freed individually: `a.reset()` releases everything at once, and
`a.save()` / `a.restore(point)` roll the arena back to an earlier mark.
Because the arena frees in bulk, the `drop` block of an object allocated
in it does not run; the compiler warns about it (`W0605`) unless the
allocation says `heap(in: a, allow_drop_skip: true) T(args)`. Accessing an
arena value after `reset` or `restore` is a stale pointer error at run
time. `rc` allocation inside an `in arena` block is an error (`E0301`).
`a.used()` and `a.capacity()` report the arena's byte counts. An arena
value releases its memory when its binding ends; a `heap Arena(bytes)` is
freed with `free`, like any heap value.

```iron
object Particle {
    val x: Int
    val y: Int
}

func main() {
    val frame = Arena(65536)
    in frame {
        val p1 = heap Particle(10, 20)
        val p2 = heap Particle(30, 40)
        println("p1 ({p1.x},{p1.y}) p2 ({p2.x},{p2.y})")
    }
    val mark = frame.save()
    val p3 = heap(in: frame) Particle(1, 2)
    println("{p3.x} {frame.used() > 0}")
    frame.restore(mark)
    frame.reset()
    println("{frame.used()}")
}
```

```output
p1 (10,20) p2 (30,40)
1 true
0
```

### 6.7 `drop`, `copy` and `nocopy`

An object body may contain a `drop { ... }` block, run once when each
instance is destroyed (stack scope exit, `free`, last `rc` handle released,
list cleared), with `self` bound to the dying value. It may contain a
`copy { ... }` block, run after each implicit or explicit copy of the value
(`x.copy()` copies an object explicitly, retaining its `rc` fields and
cloning its list fields). An object declared `nocopy object` cannot be
copied at all: assigning it to another binding, passing it by value or
storing it is an error (`E0286`), which is how `Box`, `Channel`, `Mutex`,
`RWLock` and `FileHandle` guarantee a single owner. Each object has at most
one `drop` and one `copy` block (`E0284`, `E0285`); `drop` may not be
`readonly` (`E0287`) and may not `return` early (`E0288`).

```iron
object Point {
    val x: Int
    copy {
        println("copied {self.x}")
    }
    drop {
        println("dropped {self.x}")
    }
}

nocopy object Token {
    val id: Int
}

func main() {
    val p = Point(3)
    val q = p
    val t = Token(5)
    println("{q.x} {t.id}")
}
```

```output
copied 3
3 5
dropped 3
dropped 3
```

<!-- doctest-expect-error: E0286 -->
```iron
nocopy object Token {
    val id: Int
}

func main() {
    val t = Token(1)
    val u = t
}
```

### 6.8 List ownership

Lists are the one kind of value that is never copied implicitly. The
rules, all enforced at compile time with `E0328`:

- `val b = a` where `a` is a list, storing a list binding in a field or in
  another list, and returning a list parameter are rejected; write
  `a.copy()` or `a.take()`.
- A function receives a list argument by reference for the duration of the
  call; it may read it, and mutate it only through a `var` parameter.
- A lambda may capture a list only when the lambda is passed directly as a
  call argument; a lambda that is bound, returned or stored cannot capture a
  list. A `spawn` body that captures a list must be awaited in the block
  that started it.
- To share a list, use `rc [T]`.

<!-- doctest-expect-error: E0328 -->
```iron
func main() {
    val a = [1, 2, 3]
    val b = a
}
```

---

## 7. Concurrency

### 7.1 `spawn` and `await`

`spawn("name") { body }` runs the body on a new operating system thread and
evaluates to a handle. The body is a block that must `return` a value, and
`await handle` blocks until the thread finishes and yields that value. A
handle can be awaited once (`E0325`). A spawn expression may only appear as
the initializer of a `val` or `var`, or as a statement (its result is then
discarded and the thread is never awaited). The body may read `val` bindings of the enclosing
function and use `Mutex`, `Channel` and `rc` values; a list captured by a
spawn must be awaited in the same block (`E0328`). Writing a captured
`var` from a thread is a data race and warns (`W0604`). Named thread pools
are not implemented: `spawn("name", pool)` is rejected (`E0326`).

```iron
func sum_to(n: Int) -> Int {
    var total = 0
    for i in range(n) {
        total += i
    }
    return total
}

func main() {
    val a = spawn("a") {
        return sum_to(10)
    }
    val b = spawn("b") {
        return sum_to(100)
    }
    val ra = await a
    val rb = await b
    println("{ra} {rb}")
}
```

```output
45 4950
```

### 7.2 `Channel[T]`

`Channel[T](capacity)` creates a bounded queue of `T`; the element type
is written as the type argument (`val ch = Channel[Int](4)`), since no
argument carries it.
`ch.send(v)` blocks while the channel is full and `ch.recv()` blocks while
it is empty; the received value must be bound with a written type
(`val x: Int = ch.recv()`). Channels are `nocopy` and are closed when their
owner goes out of scope; there is no explicit close and no non-blocking
receive.

### 7.3 `Mutex[T]` and `RWLock[T]`

`Mutex(value)` wraps a value in a lock; `m.lock()` returns a
`MutexGuard[T]` that holds the lock until the guard's scope ends, with
`g.get()` and `g.set(v)` to read and write the protected value (the result
of `get` must be bound with a written type). `RWLock(value)` is the
reader-writer variant: `l.read()` returns an `RWReadGuard[T]` with `get()`,
and `l.write()` an `RWWriteGuard[T]` with `get()` and `set(v)`. All of
these are `nocopy`.

```iron
func main() {
    val ch = Channel[Int](4)
    val producer = spawn("producer") {
        for i in range(3) {
            ch.send(i * 10)
        }
        return 0
    }
    var got = 0
    for i in range(3) {
        val x: Int = ch.recv()
        got += x
    }
    await producer
    println("{got}")

    val counter = Mutex(0)
    val worker = spawn("worker") {
        for i in range(100) {
            val g = counter.lock()
            val cur: Int = g.get()
            g.set(cur + 1)
        }
        return 0
    }
    for i in range(100) {
        val g = counter.lock()
        val cur: Int = g.get()
        g.set(cur + 1)
    }
    await worker
    val final_guard = counter.lock()
    val total: Int = final_guard.get()
    println("{total}")

    val settings = RWLock(5)
    val reader = settings.read()
    val seen: Int = reader.get()
    println("{seen}")
}
```

```output
30
200
5
```

### 7.4 Parallel `for`

`for x in xs parallel { ... }` runs the iterations of the loop on several
threads and waits for all of them. The body must not write bindings of the
enclosing scope (`E0208`); use a `Mutex` to accumulate. `range(n)` and lists
may be iterated in parallel. The `parallel(pool)` form parses but pools are
not implemented (`E0326`).

```iron
func main() {
    val total = Mutex(0)
    for i in range(8) parallel {
        val g = total.lock()
        val cur: Int = g.get()
        g.set(cur + i)
    }
    val g = total.lock()
    val sum: Int = g.get()
    println("{sum}")
}
```

```output
28
```

---

## 8. Compile-time evaluation

`comptime expr` evaluates `expr` while compiling and replaces it with the
resulting constant. The evaluator handles integer, float and boolean
literals and arithmetic (`+ - * / %`, comparisons, `==` and `!=`, unary
`-` and `not`), string literals, list literals and indexing, object
construction, the built-ins `len`, `range`, `fill` and `read_file(path)`
(which reads a file relative to the source file and yields its contents),
and calls to top-level functions whose bodies use only `val`, `var`,
assignment, `if`, `while`, `for` and `return` over those values.
Method calls, field access, string concatenation, interpolation,
bitwise operators, lambdas, `match`, and `heap` or `rc` allocation are not
available at compile time (`E0231`, `E0232`). Evaluation is limited to one
million steps (`E0230`). The result is typically bound to a global `val`.

```iron
val TABLE_SIZE = comptime (64 * 4)
val FACTORIAL_5 = comptime fact(5)
val SQUARES = comptime squares(4)

func fact(n: Int) -> Int {
    var acc = 1
    for i in range(n) {
        acc = acc * (i + 1)
    }
    return acc
}

func squares(n: Int) -> [Int] {
    var out = fill(n, 0)
    var i = 0
    while i < n {
        out[i] = i * i
        i += 1
    }
    return out
}

func main() {
    println("{TABLE_SIZE} {FACTORIAL_5} {SQUARES[3]} {len(SQUARES)}")
}
```

```output
256 120 9 4
```

---

## 9. The standard library

The standard library is a set of Iron declarations (in `src/stdlib/`)
whose bodies are provided by the C runtime. Modules marked *import* must be
imported by name (section 5.9); the rest is always available. Functions on a
module object are called as `Module.function(args)`; methods are called on
values.

### 9.1 Built-in functions

These are available everywhere without an import.

| Function | Description |
|---|---|
| `println(s: String)` | writes `s` and a newline to standard output |
| `print(s: String)` | writes `s` without a newline |
| `len(x) -> Int` | number of elements of a list, array or vector, or characters of a string |
| `range(n: Int)` | the sequence `0 .. n-1`, only valid as the iterable of `for` |
| `fill(n: Int, v: T) -> [T]` | a list of `n` copies of `v` |
| `min(a: Int, b: Int) -> Int`, `max(a: Int, b: Int) -> Int` | smaller or larger of two integers |
| `clamp(x: Int, lo: Int, hi: Int) -> Int` | `x` limited to `[lo, hi]` |
| `abs(x: Int) -> Int` | absolute value |
| `assert(cond: Bool)`, `assert(cond: Bool, msg: String)` | abort with `msg` (or the source location) when `cond` is false |
| `read_file(path: String) -> String` | file contents, only inside `comptime` |

`print` and `println` take exactly one `String`; interpolate other values.
`min`, `max`, `clamp` and `abs` are `Int` only (use `Math` for floats).

```iron
func main() {
    println("{min(3, 9)} {max(3, 9)} {clamp(15, 0, 10)} {abs(-4)}")
    print("no newline, ")
    println("then one")
    assert(len("abc") == 3, "len counts characters")
}
```

```output
3 9 10 4
no newline, then one
```

### 9.2 `String` methods

All string methods are `readonly`; indexes count characters from 0 and a
missing substring gives `-1`.

| Method | Description |
|---|---|
| `len() -> Int`, `byte_len() -> Int` | characters, bytes |
| `upper() -> String`, `lower() -> String`, `trim() -> String` | case and whitespace |
| `contains(sub) -> Bool`, `starts_with(p) -> Bool`, `ends_with(s) -> Bool` | tests |
| `index_of(sub) -> Int`, `rindex_of(sub) -> Int`, `count(sub) -> Int` | search |
| `split(sep: String) -> [String]`, `chars() -> [String]` | split into parts or characters |
| `join(parts: [String]) -> String` | join `parts` with the receiver as separator |
| `replace(old, new) -> String`, `repeat(n) -> String` | rewriting |
| `substring(start, end) -> String`, `char_at(i) -> String` | slices (end exclusive) |
| `pad_left(width, ch) -> String`, `pad_right(width, ch) -> String` | padding |
| `to_int() -> Int`, `to_float() -> Float` | parsing (0 when not a number) |
| `byte_at(i) -> Int`, `String.from_byte(b: Int) -> String` | byte access |
| `release()` | does nothing (kept for source compatibility) |

`Int`, `Int32` and `Float` have a `to_string() -> String` method. `s[i]`
is `s.char_at(i)` and `s[a..b]` is `s.substring(a, b)`.

```iron
func main() {
    val s = "Hello, Wörld"
    println("{s.len()} {s.byte_len()} {s.upper()} {s.index_of("o")} {s.rindex_of("o")}")
    println("{s.substring(7, 12)} {s.char_at(8)} {s.count("l")} {s.replace("l", "L")}")
    val parts = "a,b,,c".split(",")
    println("{parts.len()} {parts[2].len()} {",".join(["x", "y"])}")
    println("[{"  x ".trim()}] {"ab".repeat(3)} {"7".pad_left(3, "0")} {"7".pad_right(3, "-")}")
    println("{s.byte_at(0)} {String.from_byte(65)} {"abc".chars()[1]} {s[0]} {s[0..5]}")
}
```

```output
12 13 HELLO, WÖRLD 4 4
Wörld ö 3 HeLLo, WörLd
4 0 x,y
[x] ababab 007 7--
72 A b H Hello
```

### 9.3 List methods

Methods on `[T]` (and, where noted, on fixed arrays, bounded vectors and
`rc [T]`). Mutating methods need a `var` list.

| Method | Description |
|---|---|
| `len() -> Int` | number of elements (also `len(xs)`) |
| `push(v: T)`, `pop() -> T` | append, remove and return the last element |
| `insert(i: Int, v: T)`, `remove(i: Int) -> T` | insert before / remove at index |
| `get(i: Int) -> T`, `set(i: Int, v: T)` | checked element access (same as `xs[i]`) |
| `get_unchecked(i: Int) -> T`, `set_unchecked(i: Int, v: T)` | access without the bounds check |
| `clear()`, `reverse()`, `sort()` | in place; `sort` orders `Int`, `Float` and `String` ascending |
| `contains(v: T) -> Bool` | membership |
| `copy() -> [T]`, `take() -> [T]` | independent copy; move the contents out |
| `map(f: func(T) -> U) -> [U]` | transform |
| `filter(f: func(T) -> Bool) -> [T]` | keep matching elements |
| `reduce(init: U, f: func(U, T) -> U) -> U` | fold |
| `forEach(f: func(T))` | call `f` on each element |
| `sum() -> T` | sum of an `[Int]` or `[Float]` |

The lambdas passed to `map`, `filter`, `reduce` and `forEach` must write
their parameter types. Chains of `map`/`filter`/`reduce` calls are fused
into one loop by the optimizer.

```iron
func main() {
    val ys = [1, 2, 3, 4, 5]
    val squares = ys.map(func(x: Int) -> Int { return x * x })
    val evens = ys.filter(func(x: Int) -> Bool { return x % 2 == 0 })
    val total = ys.reduce(0, func(acc: Int, x: Int) -> Int { return acc + x })
    val labels = ys.map(func(x: Int) -> String { return "n{x}" })
    println("{squares[4]} {evens.len()} {total} {ys.sum()} {labels[0]}")
    ys.forEach(func(x: Int) { print("{x},") })
    println("")
    var xs = [5, 6, 7, 8]
    xs.set(1, 60)
    val removed = xs.remove(0)
    xs.reverse()
    val popped = xs.pop()
    println("{removed} {popped} {xs.get(0)} {xs.len()}")
    var words = ["b", "c", "a"]
    words.sort()
    println("{words[0]}{words[1]}{words[2]}")
}
```

```output
25 2 15 15 n1
1,2,3,4,5,
5 60 8 2
abc
```

### 9.4 `math` (import)

`Math` has the constants `Math.PI`, `Math.TAU` and `Math.E` and the
functions `sin`, `cos`, `tan`, `asin`, `acos`, `sqrt`, `floor`, `ceil`,
`round`, `log`, `log2`, `exp` (all `(x: Float) -> Float`), `atan2(y, x)`,
`pow(base, exp)`, `hypot(a, b)`, `lerp(a, b, t)` (`Float` arguments and
results), `sign(x: Float) -> Int`, `random() -> Float` in `[0, 1)`,
`random_float(min, max) -> Float`, `random_int(min: Int, max: Int) -> Int`
(inclusive) and `seed(n: Int)`.

```iron
import math

func main() {
    println("{Math.PI > 3.14} {Math.floor(2.7)} {Math.pow(2.0, 10.0)} {Math.sign(-2.5)}")
    Math.seed(42)
    val r = Math.random()
    println("{r >= 0.0 and r < 1.0} {Math.random_int(3, 3)} {Math.hypot(3.0, 4.0)}")
}
```

```output
true 2 1024 -1
true 3 5
```

### 9.5 `io` (import)

Simple functions on `IO`:

| Function | Description |
|---|---|
| `read_file(path) -> String`, `write_file(path, content)`, `append_file(path, content)` | whole-file text I/O (an unreadable file reads as `""`) |
| `read_lines(path) -> [String]` | the lines of a file |
| `read_line() -> String` | one line from standard input |
| `file_exists(path) -> Bool`, `is_dir(path) -> Bool` | tests |
| `create_dir(path)`, `delete_file(path)` | directories and deletion |
| `list_files(dir) -> String` | the entries of a directory, one per line |
| `basename(path)`, `dirname(path)`, `extension(path)`, `join_path(a, b)` | path helpers (`extension` has no leading dot) |

Result-returning functions, which report errors in the value instead of
failing silently:

| Function | Result |
|---|---|
| `read_text(path, max_bytes)`, `read_bytes(path, max_bytes)` | `FileReadResult { data: String, error: Int, error_message: String }` |
| `write_text(path, content)`, `write_bytes(path, content)`, `append_text(path, content)`, `append_bytes(path, content)` | `FileWriteResult { bytes: Int, error: Int, error_message: String }` |
| `copy_file(src, dst, overwrite: Bool)`, `move_file(src, dst, overwrite: Bool)` | `FileWriteResult` |
| `file_info(path)` | `FileInfo { exists, is_file, is_dir: Bool, size, modified_unix, error: Int, error_message: String }` |

`error` is 0 on success. `FileHandle.open(path) -> FileHandle` and
`h.close()` give a `nocopy` handle to an open file descriptor (`h.fd`)
that closes itself when dropped.

```iron
import io

func main() {
    val path = "/tmp/iron_manual_io.txt"
    val w = IO.write_text(path, "one\ntwo\n")
    val r = IO.read_text(path, 1024)
    val lines = IO.read_lines(path)
    val info = IO.file_info(path)
    println("{w.error} {w.bytes} {r.error} {r.data.len()} {lines[1]} {info.size}")
    val missing = IO.read_text("/tmp/iron_manual_missing_file", 16)
    println("{missing.error != 0} {IO.extension("a/b.iron")} {IO.basename("a/b.iron")} {IO.join_path("a", "b")}")
    IO.delete_file(path)
    println("{IO.file_exists(path)}")
}
```

```output
0 8 0 8 two 8
true iron b.iron a/b
false
```

### 9.6 `time` (import)

`Time.now() -> Float` (seconds since the Unix epoch), `Time.now_ms() -> Int`
and `Time.now_ns() -> Int` (milliseconds and nanoseconds), `Time.sleep(ms:
Int)`, `Time.since(start: Float) -> Float` (seconds elapsed since a
`Time.now()` value), and `Time.Timer(seconds: Float) -> Timer` with the
fields `elapsed_ms` and `duration_ms` and the methods `done() -> Bool`,
`update(dt: Float)` and `reset()` (the last two need a `var` timer).
`Duration` is a millisecond value type built with `Duration.millis(n)`,
`Duration.seconds(n)`, `Duration.minutes(n)` or `Duration.from_ms(n)`,
read with `d.ms` or `d.to_ms()`.

```iron
import time

func main() {
    val started = Time.now_ms()
    Time.sleep(5)
    println("{Time.now_ms() - started >= 5} {Duration.seconds(2).to_ms()} {Time.Timer(0.5).done()}")
}
```

```output
true 2000 false
```

### 9.7 `log` (import)

`Log.debug(msg)`, `Log.info(msg)`, `Log.warn(msg)` and `Log.error(msg)`
write a timestamped line to standard error; `Log.set_level(level)` with
`Log.DEBUG`, `Log.INFO`, `Log.WARN` or `Log.ERROR` hides messages below
the level.

### 9.8 `hint` (import)

`Hint.black_box(x: Int) -> Int` returns its argument while preventing the
C optimizer from reasoning about it; it exists for benchmarks.

### 9.9 Memory and concurrency types

These are always available and are described in sections 6 and 7. Each
one is constructed like any object, `Type(args)` or `Type[T](args)` when
no argument carries the element type; there is no `.new` method
(`E0333`):

| Type | API |
|---|---|
| `Box[T]` (nocopy) | `Box(v)` or `Box[T](v) -> Box[T]`, `Box.null() -> Box[T]`, `b.unwrap() -> *var unchecked T`, `b.is_null() -> Bool`, `b.free()` |
| `Arena` | `Arena(bytes)`, `Arena.threadsafe(bytes)`, `a.save() -> ArenaSave`, `a.restore(p: ArenaSave)`, `a.reset()`, `a.used() -> Int`, `a.capacity() -> Int` |
| `RawPtr` | `RawPtr.of(x) -> RawPtr`, `Ptr.cast[T](raw) -> *unchecked T` |
| `Channel[T]` (nocopy) | `Channel[T](capacity: Int)`, `ch.send(v: T)`, `ch.recv() -> T` |
| `Mutex[T]`, `MutexGuard[T]` (nocopy) | `Mutex(v)` or `Mutex[T](v)`, `m.lock() -> MutexGuard[T]`, `g.get() -> T`, `g.set(v: T)` |
| `RWLock[T]`, `RWReadGuard[T]`, `RWWriteGuard[T]` (nocopy) | `RWLock(v)` or `RWLock[T](v)`, `l.read()`, `l.write()`, `g.get() -> T`, `g.set(v: T)` (write guard only) |
| `FileHandle` (nocopy) | `FileHandle.open(path) -> FileHandle`, `h.close()`, field `fd: Int` |
| `Map[K, V]`, `Set[T]` | section 9.10 |

### 9.10 `Hashable`, `Map` and `Set`

`Map[K, V]` is a hash table from keys of type `K` to values of type `V`;
`Set[T]` is a hash set of `T`. Both are constructed empty with their type
arguments written out (`Map[String, Int]()`, `Set[Int]()`; nothing else
carries them) and both are library objects: there is no literal and no
`m[k]` indexing. The key type must satisfy `Hashable` (`E0206`): the
integer types, `Bool` and `String` do, and an object does when it declares
`impl Hashable` with `pure func hash() -> Int` and `pure func
equals(other: Hashable) -> Bool` (section 5.5); two keys are the same
entry when their hashes agree and `equals` is true. The value type may be
anything that can be stored in a list, including lists, `rc` handles and
other maps.

| Method | Meaning |
|---|---|
| `m.put(k, v)` | insert or overwrite; the old value is dropped |
| `m.get(k) -> V` | the value; panics when the key is absent |
| `m.get_or(k, d) -> V` | the value, or `d` when the key is absent |
| `m.has(k) -> Bool` | whether the key is present |
| `m.remove(k) -> Bool` | drop the entry; whether it was present |
| `m.len() -> Int`, `m.clear()` | entry count; drop every entry |
| `m.keys() -> [K]`, `m.values() -> [V]` | fresh lists of copies, in no particular order |
| `m.copy()`, `m.take()` | an independent copy; move the contents out, leaving `m` empty |
| `s.add(x) -> Bool` | insert; whether `x` was new |
| `s.has(x)`, `s.remove(x) -> Bool`, `s.len()`, `s.clear()` | as for a map |
| `s.values() -> [T]`, `s.copy()`, `s.take()` | as for a map |

A map or set owns its keys and values exactly as a list owns its elements
(section 6.8): `put` and `add` take ownership of their arguments (a value
copied out of a binding is retained or cloned first), `get` and `get_or`
hand out a copy that the caller owns, `remove`, `clear` and scope exit
drop what they remove, and a map binding is never duplicated implicitly
(`E0328`): write `copy()` or `take()`, or share it as `rc Map[K, V]`.
`put`, `remove`, `clear`, `add` and `take` need a `var` binding (`E0235`).
`for (k, v) in m` visits every entry with read-only copies of the key and
the value; `for x in s` visits every item. Iteration order is unspecified,
and the loop body must not add or remove entries of the map it iterates.
`get` on a missing key is a panic ("key not found in map"), so test with
`has` or use `get_or` when the key may be absent.

```iron
object Pt impl Hashable {
    val x: Int
    val y: Int
    pure func hash() -> Int {
        return self.x * 31 + self.y
    }
    pure func equals(other: Hashable) -> Bool {
        if other is Pt {
            return self.x == other.x and self.y == other.y
        }
        return false
    }
}

func main() {
    var counts = Map[String, Int]()
    for w in ["a", "b", "a"] {
        counts.put(w, counts.get_or(w, 0) + 1)
    }
    var keys = counts.keys()
    keys.sort()
    println("{counts.len()} {keys[0]} {counts.get("a")} {counts.has("z")}")
    var total = 0
    for (k, v) in counts {
        total += v
    }
    println("{total} {counts.remove("b")} {counts.remove("b")}")

    var seen = Set[Pt]()
    println("{seen.add(Pt(1, 2))} {seen.add(Pt(1, 2))} {seen.has(Pt(1, 2))} {seen.len()}")

    var groups = Map[String, [Int]]()
    groups.put("even", [2, 4])
    var even = groups.get("even")
    even.push(6)
    groups.put("even", even.take())
    println("{groups.get("even").len()}")
}
```

```output
2 a 2 false
3 true false
true false true 1
3
```

### 9.11 `net`, `http`, `websocket` and `url` (import)

The networking modules are documented in [docs/networking.md](networking.md).
In summary: `net` provides `Net.tcp_dial(host, port, timeout)`,
`Net.tcp_listen(host, port)`, `TcpListener.accept`, `TcpSocket.read`,
`TcpSocket.write`, `close`, UDP (`Net.udp_bind`, `Net.udp_sendto_v4`,
`Net.udp_sendto_v6`, `UdpSocket.recvfrom`), `IPv4Addr` / `IPv6Addr`
parsing and formatting and `Net.lookup_host`; every fallible call returns a
tuple whose second element is a `NetError { code: Int }` (0 on success).
`http` provides a server (`Http.listen`, `HttpServer.accept`,
`HttpConnection.read_request`, `send_response`, `Http.listen_tls`), a
client (`Http.get`, `Http.post_json`, `Http.request`, `HttpClient.open`)
and response builders (`Http.response`, `json_response`, `html_response`,
`text_response`, `file_response`, `Http.header`); requests and responses
are objects with `status`, `headers`, `body`, `error` and `error_message`
fields. `websocket` provides `WebSocket.connect` (and the `_with_ca`,
`_insecure` and `_with_protocols` variants), `send_text`, `send_bytes`,
`ping`, `receive`, `close`, `abort`, `is_open` and the server side
`HttpConnection.upgrade_websocket`. `url` provides `Url.parse(s) ->
(Url, UrlError)`, `Url.build(u)`, `Url.resolve(base, ref)`,
`Url.percent_encode`, `Url.percent_decode` and a `Url.builder()`.
All timeouts are integer milliseconds.

### 9.12 `raylib` (import)

`import raylib` makes the raylib bindings available: the objects
`Vector2`, `Vector3`, `Color`, `Rectangle`, `Texture`, `Font` and so on,
the enums `KeyboardKey`, `MouseButton`, `ConfigFlags` and others, and the
functions as `snake_case` methods on namespace objects (`Window.init`,
`Window.should_close`, `Draw.begin`, `Draw.text`, `Input.is_key_down`, ...).
The full binding list is `src/stdlib/raylib.iron`, and the graphics guide at
[ironlang.dev/raylib](https://ironlang.dev/raylib/) shows complete programs.

---

## 10. Programs, projects and the command line

### 10.1 Single files

`ironc build file.iron` compiles one file to a binary next to it (`-o`
picks the path), `ironc run file.iron` compiles and runs it, and
`ironc check file.iron` type-checks it. `iron build file.iron`, `iron run
file.iron` and `iron check file.iron` do the same. A single file may
`import` only standard library modules.

### 10.2 Packages

`iron init name` (or `iron init --lib name`) creates a package: a
directory with an `iron.toml` manifest, `src/main.iron` (or `src/lib.iron`
for a library) and a `.gitignore`. Inside a package, `iron build` compiles
every `.iron` file under `src/` and `vendor/` into `target/`, `iron run`
builds and runs it, `iron check` type-checks the same sources, `iron test`
compiles and runs every `tests/test_*.iron` file as a program (a test
passes when it exits with 0) and `iron fmt file.iron` reformats a file
(`--check` only reports). All files of a package share one namespace: a
`pub` declaration in one file is visible in every other file, and a
private one only in its own (`E0320`). `import` of a package file is
optional and documents the dependency.

```toml
[package]
name = "demo"
version = "0.1.0"
type = "bin"            # or "lib"
description = "optional"
iron = ">= 4.4.0"       # optional compiler version constraint
```

The `iron` constraint uses full `X.Y.Z` versions with the operators `>=`,
`>`, `<=`, `<`, `=` (or no operator for an exact version), `^` (same
major, or same minor before 1.0) and `~` (same minor), and comma-separated
clauses are combined with AND (`">= 4.0.0, < 5.0.0"`). A pre-release such
as `4.4.0-alpha` sorts before `4.4.0`. A mismatch stops the build with the
version to install. There is no `[dependencies]` table: a manifest that
declares one fails with a vendoring hint.

### 10.3 Third-party code

Iron has no package manager, registry or lockfile. To use third-party
Iron code, copy its source into `vendor/<name>/` and commit it. `iron
build`, `iron run` and `iron check` compile every `.iron` file under
`vendor/` together with `src/`: a vendored directory that has its own
`iron.toml` and `src/` contributes only its `src/`, other directories
contribute every `.iron` file recursively, and `tests/`, `examples/`,
`target/` and hidden directories are skipped. Vendored code shares the
package namespace, so its `pub` declarations are used directly; two
vendored libraries that declare the same name are a duplicate declaration
(`E0201`).

### 10.4 Build flags

`iron build` and `iron run` accept `-o path` / `--output path`,
`--release` (optimized C compilation), `--no-optimize` (skip Iron's own
IR optimizations), `--debug-build` (keep the generated C under
`.iron-build/`), `--emit-c` (`build` only: write the C file and stop
before the C compiler), `--dump-ir-passes`, `--report-compression`,
`--warn-fusion-break`, `--force-comptime` (ignore the comptime cache) and
`--target=web`. `--no-strict-v3` accepts a few removed syntax forms for
debugging old code. `--verbose` prints the generated C and the link line
and `--version` prints the compiler version and the toolchain it compiles
with.

### 10.5 The web target

`iron build --target=web` compiles a package to WebAssembly with the
Emscripten toolchain pinned in `.emsdk-version`, producing
`dist/web/index.html` together with its `.js` loader and `.wasm` module.
It runs on every host Iron supports; `emcc` (or `emcc.bat` on Windows)
must be on `PATH`, as `emsdk_env` leaves it. The `[web]` table of the
manifest configures it:

```toml
[web]
title = "My App"                 # page title
shell = "custom_shell.html"      # optional HTML template
initial_memory = 67108864        # bytes
stack_size = 5242880             # bytes
pthread_pool_size = 4
assets = ["assets/sprites.png"]  # files preloaded into the virtual FS
```

A web program cannot `await` (`E0501`) and must drive its frame loop from
`main` with a single canonical `while` loop (`E0700` to `E0703`).

### 10.6 The backend

Iron compiles to C and hands the C to a compiler it ships. The generated
C is C17 with the GNU extensions clang accepts under `-std=gnu17`
(statement expressions and `__builtin_*` intrinsics, `__attribute__`
annotations, `_Static_assert`), compiled with `-fwrapv` (integer overflow
wraps) and `-fno-strict-aliasing`. It includes only the runtime header,
which itself depends only on the freestanding C headers (`stdint.h`,
`stddef.h`, `stdbool.h`, `stdarg.h`, `stdatomic.h`); every fixture in the
test suite also compiles with `-ffreestanding -nostdinc`.

The C compiler is a pinned toolchain, the same on every machine: clang,
lld, `llvm-ar`, `llvm-dlltool` and compiler-rt from LLVM 23.1.2, built
for the X86, AArch64 and WebAssembly targets and published per host
(macOS arm64 and x86_64, Linux x86_64 and arm64, Windows x86_64) as the
`toolchain-23.1.2-1` release. `ironc` looks for it at `$IRON_TOOLCHAIN`,
then next to itself in `<prefix>/lib/iron/toolchain/`, then in
`~/.iron/toolchain/23.1.2-1/`, where it downloads it on first use and
verifies its SHA-256. It never uses a compiler from `PATH`, refuses a
bundle built from another LLVM release and reports the one it uses in
`iron --version`; `iron toolchain info`, `iron toolchain path` and
`iron toolchain install` expose the same lookup. `IRON_TOOLCHAIN` is a
developer override: a mismatched version there only warns.

Native targets today are the host: `aarch64-apple-darwin` and
`x86_64-apple-darwin` against libSystem, `x86_64-linux-gnu` and
`aarch64-linux-gnu` against glibc, and `x86_64-pc-windows-msvc` against
the Universal C Runtime, linked with `clang-cl`. The runtime and standard
library C sources are compiled together with the program for now; the
precompiled per-target runtime, redistributable link inputs and
`iron build --target=<os>-<arch>` are not yet implemented (section 12).
Until then a build also needs the platform's C library headers and link
inputs: the Visual Studio Build Tools on Windows, the Xcode command line
tools on macOS, the C library development package on Linux. When they are
missing `ironc` names them and offers to run the installer;
`iron toolchain check` probes for them explicitly.

---

## 11. Diagnostics

The compiler reports errors as `error[E0nnn]` and warnings as
`warning[W0nnn]`, each with the source location and usually a hint. The
codes cited in this manual:

| Code | Meaning |
|---|---|
| E0001 to E0005 | lexical errors: unterminated string, invalid character, invalid number, string too long |
| E0101, E0102 | unexpected token, expected expression |
| E0175, E0176 | keyword used as a binding name; field without `val` or `var` |
| E0200, E0201 | undefined identifier; duplicate declaration |
| E0202, E0215, E0216, E0217, E0218 | type mismatch; return type; argument count; argument type; not callable |
| E0203, E0234, E0235, E0266 | reassigning a `val`; writing a field of a `val`; mutating call on a `val`; writing a parameter |
| E0204 | using a nullable value without a null check |
| E0205, E0206 | missing interface method; unsatisfied generic constraint |
| E0207, E0212, E0213, E0214, E0274, E0275 | heap value escapes; `free`/`leak` of a non-heap value; `leak` of an `rc`; `free`/`leak` of a non-binding |
| E0208 | write to an outer binding inside `parallel` |
| E0209 | module not found |
| E0210 | `self` outside a method |
| E0219, E0220 | no such field; no such method |
| E0222 | mixing `Int` and `Float` |
| E0224, E0225, E0226, E0227, E0228 | non-exhaustive match; pattern arity; unreachable arm; pattern binding shadows a name; unknown variant |
| E0229 | empty list literal without a type |
| E0230, E0231, E0232 | comptime step limit; unsupported comptime construct; comptime error |
| E0233 | bitwise operator on a non-integer |
| E0237 | method name reserved by a `pub` field accessor |
| E0238 to E0245 | method tier violations (`readonly` writes, `pure` I/O and calls, modifier placement) |
| E0246 to E0252 | `init` rules (read before assign, unassigned field, double assign, method on partial `self`, early return, delegation, return value) |
| E0253, E0254, E0255 | patch adds a field; patch target not found; patch redefines a method |
| E0256, E0257 | `init` in an interface; tier mismatch with the interface |
| E0262, E0264 | inline field default; `var` fields without an `init` |
| E0268, E0270, E0271, E0294, E0295, E0296 | pointer arithmetic; address of a temporary; escaping stack reference; regime errors; `&` on an `rc` |
| E0273, E0274, E0297, E0298 | `heap`/`rc`/`pool` in an invalid position |
| E0278 | I/O in a `readonly` method |
| E0284 to E0288 | duplicate `drop`/`copy`; copy of a `nocopy` value; `readonly drop`; early return in `drop` |
| E0289 | checked/unchecked pointer mismatch |
| E0293 | missing return |
| E0299, E0301 | dereferencing a `weak rc`; `rc` inside an arena block |
| E0310, E0311, E0312 | invalid cast; constant does not fit; constant index out of range |
| E0314 | possibly uninitialized `var` |
| E0320, E0321 | private declaration used from another file; standalone `func Type.method` form |
| E0322, E0323, E0324, E0325, E0326 | unsupported `is`; unsupported match subject; lambda parameter type; awaited twice; thread pools |
| E0328, E0329, E0330 | implicit list copy or capture; indexing an unordered list; address of a growable list element |
| E0331, E0332, E0333, E0334 | list extension with a body; refutable nested pattern; `Channel.new(4)` and the other `.new` constructor spellings; interpolating a value with no text form |
| E0501 | `await` on the web target |
| E0700 to E0703 | web main loop rules |
| W0601, W0604, W0605, W0606 | narrowing cast; spawn data race; arena skips `drop`; heap value never freed |
| W0611, W0613, W0614 | unused import; `var` never reassigned; `var` parameter never reassigned |

`docs/dev/diagnostic-codes.md` lists every code with its message.

The following programs are accepted by the compiler but do not compile to
valid C or misbehave at run time in the current release; the manual does not
document them as features:

- slicing a list (`xs[a..b]`),
- `s += t` on strings (write `s = s + t`),
- ordering comparisons of strings (`"a" < "b"`),
- a named top-level function used as a value (`val f = twice`,
  `apply(twice, 4)`, `[twice]`; wrap it in a lambda),
- an object that has both an `init` and a `copy` block,
- an object with a field of its own nullable type (`var next: Node?`),
- `-> Self` in an interface method signature,
- `Ptr.offset` and `Ptr.diff`,
- nested `match` patterns (`A.X(B.Y(v))`) are not checked at run time,
- duplicate integer arms in a `match`.

---

## 12. Not yet implemented

Settled design decisions that the compiler does not implement yet, listed
so that older material is not mistaken for the current language:

- Thread pools: the `pool` keyword, `spawn("name", pool)` and `for ... parallel(pool)`.
- Reading and writing a primitive through a pointer (`*p`).
- Method-level generic inference for the container methods (`ch.recv()` without a written type).
- Lambda parameter inference outside a function-typed parameter position.
- `String`, `Bool` and `Float` subjects in `match`.
- Cross compilation: `iron build --target=<os>-<arch>` with a precompiled
  per-target runtime and link inputs that need no platform SDK (musl on
  Linux, import libraries and an Iron entry point on Windows, `.tbd` stubs
  on macOS).

---

## 13. Complete syntax of Iron

This section gives the syntax of Iron in extended BNF, one production per
construct, as implemented by the parser in `src/parser/parser.c`.
`{ x }` means zero or more repetitions of `x`, `[ x ]` means optional,
`( x | y )` groups alternatives and `'x'` is a literal token. `IDENT`,
`INT`, `FLOAT` and `STRING` are the lexical tokens of section 1; `STRING`
covers plain, multi-line and interpolated strings. `NAME` is an identifier
or one of the keywords allowed in name position. Newlines and comments are
not part of the grammar: the parser skips them between any two tokens (see
section 1.6). The grammar is checked against the fixture corpus by
`scripts/grammar_check.py`.

```ebnf
program        ::= { decl }

decl           ::= import_decl
                 | val_decl
                 | var_decl
                 | [ 'nocopy' ] [ 'pub' ] object_decl
                 | [ 'pub' ] ( func_decl | extern_decl | patch_decl | interface_decl | enum_decl | array_ext_decl )

import_decl    ::= 'import' IDENT { '.' IDENT }
func_decl      ::= [ '@' 'fusible' ] 'func' IDENT [ generic_params ] param_list [ '->' type ] block
extern_decl    ::= 'extern' 'func' IDENT param_list [ '->' type ]
array_ext_decl ::= 'func' '[' IDENT ']' '.' NAME [ generic_params ] param_list [ '->' type ] block
generic_params ::= '[' generic_param { ',' generic_param } [ ',' ] ']'
generic_param  ::= IDENT [ ':' IDENT ]
param_list     ::= '(' [ param { ',' param } [ ',' ] ] ')'
param          ::= [ 'val' | 'var' ] NAME [ ':' type ]

object_decl    ::= 'object' IDENT [ generic_params ] [ impl_clause ] '{' { member } '}'
impl_clause    ::= 'impl' IDENT { ',' IDENT }
member         ::= [ 'pub' ] [ 'readonly' | 'pure' ] ( field | method | init_decl | 'copy' block | 'drop' block )
field          ::= ( 'val' | 'var' ) IDENT ':' type
method         ::= 'func' NAME [ generic_params ] param_list [ '->' type ] block
init_decl      ::= 'init' [ IDENT ] param_list block

patch_decl     ::= 'patch' 'object' IDENT [ impl_clause ] '{' { patch_member } '}'
patch_member   ::= [ 'pub' ] [ 'readonly' | 'pure' ] ( method | init_decl | 'copy' block | 'drop' block )

interface_decl ::= 'interface' IDENT '{' { iface_method } '}'
iface_method   ::= [ 'readonly' | 'pure' ] 'func' IDENT param_list [ '->' type ] [ block ]

enum_decl      ::= 'enum' IDENT [ generic_params ] '{' [ variant { ',' variant } [ ',' ] ] '}'
variant        ::= IDENT [ '(' type { ',' type } [ ',' ] ')' | '=' INT ]

val_decl       ::= 'val' ( binding [ ':' type ] [ '=' init_expr ]
                         | '(' binding { ',' binding } [ ',' ] ')' [ ':' type ] '=' expr )
var_decl       ::= 'var' binding [ ':' type ] [ '=' init_expr ]
binding        ::= IDENT | '_'
init_expr      ::= spawn_expr | expr

block          ::= '{' { stmt } '}'
stmt           ::= val_decl | var_decl | return_stmt | if_stmt | while_stmt | for_stmt
                 | match_stmt | defer_stmt | free_stmt | leak_stmt | in_arena_stmt
                 | spawn_expr | block | expr_stmt
return_stmt    ::= 'return' [ expr ]
if_stmt        ::= 'if' expr block { 'elif' expr block } [ 'else' block ]
while_stmt     ::= 'while' expr block
for_stmt       ::= 'for' ( IDENT | '(' IDENT ',' IDENT ')' ) 'in' expr
                   [ 'parallel' [ '(' expr ')' ] ] block
match_stmt     ::= 'match' expr '{' { match_arm } [ 'else' '->' arm_body ] '}'
match_arm      ::= pattern '->' arm_body
arm_body       ::= block | stmt
pattern        ::= [ IDENT '.' ] IDENT [ '(' [ sub_pattern { ',' sub_pattern } [ ',' ] ] ')' ]
                 | expr
sub_pattern    ::= '_' | IDENT '.' IDENT [ '(' [ sub_pattern { ',' sub_pattern } [ ',' ] ] ')' ] | IDENT
defer_stmt     ::= 'defer' ( 'free' expr | block | stmt )
free_stmt      ::= 'free' expr
leak_stmt      ::= 'leak' expr
in_arena_stmt  ::= 'in' expr block
spawn_expr     ::= 'spawn' '(' STRING [ ',' expr ] ')' block
expr_stmt      ::= expr [ assign_op expr ]
assign_op      ::= '=' | '+=' | '-=' | '*=' | '/=' | '<<=' | '>>=' | '&=' | '|=' | '^='

expr           ::= unary { binary_op unary | 'is' IDENT }
binary_op      ::= 'or' | 'and' | '|' | '^' | '&' | '==' | '!=' | '<' | '>' | '<=' | '>='
                 | '<<' | '>>' | '+' | '-' | '*' | '/' | '%'
unary          ::= '-' unary | 'not' unary | '~' unary | '&' unary
                 | 'heap' [ heap_opts ] unary | 'rc' unary | 'weak' 'rc' ( 'null' | unary )
                 | 'comptime' unary | 'await' unary | postfix
heap_opts      ::= '(' heap_opt { ',' heap_opt } ')'
heap_opt       ::= 'in' ':' expr | 'allow_drop_skip' ':' ( 'true' | 'false' )
postfix        ::= primary { '.' NAME [ type_args ] [ call_args ]
                           | '[' expr [ '..' [ expr ] ] ']'
                           | '[' type ',' type { ',' type } ']'
                           | call_args }
call_args      ::= '(' [ expr { ',' expr } [ ',' ] ] ')'
type_args      ::= '[' type { ',' type } ']'
primary        ::= INT | FLOAT | STRING | 'true' | 'false' | 'null' | IDENT | 'self'
                 | '(' expr ')'
                 | '(' expr ',' expr { ',' expr } [ ',' ] ')'
                 | '[' type ';' expr ']'
                 | '[' [ expr { ',' expr } [ ',' ] ] ']'
                 | lambda
lambda         ::= 'func' param_list [ '->' type ] block

type           ::= 'weak' 'rc' type
                 | 'rc' type
                 | [ '?' ] ptr_type
                 | tuple_type
                 | list_type
                 | func_type
                 | named_type
ptr_type       ::= '*' [ 'var' ] [ 'unchecked' ] type
tuple_type     ::= '(' type ',' type { ',' type } ')'
list_type      ::= '[' elem_type { ',' list_attr } [ ';' [ '<=' ] expr ] ']'
elem_type      ::= func_type | 'rc' type | 'weak' 'rc' type | list_type | ptr_type | tuple_type | IDENT
list_attr      ::= 'layout' ':' ( 'soa' | 'aos' ) | 'unordered'
func_type      ::= 'func' [ '(' [ type { ',' type } ] ')' ] [ '->' type ]
named_type     ::= IDENT '?' [ type_args ] | IDENT [ type_args ] [ '?' ]

NAME           ::= IDENT | 'init' | 'copy' | 'drop' | 'null' | 'free'
```

Operator precedence is given in section 3.1: `expr` is parsed by
precedence climbing over the flat sequence of `unary` operands and
operators, with every binary operator left associative and `is` binding
loosest of all. In `postfix`, `X.Y(args)` and `X.Y` where `X` and `Y` both
start with an uppercase letter denote an enum variant construction, and
`x.m[T](args)` is only read as a generic method call when the token after
`[` is a type name starting with an uppercase letter or `[`. In `pattern`,
the first alternative is used when the arm starts with `IDENT '.'` or with
an uppercase identifier followed by `(`, and `expr` otherwise. The
standalone forms `func Type.method()` and `func (r: T) method()` are
recognized only to report `E0321` and `E0260`; `array_ext_decl` is used by
the standard library and cannot be implemented in user code.
