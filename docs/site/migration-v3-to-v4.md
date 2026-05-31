# Migrating from Iron v3 to Iron v4

Iron v4 is a memory-model overhaul. Most syntactic constructs from v3 still compile, but binding tiers (`val` / `var`), pointer types (`*T` / `*var T`), resource discipline (`drop` / `copy` / `nocopy`), reference counting (`rc` / `weak rc`), and arena scoping are now first-class type-level concerns. This guide walks through every surface change with before/after code blocks. There is **no automatic codemod** for this migration; the LSP ships 5 quickfixes that cover the most common mechanical changes.

This guide is companion to the [v4.0.0-alpha.1 release notes](../release/v4.0.0-alpha.1.md).

## §1 Why v4

Iron v3 had implicit mutation and unbounded ownership: a `let` binding could be re-bound, a pointer could mutate at any call site, and shared ownership was a runtime concern with no syntactic trace. v4 promotes all three to the type level. Every binding declares its tier (`val` / `var`), every pointer declares its mutability (`*T` / `*var T`), and every shared-ownership relationship picks a policy (`heap` / `rc` / `weak rc` / `arena` / `Box[T]`). The cost: every `let x =` in v3 becomes `val x =` or `var x =` in v4. The benefit: a 4-tier escape analyzer (Phase 30) elides the runtime check entirely in over 90% of dereferences, so the explicit typing pays for itself with negligible runtime overhead.

What follows is mechanical for most code; structural for code that relied on implicit shared mutation.

## §2 At a glance — v3 → v4 syntax delta

| v3 syntax                          | v4 syntax                                                   | What changed                                                          |
| ---------------------------------- | ----------------------------------------------------------- | --------------------------------------------------------------------- |
| `let x = 1`                        | `val x = 1` or `var x = 1`                                  | Binding tier is now explicit (§3.1)                                   |
| `let mut x = 1`                    | `var x = 1`                                                 | `mut` keyword removed; `var` is the mutable tier (§3.1)               |
| `func f(p: Point)`                 | `func f(p: Point)` or `func f(var p: Point)`                | Param mutation requires `var` modifier (§3.2)                         |
| `func f(p: *Point)`                | `func f(p: *Point)` (read-only)                             | Default pointer is read-only (§3.3)                                   |
| `func f(p: *Point)` *(mutating)*   | `func f(p: *var Point)`                                     | Pointer mutability is part of the type (§3.3)                         |
| `Box.new(x)`                       | `heap x` + `defer free x` *or* `Box[T] { x }`               | Allocation policy is explicit (§3.4, §3.8)                            |
| (no explicit free)                 | `free x` (or `defer free x`)                                | Owned heap allocations must be released (§3.4)                        |
| `readonly func ...` *(advisory)*   | `readonly func ...` *(enforced)*                            | `readonly` is now structurally enforced via E0820 (§3.5)              |
| `[1, 2, 3]: [Int]`                 | `[1, 2, 3]: [Int; <=8]` *(if bounded)*                      | Bounded vectors get capacity in the type (§3.6)                       |
| `object Foo { ... }`               | `object Foo { ... copy { ... } }`                           | Opt-in `copy` for deep-copy semantics (§3.7)                          |
| `object Foo { ... }` *(resource)*  | `nocopy object Foo { ... drop { ... } }`                    | Resource types must be `nocopy` + carry `drop` (§3.7)                 |
| `extern func c(p: void*)`          | `extern func c(p: *unchecked U8)`                           | FFI pointers use the explicit unchecked regime (§3.8)                 |
| `let owned = Box.new(x)`           | `val owned = Box.new(x)`                                    | `Box[T]` survives; v3 implicit shared ownership becomes `rc x` (§3.8) |
| *(implicit shared ownership)*      | `val r = rc x`                                              | Shared ownership opts into reference counting (§3.9)                  |
| *(cyclic shared references leak)*  | `val back = weak rc null` + `node.demote()`                 | Cycles broken by `weak rc` (§3.10)                                    |
| *(many small allocations)*         | `arena { val x = heap(in: a) Foo() }`                       | Bump-allocator scope (§3.11)                                          |
| `try { ... } finally { close(f) }` | `defer close(f); ...`                                       | LIFO scope-exit hook (§3.12)                                          |
| `Mutex` *(manual lock/unlock)*     | `var m = Mutex.new(0); val g = m.lock()`                    | Phase 33 stdlib containers are `nocopy` + auto-drop (§3.13)           |
| `File.open(...)` *(manual close)*  | `var fh = FileHandle.open(...)` *(drop closes fd)*          | Same — `nocopy` + auto-drop (§3.13)                                   |

> **If you only read one section:** that table above is the cheat-sheet. The rest of §3 is one subsection per row with the worked before/after.

## §3 Per-feature migration

### §3.1 Binding tier: `val` / `var` (val/var) (Phase 17)

Every binding (local let-binding, struct field, function parameter, closure capture) opts into an immutable tier (`val`) or a mutable tier (`var`). The previous `let` and `let mut` forms are removed.

**v3:**

```iron
let x = 1
let mut counter = 0
counter = counter + 1

object Point {
    let mut x: Int
    let y: Int
}
```

**v4:**

```iron
val x = 1
var counter = 0
counter = counter + 1

object Point {
    var x: Int
    val y: Int
}
```

**Closure captures** follow the same rule — a closure capturing a mutable binding must capture it as `var` (or take a `*var T` pointer):

```iron
func make_counter() -> func() -> Int {
    var n = 0
    return func() -> Int {
        n += 1
        return n
    }
}
```

**LSP quickfix (LSP-06): Add 'val'.** Whenever a binding declaration appears without a tier — typically `let x = ...` carried over from v3 — the LSP surfaces a quickfix that prefixes `val`. If the variable is later reassigned, the analyzer's tier-mismatch diagnostic upgrades the fix to `var`.

### §3.2 Parameter modifiers (Phase 18)

Function and method parameters that rebind their binding or mutate through a pointer carry an explicit `var` modifier. By-value parameters default to `val` (binding cannot be rebound inside the body).

**v3:**

```iron
func tally(n: Int, p: *Counter) {
    n = n + 1
    p.count = p.count + n
}
```

**v4:**

```iron
func tally(var n: Int, p: *var Counter) {
    n = n + 1
    p.count = p.count + n
}
```

The first parameter is rebound inside the body (`n = n + 1` is a binding reassignment) so it needs `var`. The second mutates through the pointer, so the pointer type itself becomes `*var Counter` (see §3.3).

**LSP quickfix (LSP-08): Drop 'var' modifier.** When a parameter is declared `var` but the body never rebinds or mutates it, the LSP surfaces a fix that strips the redundant `var` — the codemod is conservative; tightening drives down the noise floor on real diffs.

### §3.3 Checked pointer types `*T` / `*var T` (Phase 20)

Pointer mutability is part of the type. `*T` is a read-only pointer; `*var T` allows writes through the pointer. Both carry generation checks (Phase 19) so use-after-free is caught at every dereference at runtime.

**v3:**

```iron
func read(p: *Player) -> Int {
    return p.hp
}

func damage(p: *Player, amount: Int) {
    p.hp = p.hp - amount
}
```

**v4:**

```iron
func read(p: *Player) -> Int {
    return p.hp
}

func damage(p: *var Player, amount: Int) {
    p.hp = p.hp - amount
}
```

The type-system rules:

- `*T` may be passed where `*T` is expected (identity).
- `*var T` may be passed where `*T` is expected (variance: writeable is a stronger promise than read-only).
- `*T` may **not** be passed where `*var T` is expected (typecheck error — the callee would mutate through a read-only handle).

Dereferencing through `*T` after the underlying allocation has been `free`'d is a runtime error (the generation guard), not a compile-time error — see §3.4 for the `defer free` pattern that prevents the issue at the source.

### §3.4 `heap` allocation + `free` (Phase 21)

Explicit `heap` policy returns a fat pointer with a generation header; `free` releases it. v3's implicit shared ownership is replaced by `heap T` for single-owner heap allocations, `rc T` for shared ownership (§3.9), and `weak rc T` for non-owning observers (§3.10).

**v3:**

```iron
func make_world() -> World {
    return World(seed: 42, size: 1024)
}
```

**v4:**

```iron
func main() {
    val world = heap World(seed: 42, size: 1024)
    defer free world
    println("seed: {world.seed}")
}
```

The canonical pattern is `heap T(...) + defer free`. The Phase 31 debug allocator reports a leak when a `heap` allocation is reachable at scope exit with no matching `free`.

**LSP quickfix (LSP-07): Insert 'defer free \<binding\>'.** When a `heap` binding has no matching `free` in the enclosing function body, the LSP surfaces a quickfix that inserts `defer free <binding>` immediately after the declaration. The snippet is also offered as a completion when the cursor lands on a fresh empty line below a `heap` binding (the Phase 34 LSP-04 surface).

### §3.5 `readonly` purity tightening (Phase 22)

`readonly` methods structurally cannot mutate `self` or anything transitively reachable from `self`. v3 `readonly` was advisory; v4 `readonly` is enforced by the analyzer with diagnostic `E0820 IRON_ERR_READONLY_MEMORY`.

**v3 (would compile but silently accept the mutation):**

```iron
object Buffer {
    var data: [Int; <=64]

    readonly func sample(p: *var Buffer) -> Int {
        p.data.push(0)         -- v3 advisory: no error
        return p.data.len()
    }
}
```

**v4 (analyzer rejects with E0820):**

```iron
object Buffer {
    var data: [Int; <=64]

    readonly func sample(self: *Buffer) -> Int {
        return self.data.len()   -- read-only is enforced
    }

    func sample_and_grow(self: *var Buffer) -> Int {
        self.data.push(0)
        return self.data.len()
    }
}
```

The fix is structural — extract the mutating block into a non-readonly helper. `pure` is a strict subset of `readonly` (see Phase 22 closeout); promoting a `readonly` method to `pure` further forbids any I/O.

**LSP quickfix (LSP-10): Remove 'readonly' + Extract mutating block into helper.** Two actions: the first strips the `readonly` modifier so the function compiles as-is; the second is an alpha-placeholder that surfaces a code-action title for the structural extraction (the actual refactor is a no-op in alpha — manual extraction is the workaround).

### §3.6 Bounded vector `[T; <=N]` (Phase 23)

Fixed-capacity stack vector. The capacity `N` is part of the type; pushes beyond it are a typecheck error. Use them anywhere "small, known-bounded sequence" is the pattern (function argument lists, parser-state stacks, per-frame draw commands).

**v3:**

```iron
func sample() {
    var xs: [Int] = []
    xs.push(10)
    xs.push(20)
    xs.push(30)
    println("{xs.len()}")
}
```

**v4:**

```iron
func sample() {
    var xs: [Int; <=4]
    xs.push(10)
    xs.push(20)
    xs.push(30)
    println("{xs.len()}")
}
```

**Typecheck fail when over-pushing a known-constant count:**

```iron
var xs: [Int; <=2]
xs.push(1)
xs.push(2)
xs.push(3)        -- E0202: bvec capacity exceeded
```

**When to choose:** if you have a known upper bound, prefer `[T; <=N]` over `[T]`. The vector lives inline on the stack — zero heap traffic — and the bound is checked statically when the count is known.

### §3.7 Resource types: `drop` / `copy` / `nocopy` (Phase 24)

Object declarations opt into resource discipline:

- **`drop`** declares a destructor that runs at scope exit (LIFO with `defer`).
- **`copy`** declares deep-copy semantics — assignments duplicate the object.
- **`nocopy`** forbids implicit duplication — the object is move-only; aliasing is rejected by the analyzer.

**v3 (implicit deep-copy for some types, ad-hoc cleanup):**

```iron
object Logger {
    let path: String
    let fd: Int
}

func main() {
    let log = Logger.open("/tmp/log.txt")
    write(log, "hello")
    close(log)
}
```

**v4 (`nocopy` + `drop` makes cleanup automatic):**

```iron
nocopy object Logger {
    val path: String
    val fd: Int

    drop {
        close(self.fd)
    }
}

func main() {
    val log = Logger.open("/tmp/log.txt")
    log.write("hello")
    -- scope exit: Logger.drop runs, fd closed
}
```

The Phase 33 stdlib containers (`Mutex[T]`, `RWLock[T]`, `Channel[T]`, `FileHandle`) are all `nocopy`. See §3.13 for the receiver-method surface.

### §3.8 `*unchecked T` + `Box[T]` (Phase 25)

`*unchecked T` is the escape hatch for FFI and zero-overhead interop — no generation guard, no mutability tracking, raw C-style pointer. `Box[T]` is a single-owner checked heap box: useful when you want to hand off owned heap memory to a caller without exposing the lifetime to the type system.

**FFI via `*unchecked T`:**

```iron
extern func c_strlen(s: *unchecked U8) -> Int

func main() {
    val message: [U8; 14] = "Hello, Iron!\0"
    val raw: *unchecked U8 = message as *unchecked U8
    val length = c_strlen(raw)
    println("length: {length}")
}
```

**Owned heap value via `Box[T]`:**

```iron
func make_config() -> Box[Config] {
    return Box.new(Config(width: 1920, height: 1080, title: "Iron App"))
}

func main() {
    val boxed = make_config()
    val cfg: *unchecked Config = boxed.unwrap()
    println("config: {cfg.width}x{cfg.height}")
}
```

**Safety contract:** `*unchecked T` is the **only** surface that opts out of memory safety. Use it at FFI boundaries and nowhere else. Casting `*T → *unchecked T` is allowed (loses safety); the inverse cast is also allowed but the regime is the caller's responsibility. The LSP highlights `*unchecked` differently from the rest of the pointer surface to reinforce the boundary.

### §3.9 `rc` policy (Phase 26)

`rc x` heap-allocates with a 24-byte header: atomic refcount + drop-fn + weak-count. Cloning the binding increments the refcount; each drop decrements; the underlying value is dropped when both `refcount` and `weak_count` reach zero (so observers via `weak rc` keep the header alive but not the payload — see §3.10).

**v3 (implicit shared ownership):**

```iron
func main() {
    let sprite = Texture("hero.png")
    let also  = sprite      -- v3: silently shared
    use(sprite)
    use(also)
}
```

**v4 (explicit `rc`):**

```iron
object Texture {
    val path: String
    drop { println("free: {path}") }
}

func main() {
    val sprite = rc Texture(path: "hero.png")
    val also   = sprite     -- refcount bumped to 2
    println("loaded: {sprite.path}")
    -- scope exit: both drop, refcount → 0, Texture.drop runs once
}
```

**Phase 29 atomic-elision optimizer:** local `rc` allocations that the escape analyzer proves never cross a thread boundary lower to non-atomic refcount ops. No source change is required to get the elision — write the idiomatic `rc T` and the optimizer picks it up.

### §3.10 `weak rc` policy (Phase 27)

A non-owning observer of an `rc T`. `weak rc T` does not keep the value alive; `upgrade()` returns `T?` (None if the underlying value has already been dropped). Use it to break refcount cycles.

**v3 (cyclic `rc`-like pattern leaks):**

```iron
object NodeA {
    let next: NodeA       -- v3: implicit alias; cycle leaks
}
```

**v4 (cycle broken by `weak rc`):**

```iron
object NodeB {
    val id: Int
    drop { println("drop: B-{id}") }
}

object NodeA {
    val id: Int
    val b_link: rc NodeB
    var back: weak rc NodeA

    drop { println("drop: A-{id}") }
}

func main() {
    val node_a = rc NodeA(id: 1, b_link: rc NodeB(id: 2), back: weak rc null)
    node_a.back = node_a.demote()
    println("cycle set up")
    -- scope exit: rc NodeA refcount → 0; weak_count via `back` does not keep
    -- payload alive; NodeA.drop runs; then NodeB.drop runs (b_link → 0).
}
```

The pattern is: owner field carries `rc T`; back-reference field carries `weak rc T`; the back-reference is set via `demote()` after the owner exists.

### §3.11 Arena allocation (Phase 28)

Explicit `Arena.with_capacity(N)` scopes a bump allocator. Allocations via `heap(in: arena) T(...)` go into the arena; when the arena binding leaves scope (or is explicitly reset), all allocations release at once.

> **Restriction:** `rc` and `weak rc` allocations are **not** allowed inside an arena — the drop-ordering invariant required by atomic refcount release is incompatible with bulk-reset semantics.

**v3 (many small heap allocations, manual cleanup):**

```iron
func render_frame() {
    let cmd1 = DrawCmd(id: 1)
    let cmd2 = DrawCmd(id: 2)
    let cmd3 = DrawCmd(id: 3)
    -- N draw cmds, all freed implicitly at scope exit
}
```

**v4 (arena-scoped, single bulk release):**

```iron
object DrawCmd {
    val id: Int
    drop { println("drop cmd: {id}") }
}

func main() {
    val frame_arena = Arena.with_capacity(65536)
    val cmd1 = heap(in: frame_arena) DrawCmd(id: 1)
    val cmd2 = heap(in: frame_arena) DrawCmd(id: 2)
    val cmd3 = heap(in: frame_arena) DrawCmd(id: 3)
    println("cmds: {cmd1.id}, {cmd2.id}, {cmd3.id}")
    -- scope exit: all 3 drops run, then arena releases all 3 chunks at once
}
```

**When to choose:** ETL pipelines, parser-state allocations, per-frame game state, per-request server allocations — anywhere "many small allocations, one lifetime" is the pattern.

### §3.12 `defer` statement (Phase 32)

LIFO scope-exit hook. Multiple `defer` statements run in reverse declaration order. At scope exit, the unified unwind sequence runs all `defer` statements (LIFO) **first**, then local destructors in reverse declaration order.

**v3 (manual cleanup):**

```iron
func process() {
    let f = open("/tmp/data")
    -- ... work that may early-return ...
    close(f)
}
```

**v4 (`defer` makes the cleanup unmissable):**

```iron
func main() {
    var fh = FileHandle.open("/tmp/data")
    defer fh.close()
    -- ... work that may early-return ...
    println("done")
}
```

**Ordering example** (two `defer` + two droppable locals):

```iron
object Resource {
    val name: String
    drop { println("drop{self.name}") }
}

func main() {
    val local1 = Resource("1")
    val local2 = Resource("2")
    defer println("defer1")
    defer println("defer2")
    println("body")
}
-- output:
-- body
-- defer2
-- defer1
-- drop2
-- drop1
```

`defer` pairs naturally with `heap` / `free` and resource types. The LSP-07 "Insert 'defer free \<binding\>'" quickfix is the canonical first use; see §3.4.

### §3.13 Stdlib container surface (Phase 33)

New types ship in v4: `Mutex[T]`, `RWLock[T]`, `Channel[T]` (bounded), `FileHandle`. All four are `nocopy`. All carry `drop` blocks that release the underlying resource at scope exit.

**Mutex with scoped guard:**

```iron
func main() {
    var m: Mutex[Int] = Mutex.new(0)
    {
        var guard = m.lock()
        guard.set(42)
        println("locked: {guard.get()}")
        -- scope exit: guard drop releases the mutex
    }
    println("released")
}
```

**Bounded channel:**

```iron
func main() {
    var ch: Channel[Int] = Channel.new(2)
    ch.send(10)
    ch.send(20)
    println("recv: {ch.recv()}")
    println("recv: {ch.recv()}")
}
```

**File handle (drop closes fd):**

```iron
func main() {
    {
        var fh = FileHandle.open("/tmp/iron_fh_drop.txt")
        println("opened")
        -- scope exit: FileHandle drop closes fd
    }
    println("done")
}
```

Receiver-form methods are universal: `mutex.lock()`, `rwlock.read_lock()`, `channel.send(x)`, `file.read()`, etc. See [`docs/dev/STDLIB-CONTAINERS.md`](../dev/STDLIB-CONTAINERS.md) for the full method roster.

## §4 Policy-promotion guidance

Decision tree for choosing the right policy. Start at the top, follow the first match.

- **Single owner, known scope** → stack binding (`val x = ...` or `var x = ...`). No allocation, no drop ceremony, fastest path.
- **Single owner, dynamic lifetime, dies at scope exit** → `heap x` + `defer free x`. The Phase 31 debug allocator catches the leak if you forget the `free`.
- **Single owner, dynamic lifetime, hand-off to caller** → `Box[T]`. The caller takes ownership via `.unwrap()`.
- **Shared owners, no cycles** → `rc x`. Clones are atomic refcount increments; Phase 29 elides the atomic when the escape analyzer proves no thread crossing.
- **Shared owners with back-references** → `rc x` for the owner, `weak rc x` for the back-edge. Breaks cycles; `upgrade()` returns `T?`.
- **Batch of short-lived allocations with one lifetime** → `arena { val x = heap(in: a) Foo() }`. One bulk release at arena exit.
- **FFI / raw pointer interop** → `*unchecked T`. Confine to the FFI shim.
- **`*T` vs `*var T`** → start with `*T`. Promote to `*var T` only when the callee provably needs to write through the pointer. The LSP-08 quickfix narrows over-tight `var` automatically.
- **`copy` vs `nocopy`** → `nocopy` for resource types (file handles, locks, sockets, mutex guards, network connections). `copy` for value types with deep-copy semantics. The default (neither) is move-only.
- **Arena vs rc** → arena when the lifetime is structural (one bump-frame fits all allocations). `rc` when ownership is shared dynamically across the lifetime axis.

**Composition rules (Phase 8 composition tests pin these):**

| Container               | Stack | `heap` | `rc` | `weak rc` | `Box[T]` | `arena` |
| ----------------------- | :---: | :----: | :--: | :-------: | :------: | :-----: |
| Plain object            |   Y   |   Y    |  Y   |    Y      |    Y     |    Y    |
| `nocopy` object         |   Y   |   Y    |  Y   |    Y      |    Y     |    Y    |
| `rc` field inside `rc`  |   —   |   —    |  Y   |    Y      |    —     |    N    |
| `rc` field inside arena |   —   |   —    |  —   |    —      |    —     |    N    |

(N = rejected by analyzer; — = not meaningful.)
